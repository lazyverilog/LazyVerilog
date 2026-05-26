#include "formatter.hpp"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <slang/parsing/Lexer.h>
#include <slang/parsing/LexerFacts.h>
#include <slang/parsing/Token.h>
#include <slang/syntax/SyntaxKind.h>
#include <slang/text/SourceManager.h>
#include <slang/util/BumpAllocator.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace slang;
using namespace slang::parsing;

namespace {

static int snap_to_indent_grid(int value, int indent_size) {
    if (indent_size <= 0)
        return value;
    return ((value + indent_size - 1) / indent_size) * indent_size;
}

static int tab_aligned_width(int value, const FormatOptions& opts) {
    return opts.tab_align ? snap_to_indent_grid(value, opts.indent_size) : value;
}

// ---------------------------------------------------------------------------
// Line vector helpers
// ---------------------------------------------------------------------------

static std::string render_lines(const std::vector<std::string>& lines) {
    std::string r;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k)
            r += '\n';
        r += lines[k];
    }
    return r;
}

static void write_log(const FormatOptions& opts, const std::string& filename,
                            const std::string& text) {
    if (opts.log_path.empty())
        return;
    std::filesystem::path dir(opts.log_path);
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / filename);
    out << text;
}

using slang::parsing::TokenKind;

struct TokLexeme {
    TokenKind kind{TokenKind::Unknown};
    std::string text;
    std::string lo;
    int pos{0};
    bool whitespace{false};
    bool comment{false};
    bool directive{false};
    bool define_block{false};
    syntax::SyntaxKind directive_kind{syntax::SyntaxKind::Unknown};
};

struct Tok {
    // Lexical identity is immutable after token collection. Formatting passes may
    // only update fmt_* metadata; token text itself is never rewritten.
    std::shared_ptr<const TokLexeme> lex;
    TokenKind kind{TokenKind::Unknown};

    // --- Mutable formatting metadata (set/updated by passes) ---
    int fmt_indent{0};            // indentation level at this token
    int fmt_spaces_before{0};     // spaces before this token (within a line)
    bool fmt_newline_before{false}; // emit newline before this token
    int fmt_blank_lines{0};       // blank lines before this token (after newline)
    bool fmt_disabled{false};     // inside verilog_format:off region (verbatim)
    bool fmt_passthrough{false};  // whitespace-sensitive macro (verbatim)
};

static const std::string& tok_text(const Tok& tok) {
    return tok.lex->text;
}
static const std::string& tok_lex_text(const Tok& tok) { return tok.lex->text; }
static const std::string& tok_lo(const Tok& tok) { return tok.lex->lo; }
static int tok_pos(const Tok& tok) { return tok.lex->pos; }
static bool tok_whitespace(const Tok& tok) { return tok.lex->whitespace; }
static bool tok_comment(const Tok& tok) { return tok.lex->comment; }
static bool tok_directive(const Tok& tok) { return tok.lex->directive; }
static bool tok_define_block(const Tok& tok) { return tok.lex->define_block; }
static syntax::SyntaxKind tok_directive_kind(const Tok& tok) { return tok.lex->directive_kind; }

static std::vector<Tok> collect_lexer_tokens(const std::string& source);
static std::string render_tokens(const std::vector<Tok>& tokens, const FormatOptions& opts);
static void align_port_pass(std::vector<Tok>& tokens, const FormatOptions& opts);

static std::vector<size_t> tok_line_starts(const std::vector<Tok>& tokens);

static bool is_pp_cond_with(syntax::SyntaxKind kind);
static bool is_pp_conditional(syntax::SyntaxKind kind);

static std::vector<int> line_start_offsets(const std::vector<std::string>& lines) {
    std::vector<int> starts;
    starts.reserve(lines.size());
    int pos = 0;
    for (const auto& line : lines) {
        starts.push_back(pos);
        pos += (int)line.size() + 1;
    }
    return starts;
}

struct DirectiveMatch {
    syntax::SyntaxKind kind{syntax::SyntaxKind::Unknown};
    size_t start{std::string::npos};
    size_t end{std::string::npos};
};

struct DirectiveIndex {
    std::unordered_map<size_t, DirectiveMatch> by_start;

    explicit DirectiveIndex(const std::string& text) {
        for (const auto& tok : collect_lexer_tokens(text)) {
            if (tok.kind != TokenKind::Directive)
                continue;
            size_t start = (size_t)tok_pos(tok);
            by_start.emplace(start, DirectiveMatch{tok_directive_kind(tok), start,
                                                   start + tok_text(tok).size()});
        }
    }

    DirectiveMatch at(size_t offset) const {
        auto it = by_start.find(offset);
        if (it == by_start.end())
            return {};
        return it->second;
    }
};

static DirectiveMatch directive_at_offset(const std::string& text, size_t offset) {
    if (offset >= text.size() || text[offset] != '`')
        return {};
    size_t line_end = text.find('\n', offset);
    if (line_end == std::string::npos)
        line_end = text.size();
    std::string line = text.substr(offset, line_end - offset);
    for (const auto& tok : collect_lexer_tokens(line)) {
        if (tok_whitespace(tok) || tok_comment(tok))
            continue;
        if (tok.kind != TokenKind::Directive)
            return {};
        return {tok_directive_kind(tok), offset + (size_t)tok_pos(tok),
                offset + (size_t)tok_pos(tok) + tok_text(tok).size()};
    }
    return {};
}

static DirectiveMatch first_directive_on_line(const std::string& text, size_t line_start,
                                              size_t line_end) {
    size_t first = line_start;
    while (first < line_end && (text[first] == ' ' || text[first] == '\t'))
        ++first;
    if (first >= line_end)
        return {};
    return directive_at_offset(text, first);
}

static DirectiveMatch first_directive_on_line(const std::string& text, size_t line_start,
                                              size_t line_end,
                                              const DirectiveIndex& directives) {
    size_t first = line_start;
    while (first < line_end && (text[first] == ' ' || text[first] == '\t'))
        ++first;
    if (first >= line_end)
        return {};
    return directives.at(first);
}

// ---------------------------------------------------------------------------
// Formatting token helpers
// ---------------------------------------------------------------------------

using TokenCache = std::unordered_map<std::string, std::vector<Tok>>;
thread_local TokenCache* active_token_cache = nullptr;

struct ScopedTokenCache {
    TokenCache cache;
    TokenCache* prev{nullptr};

    ScopedTokenCache() : prev(active_token_cache) { active_token_cache = &cache; }
    ~ScopedTokenCache() { active_token_cache = prev; }
};

enum class SD { MustAppend, MustWrap, Preserve, Undecided };

static std::vector<Tok> significant_tokens(const std::string& text);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        c = (char)std::tolower((unsigned char)c);
    return r;
}

static bool has(const std::unordered_set<std::string>& s, const std::string& k) {
    return s.count(k) > 0;
}

static bool is_compiler_directive_name(const std::string& name) {
    syntax::SyntaxKind kind = directive_at_offset(name, 0).kind;
    return kind != syntax::SyntaxKind::Unknown && kind != syntax::SyntaxKind::MacroUsage &&
           !is_pp_conditional(kind);
}

static bool is_type_keyword(TokenKind kind) {
    switch (kind) {
        case TokenKind::LogicKeyword:
        case TokenKind::WireKeyword:
        case TokenKind::RegKeyword:
        case TokenKind::BitKeyword:
        case TokenKind::ByteKeyword:
        case TokenKind::ShortIntKeyword:
        case TokenKind::IntKeyword:
        case TokenKind::LongIntKeyword:
        case TokenKind::IntegerKeyword:
        case TokenKind::RealKeyword:
        case TokenKind::RealTimeKeyword:
        case TokenKind::ShortRealKeyword:
        case TokenKind::TimeKeyword:
        case TokenKind::StringKeyword:
        case TokenKind::CHandleKeyword:
        case TokenKind::EventKeyword:
        case TokenKind::VoidKeyword:
        case TokenKind::SignedKeyword:
        case TokenKind::UnsignedKeyword:
        case TokenKind::PackedKeyword:
            return true;
        default:
            return false;
    }
}

static bool is_indent_open(TokenKind kind) {
    switch (kind) {
        case TokenKind::ModuleKeyword:
        case TokenKind::MacromoduleKeyword:
        case TokenKind::InterfaceKeyword:
        case TokenKind::ProgramKeyword:
        case TokenKind::PackageKeyword:
        case TokenKind::ClassKeyword:
        case TokenKind::FunctionKeyword:
        case TokenKind::TaskKeyword:
        case TokenKind::BeginKeyword:
        case TokenKind::ForkKeyword:
        case TokenKind::CaseKeyword:
        case TokenKind::CaseXKeyword:
        case TokenKind::CaseZKeyword:
        case TokenKind::GenerateKeyword:
        case TokenKind::CoverGroupKeyword:
        case TokenKind::PropertyKeyword:
        case TokenKind::SequenceKeyword:
        case TokenKind::CheckerKeyword:
        case TokenKind::ClockingKeyword:
        case TokenKind::ConfigKeyword:
        case TokenKind::PrimitiveKeyword:
        case TokenKind::SpecifyKeyword:
            return true;
        default:
            return false;
    }
}

static bool is_block_open(TokenKind kind) {
    switch (kind) {
        case TokenKind::BeginKeyword:
        case TokenKind::ForkKeyword:
        case TokenKind::CaseKeyword:
        case TokenKind::CaseXKeyword:
        case TokenKind::CaseZKeyword:
        case TokenKind::GenerateKeyword:
        case TokenKind::CoverGroupKeyword:
        case TokenKind::PropertyKeyword:
        case TokenKind::SequenceKeyword:
        case TokenKind::CheckerKeyword:
        case TokenKind::ClockingKeyword:
        case TokenKind::ConfigKeyword:
        case TokenKind::PrimitiveKeyword:
        case TokenKind::SpecifyKeyword:
            return true;
        default:
            return false;
    }
}

static bool is_indent_close(TokenKind kind) {
    switch (kind) {
        case TokenKind::EndModuleKeyword:
        case TokenKind::EndInterfaceKeyword:
        case TokenKind::EndProgramKeyword:
        case TokenKind::EndPackageKeyword:
        case TokenKind::EndClassKeyword:
        case TokenKind::EndFunctionKeyword:
        case TokenKind::EndTaskKeyword:
        case TokenKind::EndKeyword:
        case TokenKind::JoinKeyword:
        case TokenKind::JoinAnyKeyword:
        case TokenKind::JoinNoneKeyword:
        case TokenKind::EndCaseKeyword:
        case TokenKind::EndGenerateKeyword:
        case TokenKind::EndGroupKeyword:
        case TokenKind::EndPropertyKeyword:
        case TokenKind::EndSequenceKeyword:
        case TokenKind::EndCheckerKeyword:
        case TokenKind::EndClockingKeyword:
        case TokenKind::EndConfigKeyword:
        case TokenKind::EndPrimitiveKeyword:
        case TokenKind::EndSpecifyKeyword:
        case TokenKind::EndTableKeyword:
            return true;
        default:
            return false;
    }
}

static bool is_pp_cond_with(syntax::SyntaxKind kind) {
    return kind == syntax::SyntaxKind::IfDefDirective ||
           kind == syntax::SyntaxKind::IfNDefDirective ||
           kind == syntax::SyntaxKind::ElsIfDirective;
}

static bool is_pp_cond_bare(syntax::SyntaxKind kind) {
    return kind == syntax::SyntaxKind::ElseDirective ||
           kind == syntax::SyntaxKind::EndIfDirective;
}

static bool is_pp_conditional(syntax::SyntaxKind kind) {
    return is_pp_cond_with(kind) || is_pp_cond_bare(kind);
}

static bool is_pp_conditional_text(const std::string& text) {
    return is_pp_conditional(directive_at_offset(text, 0).kind);
}

static bool is_macro_usage(const Tok& t) {
    return t.kind == TokenKind::MacroUsage ||
           (t.kind == TokenKind::Directive && tok_directive_kind(t) == syntax::SyntaxKind::MacroUsage);
}

enum class MacroRole {
    ObjectLikeExpr,
    FunctionLikeExpr,
    StatementLike,
    DeclarationLike,
    ControlFlowLike,
    BlockBeginLike,
    BlockEndLike,
};

struct MacroClassification {
    MacroRole role{MacroRole::ObjectLikeExpr};
    bool whitespace_sensitive{false};
};

static std::string normalize_macro_config_name(std::string name) {
    if (!name.empty() && name[0] == '`')
        name.erase(name.begin());
    return name;
}

static std::string macro_name(const Tok& tok) {
    if (!is_macro_usage(tok))
        return {};
    std::string text = tok_text(tok);
    if (text.empty() || text[0] != '`')
        return {};
    size_t end = 1;
    while (end < text.size() &&
           (std::isalnum((unsigned char)text[end]) || text[end] == '_' || text[end] == '$'))
        ++end;
    return text.substr(1, end - 1);
}

struct MacroClassifier {
    std::unordered_set<std::string> object_like_expr;
    std::unordered_set<std::string> function_like_expr;
    std::unordered_set<std::string> statement_like;
    std::unordered_set<std::string> declaration_like;
    std::unordered_set<std::string> control_flow_like;
    std::unordered_set<std::string> block_begin_like;
    std::unordered_set<std::string> block_end_like;
    std::unordered_set<std::string> whitespace_sensitive;

    explicit MacroClassifier(const MacroOptions& macros) {
        auto add_all = [](std::unordered_set<std::string>& dst,
                          const std::vector<std::string>& src) {
            for (const auto& name : src)
                dst.insert(normalize_macro_config_name(name));
        };
        add_all(object_like_expr, macros.object_like_expr);
        add_all(function_like_expr, macros.function_like_expr);
        add_all(statement_like, macros.statement_like);
        add_all(declaration_like, macros.declaration_like);
        add_all(control_flow_like, macros.control_flow_like);
        add_all(block_begin_like, macros.block_begin_like);
        add_all(block_end_like, macros.block_end_like);
        add_all(whitespace_sensitive, macros.whitespace_sensitive);
    }
};

static MacroClassification classify_macro(const Tok& tok, bool has_args,
                                          const MacroClassifier& macros) {
    MacroClassification result;
    result.role = has_args ? MacroRole::FunctionLikeExpr : MacroRole::ObjectLikeExpr;

    std::string name = macro_name(tok);
    if (macros.block_end_like.contains(name)) {
        result.role = MacroRole::BlockEndLike;
    } else if (macros.block_begin_like.contains(name)) {
        result.role = MacroRole::BlockBeginLike;
    } else if (macros.control_flow_like.contains(name)) {
        result.role = MacroRole::ControlFlowLike;
    } else if (macros.declaration_like.contains(name)) {
        result.role = MacroRole::DeclarationLike;
    } else if (macros.statement_like.contains(name)) {
        result.role = MacroRole::StatementLike;
    } else if (macros.function_like_expr.contains(name)) {
        result.role = has_args ? MacroRole::FunctionLikeExpr : MacroRole::ObjectLikeExpr;
    } else if (macros.object_like_expr.contains(name)) {
        result.role = MacroRole::ObjectLikeExpr;
    }

    result.whitespace_sensitive = macros.whitespace_sensitive.contains(name);
    return result;
}

static bool is_line_directive(const Tok& t) {
    return tok_directive(t) && !is_macro_usage(t);
}

static bool is_keyword(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) && LexerFacts::isKeyword(t.kind);
}

static bool is_constraint_keyword(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) &&
           t.kind == TokenKind::ConstraintKeyword;
}

static bool is_begin_keyword(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) && t.kind == TokenKind::BeginKeyword;
}

static bool is_else_keyword(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) && t.kind == TokenKind::ElseKeyword;
}

static bool is_identifier(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) &&
           (t.kind == TokenKind::Identifier || t.kind == TokenKind::SystemIdentifier ||
            (!tok_text(t).empty() &&
             (std::isalpha((unsigned char)tok_text(t)[0]) || tok_text(t)[0] == '_' || tok_text(t)[0] == '$' ||
              tok_text(t)[0] == '`') &&
             !is_keyword(t)));
}

static bool is_port_direction_token(const Tok& tok) {
    return tok.kind == TokenKind::InputKeyword || tok.kind == TokenKind::OutputKeyword ||
           tok.kind == TokenKind::InOutKeyword || tok.kind == TokenKind::RefKeyword;
}

static bool is_sign_qualifier_token(const Tok& tok) {
    return tok.kind == TokenKind::SignedKeyword || tok.kind == TokenKind::UnsignedKeyword;
}

static bool is_var_prefix_token(const Tok& tok) {
    return tok.kind == TokenKind::StaticKeyword || tok.kind == TokenKind::AutomaticKeyword ||
           tok.kind == TokenKind::ConstKeyword || tok.kind == TokenKind::VarKeyword;
}

static bool is_var_builtin_type_token(const Tok& tok) {
    return tok.kind == TokenKind::WireKeyword || tok.kind == TokenKind::LogicKeyword ||
           tok.kind == TokenKind::RegKeyword || tok.kind == TokenKind::BitKeyword ||
           tok.kind == TokenKind::ByteKeyword || tok.kind == TokenKind::IntKeyword ||
           tok.kind == TokenKind::IntegerKeyword || tok.kind == TokenKind::TimeKeyword ||
           tok.kind == TokenKind::ShortIntKeyword || tok.kind == TokenKind::LongIntKeyword ||
           tok.kind == TokenKind::SignedKeyword || tok.kind == TokenKind::UnsignedKeyword;
}

static bool is_function_call_skip_token(const Tok& tok) {
    switch (tok.kind) {
    case TokenKind::IfKeyword:
    case TokenKind::ForKeyword:
    case TokenKind::ForeachKeyword:
    case TokenKind::WhileKeyword:
    case TokenKind::RepeatKeyword:
    case TokenKind::WaitKeyword:
    case TokenKind::CaseKeyword:
    case TokenKind::CaseXKeyword:
    case TokenKind::CaseZKeyword:
    case TokenKind::ModuleKeyword:
    case TokenKind::MacromoduleKeyword:
    case TokenKind::FunctionKeyword:
    case TokenKind::TaskKeyword:
    case TokenKind::CoverGroupKeyword:
    case TokenKind::ClassKeyword:
    case TokenKind::PropertyKeyword:
    case TokenKind::SequenceKeyword:
    case TokenKind::AssertKeyword:
    case TokenKind::AssumeKeyword:
    case TokenKind::CoverKeyword:
        return true;
    default:
        return false;
    }
}

static bool is_covergroup_item_keyword(TokenKind kind) {
    switch (kind) {
        case TokenKind::CoverGroupKeyword:
        case TokenKind::CoverPointKeyword:
        case TokenKind::BinsKeyword:
        case TokenKind::IllegalBinsKeyword:
        case TokenKind::IgnoreBinsKeyword:
        case TokenKind::CrossKeyword:
        case TokenKind::EndGroupKeyword:
            return true;
        default:
            return false;
    }
}

static bool is_numeric(const Tok& t) {
    return t.kind == TokenKind::IntegerLiteral || t.kind == TokenKind::IntegerBase ||
           t.kind == TokenKind::UnbasedUnsizedLiteral || t.kind == TokenKind::RealLiteral ||
           t.kind == TokenKind::TimeLiteral;
}

static bool is_open_group(const Tok& t) { return tok_text(t) == "(" || tok_text(t) == "[" || tok_text(t) == "{"; }

static bool is_close_group(const Tok& t) { return tok_text(t) == ")" || tok_text(t) == "]" || tok_text(t) == "}"; }

static bool is_hierarchy(const Tok& t) { return tok_text(t) == "." || tok_text(t) == "::"; }

static bool is_unary_op(const Tok& t) {
    switch (t.kind) {
        case TokenKind::Tilde:
        case TokenKind::Exclamation:
        case TokenKind::TildeAnd:
        case TokenKind::TildeOr:
        case TokenKind::TildeXor:
        case TokenKind::XorTilde:
        case TokenKind::DoublePlus:
        case TokenKind::DoubleMinus:
            return true;
        default:
            return false;
    }
}

static bool is_combined_operator_token(TokenKind kind) {
    switch (kind) {
        case TokenKind::ColonEquals:
        case TokenKind::ColonSlash:
        case TokenKind::DoubleColon:
        case TokenKind::DoubleStar:
        case TokenKind::StarArrow:
        case TokenKind::DoublePlus:
        case TokenKind::PlusColon:
        case TokenKind::PlusDivMinus:
        case TokenKind::PlusModMinus:
        case TokenKind::DoubleMinus:
        case TokenKind::MinusColon:
        case TokenKind::MinusArrow:
        case TokenKind::MinusDoubleArrow:
        case TokenKind::TildeAnd:
        case TokenKind::TildeOr:
        case TokenKind::TildeXor:
        case TokenKind::DoubleHash:
        case TokenKind::HashMinusHash:
        case TokenKind::HashEqualsHash:
        case TokenKind::XorTilde:
        case TokenKind::DoubleEquals:
        case TokenKind::DoubleEqualsQuestion:
        case TokenKind::TripleEquals:
        case TokenKind::EqualsArrow:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::SlashEqual:
        case TokenKind::StarEqual:
        case TokenKind::AndEqual:
        case TokenKind::OrEqual:
        case TokenKind::PercentEqual:
        case TokenKind::XorEqual:
        case TokenKind::LeftShiftEqual:
        case TokenKind::TripleLeftShiftEqual:
        case TokenKind::RightShiftEqual:
        case TokenKind::TripleRightShiftEqual:
        case TokenKind::LeftShift:
        case TokenKind::RightShift:
        case TokenKind::TripleLeftShift:
        case TokenKind::TripleRightShift:
        case TokenKind::ExclamationEquals:
        case TokenKind::ExclamationEqualsQuestion:
        case TokenKind::ExclamationDoubleEquals:
        case TokenKind::LessThanEquals:
        case TokenKind::LessThanMinusArrow:
        case TokenKind::GreaterThanEquals:
        case TokenKind::DoubleOr:
        case TokenKind::OrMinusArrow:
        case TokenKind::OrEqualsArrow:
        case TokenKind::DoubleAt:
        case TokenKind::DoubleAnd:
        case TokenKind::TripleAnd:
            return true;
        default:
            return false;
    }
}

static bool is_control_keyword(const Tok& t) {
    switch (t.kind) {
        case TokenKind::IfKeyword:
        case TokenKind::ForKeyword:
        case TokenKind::ForeachKeyword:
        case TokenKind::WhileKeyword:
        case TokenKind::RepeatKeyword:
        case TokenKind::CaseKeyword:
        case TokenKind::CaseXKeyword:
        case TokenKind::CaseZKeyword:
            return is_keyword(t);
        default:
            return false;
    }
}

static bool is_procedural_event_keyword(const Tok& t) {
    switch (t.kind) {
        case TokenKind::AlwaysKeyword:
        case TokenKind::AlwaysFFKeyword:
        case TokenKind::AlwaysCombKeyword:
        case TokenKind::AlwaysLatchKeyword:
            return is_keyword(t);
        default:
            return false;
    }
}

static bool is_binary_op(const Tok& t) {
    switch (t.kind) {
        case TokenKind::TripleEquals:
        case TokenKind::ExclamationDoubleEquals:
        case TokenKind::DoubleEquals:
        case TokenKind::ExclamationEquals:
        case TokenKind::GreaterThanEquals:
        case TokenKind::MinusArrow:
        case TokenKind::MinusDoubleArrow:
        case TokenKind::DoubleAnd:
        case TokenKind::DoubleOr:
        case TokenKind::DoubleStar:
        case TokenKind::DoubleHash:
        case TokenKind::OrMinusArrow:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::AndEqual:
        case TokenKind::OrEqual:
        case TokenKind::XorEqual:
        case TokenKind::LeftShiftEqual:
        case TokenKind::RightShiftEqual:
        case TokenKind::TripleLeftShiftEqual:
        case TokenKind::TripleRightShiftEqual:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Xor:
        case TokenKind::And:
        case TokenKind::Or:
        case TokenKind::LessThanEquals:
        case TokenKind::LessThan:
        case TokenKind::GreaterThan:
        case TokenKind::Question:
        case TokenKind::LeftShift:
        case TokenKind::RightShift:
        case TokenKind::TripleLeftShift:
        case TokenKind::TripleRightShift:
        case TokenKind::TripleAnd:
        case TokenKind::InsideKeyword:
            return true;
        default:
            return false;
    }
}

static bool is_assignment_op(const Tok& t, bool in_parens = false) {
    if (t.kind == TokenKind::LessThanEquals && in_parens)
        return false;
    switch (t.kind) {
        case TokenKind::Equals:
        case TokenKind::LessThanEquals:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::AndEqual:
        case TokenKind::OrEqual:
        case TokenKind::XorEqual:
        case TokenKind::LeftShiftEqual:
        case TokenKind::RightShiftEqual:
        case TokenKind::TripleLeftShiftEqual:
        case TokenKind::TripleRightShiftEqual:
            return true;
        default:
            return false;
    }
}

static bool continues_after_function_like_macro(const Tok& tok) {
    if (tok_text(tok) == ";" || tok_text(tok) == "," || is_close_group(tok) || is_binary_op(tok) ||
        is_assignment_op(tok) || is_unary_op(tok))
        return true;
    return tok_text(tok) == "." || tok_text(tok) == "::" || tok_text(tok) == "[" || tok_text(tok) == "with";
}

static bool binary_space_before(const std::string& mode) {
    return mode == "before" || mode == "both";
}

static bool binary_space_after(const std::string& mode) {
    return mode == "after" || mode == "both";
}

enum class ParenKind {
    Ordinary,
    FunctionCall,
    EventControl,
    InstantiationParamList,
    InstantiationParamConnection,
    InstantiationPortList,
    InstantiationPortConnection
};

static ParenKind classify_paren(const Tok& L, bool procedural_at = false) {
    if (tok_text(L) == "@")
        return procedural_at ? ParenKind::EventControl : ParenKind::FunctionCall;
    if (is_identifier(L) || tok_text(L) == "]")
        return ParenKind::FunctionCall;
    return ParenKind::Ordinary;
}

static bool looks_unary_context(const Tok& L, const Tok& R) {
    if (tok_text(R) != "+" && tok_text(R) != "-")
        return false;
    return tok_text(L) == "(" || tok_text(L) == "[" || tok_text(L) == "{" || tok_text(L) == "," || tok_text(L) == ";" ||
           tok_text(L) == ":" || tok_text(L) == "?" || tok_text(L) == "=" || is_binary_op(L) || is_keyword(L);
}

static bool is_indexed_part_select_op_pair(const Tok& A, const Tok& B) {
    return (tok_text(A) == "+" || tok_text(A) == "-") && tok_text(B) == ":";
}

static bool is_indexed_part_select_op(const Tok& t) { return tok_text(t) == "+:" || tok_text(t) == "-:"; }

// ---------------------------------------------------------------------------
// Disabled ranges (verilog_format: off/on and `define)
// ---------------------------------------------------------------------------

// Match "// <optional whitespace> verilog_format <optional whitespace> : <optional whitespace> <word>"
// case-insensitive.  Returns the position past the matched word, or std::string::npos.
static size_t match_fmt_comment(const std::string& src, size_t pos, const std::string& word) {
    size_t n = src.size();
    if (pos + 1 >= n || src[pos] != '/' || src[pos + 1] != '/')
        return std::string::npos;
    size_t i = pos + 2;
    while (i < n && (src[i] == ' ' || src[i] == '\t'))
        ++i;
    const char* tag = "verilog_format";
    for (size_t k = 0; tag[k]; ++k, ++i) {
        if (i >= n || std::tolower((unsigned char)src[i]) != tag[k])
            return std::string::npos;
    }
    while (i < n && (src[i] == ' ' || src[i] == '\t'))
        ++i;
    if (i >= n || src[i] != ':')
        return std::string::npos;
    ++i;
    while (i < n && (src[i] == ' ' || src[i] == '\t'))
        ++i;
    for (size_t k = 0; k < word.size(); ++k, ++i) {
        if (i >= n || std::tolower((unsigned char)src[i]) != (unsigned char)word[k])
            return std::string::npos;
    }
    // Word boundary: next char must not be alphanumeric/underscore
    if (i < n && (std::isalnum((unsigned char)src[i]) || src[i] == '_'))
        return std::string::npos;
    // Advance to end of line
    while (i < n && src[i] != '\n')
        ++i;
    return i;
}

static std::vector<std::pair<int, int>> find_disabled(const std::string& src) {
    std::vector<std::pair<int, int>> out;
    size_t n = src.size();
    DirectiveIndex directives(src);

    // Find verilog_format: off/on regions
    for (size_t i = 0; i < n; ++i) {
        size_t off_end = match_fmt_comment(src, i, "off");
        if (off_end == std::string::npos)
            continue;
        int off_pos = (int)i;
        // Search for matching "on"
        int end = (int)n;
        for (size_t j = off_end; j < n; ++j) {
            size_t on_end = match_fmt_comment(src, j, "on");
            if (on_end != std::string::npos) {
                end = (int)on_end;
                break;
            }
        }
        out.push_back({off_pos, end});
        i = (size_t)end;
    }

    // Find `define blocks (including multi-line with \ continuation)
    for (size_t i = 0; i < n;) {
        // Must be at start of line (or start of file)
        if (i > 0 && src[i - 1] != '\n') {
            ++i;
            continue;
        }
        size_t line_end = src.find('\n', i);
        if (line_end == std::string::npos)
            line_end = n;
        auto directive = first_directive_on_line(src, i, line_end, directives);
        if (directive.kind != syntax::SyntaxKind::DefineDirective) {
            // Skip to next line
            i = line_end;
            if (i < n)
                ++i;
            continue;
        }
        size_t ls = directive.start;
        // Found `define at ls.  First skip past the macro name and any argument
        // list.  The argument list `(...)` can span multiple lines WITHOUT
        // backslash continuation (only the body needs `\`).
        size_t j = directive.end;
        // Skip whitespace after `define
        while (j < n && (src[j] == ' ' || src[j] == '\t'))
            ++j;
        // Skip macro name
        while (j < n && (std::isalnum((unsigned char)src[j]) || src[j] == '_' || src[j] == '$'))
            ++j;
        // If there's an argument list, skip it (may span lines)
        if (j < n && src[j] == '(') {
            int depth = 1;
            ++j;
            while (j < n && depth > 0) {
                if (src[j] == '(')
                    ++depth;
                else if (src[j] == ')')
                    --depth;
                ++j;
            }
        }
        // Now consume lines with trailing backslash continuation (macro body).
        while (true) {
            while (j < n && src[j] != '\n')
                ++j;
            // Check if line ends with backslash (before the newline)
            if (j > ls && src[j - 1] == '\\' && j < n) {
                ++j; // skip newline, continue to next line
            } else {
                break;
            }
        }
        out.push_back({(int)ls, (int)j});
        i = j;
        if (i < n)
            ++i;
    }

    std::sort(out.begin(), out.end());
    return out;
}

static bool in_disabled(int pos, const std::vector<std::pair<int, int>>& ranges) {
    for (auto& r : ranges)
        if (r.first <= pos && pos < r.second)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Spacing rules — ported from SpacesRequiredBetween() in token-annotator.cc
// ---------------------------------------------------------------------------

static int spaces_req(const Tok& L, const Tok& R, const FormatOptions& opts, bool in_dim,
                      bool in_for_header, bool in_parens, ParenKind paren_kind,
                      bool procedural_at) {
    const auto& lx = tok_text(L);
    const auto& ll = tok_lo(L);
    const auto& rx = tok_text(R);
    const auto& sp = opts.spacing;
    bool L_assign = is_assignment_op(L, in_parens);
    bool R_assign = is_assignment_op(R, in_parens);

    if (is_line_directive(L) || is_line_directive(R))
        return 0;
    if (tok_comment(R))
        return 1;
    if (lx == "(") {
        if (paren_kind == ParenKind::InstantiationPortList)
            return 0;
        if (paren_kind == ParenKind::EventControl)
            return sp.space_inside_event_control_parens ? 1 : 0;
        if (paren_kind == ParenKind::Ordinary)
            return sp.space_inside_parens ? 1 : 0;
        return 0;
    }
    if (rx == ")") {
        if (paren_kind == ParenKind::InstantiationPortList)
            return 0;
        if (paren_kind == ParenKind::EventControl)
            return sp.space_inside_event_control_parens ? 1 : 0;
        if (paren_kind == ParenKind::Ordinary)
            return sp.space_inside_parens ? 1 : 0;
        return 0;
    }
    if (lx == "[")
        return sp.space_inside_dimension_brackets ? 1 : 0;
    if (rx == "]")
        return sp.space_inside_dimension_brackets ? 1 : 0;
    if (is_open_group(L) || is_close_group(R))
        return 0;
    if (is_unary_op(L) && is_binary_op(R))
        return 1;
    if (is_unary_op(L))
        return 0;
    if (is_hierarchy(L) && lx == "::")
        return 0;
    if (rx == ",")
        return 0;
    if (lx == ",")
        return 1;
    if (rx == ";")
        return in_for_header ? (binary_space_before(sp.semicolon_spacing) ? 1 : 0)
                             : ((lx == ":") ? 1 : 0);
    if (lx == ";")
        return in_for_header ? (binary_space_after(sp.semicolon_spacing) ? 1 : 0) : 1;
    if (is_indent_close(L.kind) && rx == ":")
        return 0;
    if (lx == "@")
        return procedural_at ? (binary_space_after(sp.procedural_event_control_at_spacing) ? 1 : 0)
                             : 0;
    if (rx == "@")
        return procedural_at ? (binary_space_before(sp.procedural_event_control_at_spacing) ? 1 : 0)
                             : 1;
    if (is_unary_op(L) && rx == "{")
        return 0;
    if (L_assign || R_assign) {
        if (R_assign)
            return binary_space_before(sp.assignment_operator_spacing) ? 1 : 0;
        return binary_space_after(sp.assignment_operator_spacing) ? 1 : 0;
    }
    if (in_dim && is_indexed_part_select_op(R))
        return binary_space_before(sp.indexed_part_select_spacing) ? 1 : 0;
    if (in_dim && is_indexed_part_select_op(L))
        return binary_space_after(sp.indexed_part_select_spacing) ? 1 : 0;
    if (in_dim && is_indexed_part_select_op_pair(L, R))
        return binary_space_before(sp.indexed_part_select_spacing) ? 1 : 0;
    if (in_dim && (lx == "+" || lx == "-") && rx == ":")
        return 0;
    if (in_dim && (lx == ":" || rx == ":"))
        return (lx == ":") ? (binary_space_after(sp.range_colon_spacing) ? 1 : 0)
                           : (binary_space_before(sp.range_colon_spacing) ? 1 : 0);
    if (L.kind == TokenKind::InsideKeyword || R.kind == TokenKind::InsideKeyword)
        return 1;
    if ((is_binary_op(L) && !L_assign) || (is_binary_op(R) && !R_assign)) {
        if (is_binary_op(L) && is_unary_op(R))
            return 1;
        if (looks_unary_context(L, R))
            return 0;
        const std::string& mode =
            in_dim ? sp.dimension_binary_operator_spacing : sp.binary_operator_spacing;
        if (is_binary_op(R) && !R_assign)
            return binary_space_before(mode) ? 1 : 0;
        if (is_binary_op(L) && !L_assign)
            return binary_space_after(mode) ? 1 : 0;
    }
    if (is_binary_op(L) || is_binary_op(R)) {
        return 1;
    }
    if (is_hierarchy(L) || is_hierarchy(R))
        return 0;
    if (rx == "'" || lx == "'" || (!rx.empty() && rx[0] == '\'') || (!lx.empty() && lx[0] == '\''))
        return 0;
    if (rx == "(") {
        if (lx == "#")
            return 0;
        if (lx == ")")
            return 1;
        if (L.kind == TokenKind::WaitKeyword)
            return 0;
        if (is_identifier(L))
            return 0;
        if (is_control_keyword(L))
            return sp.control_keyword_space ? 1 : 0;
        if (is_keyword(L))
            return 1;
        return 0;
    }
    if (lx == ":")
        return in_dim ? 0 : 1;
    if (rx == ":") {
        if (L.kind == TokenKind::DefaultKeyword)
            return 0;
        if (in_dim)
            return 0;
        if (is_identifier(L) || is_numeric(L) || is_close_group(L))
            return 0;
        return 1;
    }
    if (lx == "}")
        return 1;
    if (R.kind == TokenKind::OpenBrace)
        return (is_keyword(L) || is_identifier(L) || L.kind == TokenKind::CloseParenthesis ||
                L.kind == TokenKind::CloseBracket) ? 1 : 0;
    if (rx == "[") {
        if (lx == "]")
            return 0;
        if (is_type_keyword(L.kind))
            return 1;
        return 0;
    }
    auto nm = [](const Tok& t) { return is_numeric(t) || is_identifier(t) || is_keyword(t); };
    if (nm(L) && nm(R))
        return 1;
    if (is_keyword(L))
        return 1;
    if (is_unary_op(L) || is_unary_op(R))
        return 0;
    if (lx == "#")
        return 0;
    if (rx == "#")
        return 1;
    if (is_keyword(R))
        return 1;
    if (lx == ")")
        return (rx == ":") ? 0 : 1;
    if (lx == "]")
        return 1;
    return 1;
}

// ---------------------------------------------------------------------------
// Break decisions — ported from BreakDecisionBetween()
// ---------------------------------------------------------------------------

static SD break_dec(const Tok& L, const Tok& R, const FormatOptions& opts, bool in_dim) {
    const auto& lx = tok_text(L);
    const auto& ll = tok_lo(L);
    const auto& rx = tok_text(R);
    const auto& rl = tok_lo(R);

    if (in_dim && lx != ":" && lx != "[" && lx != "]" && rx != ":" && rx != "[" && rx != "]")
        return SD::Preserve;
    if (is_line_directive(R) || is_line_directive(L))
        return SD::MustWrap;
    if (tok_comment(L) && tok_text(L).find('\n') != std::string::npos)
        return SD::MustWrap;
    if (tok_comment(L) && tok_text(L).rfind("//", 0) == 0)
        return SD::MustWrap;
    if (is_unary_op(L))
        return SD::MustAppend;
    if (is_indent_close(L.kind) && rx == ":")
        return SD::MustAppend;
    if (is_indent_close(R.kind))
        return SD::MustWrap;
    if (is_else_keyword(R)) {
        if (L.kind == TokenKind::EndKeyword)
            return opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        if (L.kind == TokenKind::CloseBrace)
            return opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        return SD::MustWrap;
    }
    if (is_else_keyword(L) &&
        (is_begin_keyword(R) || R.kind == TokenKind::IfKeyword || R.kind == TokenKind::OpenBrace))
        return (R.kind == TokenKind::OpenBrace && opts.statement.begin_newline) ? SD::MustWrap
                                                                                : SD::MustAppend;
    if (L.kind == TokenKind::CloseParenthesis &&
        (is_begin_keyword(R) || R.kind == TokenKind::OpenBrace))
        return opts.statement.begin_newline ? SD::MustWrap : SD::MustAppend;
    if (lx == "#")
        return SD::MustAppend;
    return SD::Undecided;
}

static void push_tok(std::vector<Tok>& toks, TokenKind kind, std::string text, int pos,
                     bool whitespace = false, bool comment = false, bool directive = false,
                     syntax::SyntaxKind directive_kind = syntax::SyntaxKind::Unknown,
                     bool define_block = false) {
    auto lex = std::make_shared<TokLexeme>();
    lex->kind = kind;
    lex->lo = lower(text);
    lex->text = std::move(text);
    lex->pos = pos;
    lex->whitespace = whitespace;
    lex->comment = comment;
    lex->directive = directive;
    lex->define_block = define_block;
    lex->directive_kind = directive_kind;
    Tok t;
    t.lex = std::move(lex);
    t.kind = kind;
    toks.push_back(std::move(t));
}

static TokenKind single_punct_token_kind(char ch) {
    switch (ch) {
        case '+':
            return TokenKind::Plus;
        case '-':
            return TokenKind::Minus;
        case '*':
            return TokenKind::Star;
        case '/':
            return TokenKind::Slash;
        case '%':
            return TokenKind::Percent;
        case '&':
            return TokenKind::And;
        case '|':
            return TokenKind::Or;
        case '^':
            return TokenKind::Xor;
        case '<':
            return TokenKind::LessThan;
        case '>':
            return TokenKind::GreaterThan;
        case '?':
            return TokenKind::Question;
        case '=':
            return TokenKind::Equals;
        case '!':
            return TokenKind::Exclamation;
        case '~':
            return TokenKind::Tilde;
        default:
            return TokenKind::Unknown;
    }
}

// Scans from pos to the end of a directive line, following '\' line continuations.
// Returns the position just past the last consumed character (≤ limit).
// If has_continuation is non-null, sets it true when any continuation line was consumed.
static size_t scan_directive_end(const std::string& src, size_t pos, size_t limit,
                                 bool* has_continuation = nullptr) {
    if (has_continuation)
        *has_continuation = false;
    size_t j = pos;
    do {
        while (j < limit && src[j] != '\n')
            ++j;
        if (j == 0 || src[j - 1] != '\\')
            break;
        if (has_continuation)
            *has_continuation = true;
        if (j < limit)
            ++j;
    } while (j < limit);
    return j;
}

// Mini-lexer: scans source[start..end] and appends typed Tok entries to toks.
// define_block: true when scanning inside a `define body — changes backtick handling.
// define_directive_kind: SyntaxKind to attach to a `define directive token found in a define block.
static void append_trivia_text(std::vector<Tok>& toks, const std::string& source, size_t start,
                               size_t end, bool define_block = false,
                               syntax::SyntaxKind define_directive_kind =
                                   syntax::SyntaxKind::Unknown) {
    size_t i = start;
    while (i < end) {
        if (source[i] == '/' && i + 1 < end && source[i + 1] == '/') {
            // Line comment: consume to end of line (excluding the newline itself).
            size_t j = i + 2;
            while (j < end && source[j] != '\n')
                ++j;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, true,
                     false, syntax::SyntaxKind::Unknown, define_block);
            i = j;
        } else if (source[i] == '/' && i + 1 < end && source[i + 1] == '*') {
            // Block comment: consume up to and including the closing */.
            size_t j = i + 2;
            while (j + 1 < end && !(source[j] == '*' && source[j + 1] == '/'))
                ++j;
            if (j + 1 < end)
                j += 2;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, true,
                     false, syntax::SyntaxKind::Unknown, define_block);
            i = j;
        } else if (source[i] == '`') {
            // Backtick: compiler directive or macro identifier.
            // Find the start of the current line to determine if the backtick is line-leading.
            size_t line_start = i;
            while (line_start > start && source[line_start - 1] != '\n')
                --line_start;
            size_t first = line_start;
            while (first < i && (source[first] == ' ' || source[first] == '\t'))
                ++first;
            // Collect the directive/macro name (backtick + alphanumeric/_/$).
            size_t name_end = i + 1;
            while (name_end < end && (std::isalnum((unsigned char)source[name_end]) ||
                                      source[name_end] == '_' || source[name_end] == '$'))
                ++name_end;
            std::string name = source.substr(i, name_end - i);
            if (!define_block && first == i && is_compiler_directive_name(name)) {
                // Line-leading compiler directive outside a define block:
                // consume the entire (possibly line-continuation-escaped) directive line.
                size_t j = scan_directive_end(source, i, end);
                push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, false,
                         true, syntax::SyntaxKind::Unknown, define_block);
                i = j;
            } else {
                // Inside a define block, a line-leading `define becomes a Directive token;
                // all other backtick names are treated as Identifiers (macro uses).
                bool define_directive =
                    define_block && name == "`define" && first == i;
                push_tok(toks, define_directive ? TokenKind::Directive : TokenKind::Identifier,
                         std::move(name), (int)i, false, false, define_directive,
                         define_directive ? define_directive_kind : syntax::SyntaxKind::Unknown,
                         define_block);
                i = name_end;
            }
        } else if (source[i] == '"') {
            // String literal: handle backslash escapes, stop at closing quote.
            size_t j = i + 1;
            while (j < end) {
                if (source[j] == '\\' && j + 1 < end) {
                    j += 2;
                } else if (source[j] == '"') {
                    ++j;
                    break;
                } else {
                    ++j;
                }
            }
            push_tok(toks, TokenKind::StringLiteral, source.substr(i, j - i), (int)i, false,
                     false, false, syntax::SyntaxKind::Unknown, define_block);
            i = j;
        } else if (std::isspace((unsigned char)source[i])) {
            // Whitespace run (spaces, tabs, newlines) — single token marked is_whitespace.
            size_t j = i + 1;
            while (j < end && std::isspace((unsigned char)source[j]))
                ++j;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, true, false,
                     false, syntax::SyntaxKind::Unknown, define_block);
            i = j;
        } else if (std::isalpha((unsigned char)source[i]) || source[i] == '_' || source[i] == '$') {
            // Identifier or system task/function name ($display, etc.).
            size_t j = i + 1;
            while (j < end &&
                   (std::isalnum((unsigned char)source[j]) || source[j] == '_' || source[j] == '$'))
                ++j;
            push_tok(toks, TokenKind::Identifier, source.substr(i, j - i), (int)i, false,
                     false, false, syntax::SyntaxKind::Unknown, define_block);
            i = j;
        } else if (std::isdigit((unsigned char)source[i])) {
            // Numeric literal; ' is included to handle SV sized literals (e.g. 8'hFF).
            size_t j = i + 1;
            while (j < end && (std::isalnum((unsigned char)source[j]) || source[j] == '_' ||
                               source[j] == '\''))
                ++j;
            push_tok(toks, TokenKind::IntegerLiteral, source.substr(i, j - i), (int)i, false,
                     false, false, syntax::SyntaxKind::Unknown, define_block);
            i = j;
        } else if (std::ispunct((unsigned char)source[i])) {
            // Operator or punctuation: try longest multi-char match first, then single char.
            static const std::vector<std::string> OPS = {
                "<<=", ">>=", "===", "!==", "<<<", ">>>", "->", "<->", "&&", "||", "**", "##",
                "|->", "+=",  "-=",  "*=",  "/=",  "%=",  "&=", "|=",  "^=", "<=", ">=", "==",
                "!=",  "::",  "+:",  "-:",  "~&",  "~|",  "~^", "^~",  "++", "--", "<<", ">>"};
            bool matched = false;
            for (const auto& op : OPS) {
                if (i + op.size() <= end && source.compare(i, op.size(), op) == 0) {
                    push_tok(toks, TokenKind::Unknown, op, (int)i, false, false, false,
                             syntax::SyntaxKind::Unknown, define_block);
                    i += op.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                push_tok(toks, single_punct_token_kind(source[i]), source.substr(i, 1),
                         (int)i, false, false, false, syntax::SyntaxKind::Unknown,
                         define_block);
                ++i;
            }
        } else {
            // Unrecognised character — emit as Unknown.
            push_tok(toks, TokenKind::Unknown, source.substr(i, 1), (int)i, false, false,
                     false, syntax::SyntaxKind::Unknown, define_block);
            ++i;
        }
    }
}

static std::string normalize_spacing_fragment(const std::string& text, const FormatOptions& opts,
                                              bool in_dim) {
    std::vector<Tok> toks;
    append_trivia_text(toks, text, 0, text.size());

    std::string out;
    const Tok* prev = nullptr;
    for (const auto& tok : toks) {
        if (tok_whitespace(tok))
            continue;
        if (prev) {
            int spaces =
                spaces_req(*prev, tok, opts, in_dim, false, false, ParenKind::Ordinary, false);
            if (spaces > 0)
                out += std::string(spaces, ' ');
        }
        out += tok_text(tok);
        prev = &tok;
    }
    return out;
}

static std::string normalize_bracket_spacing(const std::string& text, const FormatOptions& opts) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '[') {
            out += text[i++];
            continue;
        }

        size_t start = i;
        int depth = 0;
        do {
            if (text[i] == '[')
                ++depth;
            else if (text[i] == ']')
                --depth;
            ++i;
        } while (i < text.size() && depth > 0);
        out += normalize_spacing_fragment(text.substr(start, i - start), opts, true);
    }
    return out;
}

static std::vector<Tok> collect_lexer_tokens(const std::string& source) {
    if (active_token_cache) {
        auto it = active_token_cache->find(source);
        if (it != active_token_cache->end())
            return it->second;
    }

    struct DefineRange {
        size_t start;
        size_t end;
        syntax::SyntaxKind kind;
    };
    std::vector<Tok> toks;
    std::vector<DefineRange> define_ranges;
    auto find_define_range = [&](size_t pos) -> const DefineRange* {
        for (const auto& range : define_ranges)
            if (range.start <= pos && pos < range.end)
                return &range;
        return nullptr;
    };
    auto append_gap = [&](size_t start, size_t end) {
        size_t p = start;
        while (p < end) {
            const DefineRange* dr = find_define_range(p);
            bool in_def = dr != nullptr;
            size_t q = end;
            for (const auto& range : define_ranges) {
                if (in_def && range.start <= p && p < range.end) {
                    q = std::min(q, range.end);
                } else if (!in_def && p < range.start) {
                    q = std::min(q, range.start);
                }
            }
            append_trivia_text(toks, source, p, q, in_def,
                               dr ? dr->kind : syntax::SyntaxKind::Unknown);
            p = q;
        }
    };

    SourceManager sm;
    auto buffer = sm.assignText(source);
    BumpAllocator alloc;
    Diagnostics diagnostics;
    Lexer lexer(buffer, alloc, diagnostics, sm);

    size_t cursor = 0;
    while (true) {
        Token token = lexer.lex();
        if (token.kind == TokenKind::EndOfFile) {
            break;
        }
        // Get the offset of this token
        auto loc = token.location();
        if (!loc.valid())
            continue;
        size_t off = loc.offset();
        if (off > source.size())
            continue;
        if (off < cursor)
            continue;
        bool token_define_block = find_define_range(off) != nullptr;
        // Fill gap between cursor and this token with trivia text
        // (handles comments, directives, strings in gaps)
        if (cursor < off)
            append_gap(cursor, off);
        std::string raw(token.rawText());
        if (!raw.empty() && raw[0] == '`') {
            size_t line_start = off;
            while (line_start > 0 && source[line_start - 1] != '\n')
                --line_start;
            size_t first = line_start;
            while (first < off && (source[first] == ' ' || source[first] == '\t'))
                ++first;
            if (token.kind == TokenKind::Directive &&
                token.directiveKind() != syntax::SyntaxKind::MacroUsage &&
                !is_pp_conditional(token.directiveKind()) && first == off) {
                bool has_continuation = false;
                size_t end = scan_directive_end(source, off, source.size(), &has_continuation);
                if (token.directiveKind() == syntax::SyntaxKind::DefineDirective) {
                    if (!has_continuation && end < source.size() && source[end] == '\n')
                        ++end;
                    define_ranges.push_back({off, end, token.directiveKind()});
                    append_gap(off, end);
                } else {
                    push_tok(toks, token.kind, source.substr(off, end - off), (int)off,
                             false, false, true, token.directiveKind());
                }
                cursor = std::max(cursor, end);
                continue;
            }
        }
        if (!raw.empty() && std::isdigit((unsigned char)raw[0])) {
            size_t end = off + raw.size();
            if (end < source.size() && source[end] == '\'') {
                ++end;
                while (end < source.size() &&
                       (std::isalnum((unsigned char)source[end]) || source[end] == '_' ||
                        source[end] == '\'' || source[end] == '?' || source[end] == 'x' ||
                        source[end] == 'X' || source[end] == 'z' || source[end] == 'Z'))
                    ++end;
                push_tok(toks, token.kind, source.substr(off, end - off), (int)off, false,
                         false, false, syntax::SyntaxKind::Unknown, token_define_block);
                cursor = std::max(cursor, end);
                continue;
            }
        }
        if (is_combined_operator_token(token.kind) &&
            (off + raw.size() > source.size() || source.compare(off, raw.size(), raw) != 0)) {
            push_tok(toks, TokenKind::Unknown, raw.substr(0, 1), (int)off, false, false,
                     false, syntax::SyntaxKind::Unknown, token_define_block);
            cursor = std::max(cursor, off + 1);
            continue;
        }
        if (!raw.empty()) {
            syntax::SyntaxKind directive_kind = syntax::SyntaxKind::Unknown;
            if (token.kind == TokenKind::Directive)
                directive_kind = token.directiveKind();
            push_tok(toks, token.kind, raw, (int)off, false, false,
                     token.kind == TokenKind::Directive,
                     directive_kind, token_define_block);
        }
        cursor = std::max(cursor, off + raw.size());
    }
    if (cursor < source.size())
        append_gap(cursor, source.size());

    if (active_token_cache) {
        auto [it, _] = active_token_cache->emplace(source, std::move(toks));
        return it->second;
    }
    return toks;
}

static bool line_has_pp_conditional(const std::string& line) {
    for (const auto& tok : collect_lexer_tokens(line)) {
        if (tok_whitespace(tok) || tok_comment(tok))
            continue;
        if ((is_line_directive(tok) && is_pp_conditional(tok_directive_kind(tok))) ||
            is_pp_conditional_text(tok_text(tok)))
            return true;
    }
    return false;
}

static bool range_has_pp_conditional(const std::vector<std::string>& lines, size_t first,
                                     size_t last_inclusive) {
    if (lines.empty() || first >= lines.size())
        return false;
    last_inclusive = std::min(last_inclusive, lines.size() - 1);
    for (size_t i = first; i <= last_inclusive; ++i)
        if (line_has_pp_conditional(lines[i]))
            return true;
    return false;
}

struct PPContext {
    struct LineInfo {
        bool conditional{false};
        int depth_before{0};
        int depth_after{0};
    };
    std::vector<LineInfo> lines;
};

static PPContext build_pp_context(const std::vector<std::string>& lines) {
    PPContext ctx;
    ctx.lines.resize(lines.size());
    int depth = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        ctx.lines[i].depth_before = depth;
        bool is_if = false;
        bool is_endif = false;
        bool is_cond = false;
        for (const auto& tok : collect_lexer_tokens(lines[i])) {
            if (tok_whitespace(tok) || tok_comment(tok))
                continue;
            if ((is_line_directive(tok) && is_pp_conditional(tok_directive_kind(tok))) ||
                is_pp_conditional_text(tok_text(tok))) {
                is_cond = true;
                syntax::SyntaxKind kind = tok.kind == TokenKind::Directive
                                              ? tok_directive_kind(tok)
                                              : directive_at_offset(tok_text(tok), 0).kind;
                is_if = kind == syntax::SyntaxKind::IfDefDirective ||
                        kind == syntax::SyntaxKind::IfNDefDirective;
                is_endif = kind == syntax::SyntaxKind::EndIfDirective;
            }
            break;
        }
        ctx.lines[i].conditional = is_cond;
        if (is_endif && depth > 0)
            --depth;
        if (is_if)
            ++depth;
        ctx.lines[i].depth_after = depth;
    }
    return ctx;
}

static bool pp_line_has_conditional(const PPContext& ctx, size_t line) {
    return line < ctx.lines.size() && ctx.lines[line].conditional;
}

static bool pp_range_has_conditional(const PPContext& ctx, size_t first, size_t last_inclusive) {
    if (ctx.lines.empty() || first >= ctx.lines.size())
        return false;
    last_inclusive = std::min(last_inclusive, ctx.lines.size() - 1);
    for (size_t i = first; i <= last_inclusive; ++i)
        if (pp_line_has_conditional(ctx, i))
            return true;
    return false;
}

static std::string trim_left_copy(std::string s) {
    size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    s.erase(0, first);
    return s;
}

static std::string trim_right_copy(std::string s) {
    size_t last = s.find_last_not_of(" \t");
    if (last == std::string::npos)
        return "";
    s.erase(last + 1);
    return s;
}

static std::pair<std::string, std::string> split_code_line_comment_tokenized(
    const std::string& line) {
    for (const auto& tok : collect_lexer_tokens(line)) {
        if (tok_comment(tok)) {
            return {trim_right_copy(line.substr(0, (size_t)tok_pos(tok))), " " + line.substr(tok_pos(tok))};
        }
    }
    return {trim_right_copy(line), ""};
}

static std::vector<std::string> code_tokens_for_alignment(const std::string& code) {
    std::vector<std::string> out;
    for (const auto& tok : collect_lexer_tokens(code)) {
        if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
            continue;
        out.push_back(tok_text(tok));
    }
    return out;
}

static std::vector<std::string> group_bracket_tokens(std::vector<std::string> toks) {
    std::vector<std::string> out;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] != "[") {
            out.push_back(std::move(toks[i]));
            continue;
        }
        std::string grouped = "[";
        int depth = 1;
        while (++i < toks.size()) {
            grouped += toks[i];
            if (toks[i] == "[")
                ++depth;
            else if (toks[i] == "]" && --depth == 0)
                break;
        }
        out.push_back(std::move(grouped));
    }
    return out;
}

static std::vector<std::string> group_scope_resolution_tokens(std::vector<std::string> toks) {
    std::vector<std::string> out;
    for (size_t i = 0; i < toks.size(); ++i) {
        std::string grouped = toks[i];
        while (i + 2 < toks.size() && toks[i + 1] == "::") {
            grouped += "::" + toks[i + 2];
            i += 2;
        }
        out.push_back(std::move(grouped));
    }
    return out;
}

static std::vector<Tok> significant_tokens(const std::string& text) {
    std::vector<Tok> out;
    for (auto& tok : collect_lexer_tokens(text)) {
        if (!tok_whitespace(tok) && !tok_comment(tok) && !tok_directive(tok))
            out.push_back(std::move(tok));
    }
    return out;
}

static std::vector<Tok> significant_tokens_from(const std::vector<Tok>& toks) {
    std::vector<Tok> out;
    for (const auto& tok : toks) {
        if (!tok_whitespace(tok) && !tok_comment(tok) && !tok_directive(tok))
            out.push_back(tok);
    }
    return out;
}

static bool tok_is(const Tok& tok, const std::string& text, TokenKind kind) {
    return tok_text(tok) == text || tok.kind == kind;
}

static bool tok_contains(const Tok& tok, char ch) {
    if (!tok_text(tok).empty() && tok_text(tok)[0] == '"')
        return false;
    if (tok_text(tok).size() > 2)
        return false;
    return tok_text(tok).find(ch) != std::string::npos;
}

static bool starts_with_chars(const std::string& s, char a) {
    return !s.empty() && s[0] == a;
}

static bool starts_with_chars(const std::string& s, char a, char b) {
    return s.size() >= 2 && s[0] == a && s[1] == b;
}

// ---------------------------------------------------------------------------
// Split by top-level comma (depth-0)
// ---------------------------------------------------------------------------

struct ArgPiece {
    std::string text;
    bool comma{false};
};

static std::vector<ArgPiece> split_top_level_args_with_commas(const std::string& text) {
    std::vector<ArgPiece> parts;
    size_t start = 0;
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
            continue;
        if (tok_contains(tok, '('))
            ++paren;
        else if (tok_contains(tok, ')') && paren > 0)
            --paren;
        else if (tok_contains(tok, '['))
            ++bracket;
        else if (tok_contains(tok, ']') && bracket > 0)
            --bracket;
        else if (tok_contains(tok, '{'))
            ++brace;
        else if (tok_contains(tok, '}') && brace > 0)
            --brace;
        else if (tok_is(tok, ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
            parts.push_back({text.substr(start, (size_t)tok_pos(tok) - start), true});
            start = (size_t)tok_pos(tok) + tok_text(tok).size();
        }
    }
    parts.push_back({text.substr(start), false});
    return parts;
}

static std::vector<std::string> split_top_level(const std::string& text) {
    std::vector<std::string> parts;
    size_t start = 0;
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
            continue;
        if (tok_contains(tok, '('))
            ++paren;
        else if (tok_contains(tok, ')') && paren > 0)
            --paren;
        else if (tok_contains(tok, '['))
            ++bracket;
        else if (tok_contains(tok, ']') && bracket > 0)
            --bracket;
        else if (tok_contains(tok, '{'))
            ++brace;
        else if (tok_contains(tok, '}') && brace > 0)
            --brace;
        else if (tok_is(tok, ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
            parts.push_back(text.substr(start, (size_t)tok_pos(tok) - start));
            start = (size_t)tok_pos(tok) + tok_text(tok).size();
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

// ---------------------------------------------------------------------------
// Port-declaration alignment pass
// ---------------------------------------------------------------------------

struct PortParsed {
    bool valid{false};
    std::string indent, direction, dtype, qualifier, dim;
    std::vector<std::pair<std::string, std::string>> names; // (name, trailing)
    std::string terminator, comment;
};

static TokenKind single_word_kind(const std::string& text) {
    auto toks = significant_tokens(text);
    if (toks.size() != 1)
        return TokenKind::Unknown;
    return toks[0].kind;
}

static bool is_port_dir_text(const std::string& text) {
    TokenKind kind = single_word_kind(text);
    return kind == TokenKind::InputKeyword || kind == TokenKind::OutputKeyword ||
           kind == TokenKind::InOutKeyword;
}

static bool is_port_builtin_type_text(const std::string& text) {
    TokenKind kind = single_word_kind(text);
    return is_type_keyword(kind) || kind == TokenKind::WireKeyword || kind == TokenKind::RegKeyword ||
           kind == TokenKind::VarKeyword || kind == TokenKind::EventKeyword;
}

static bool is_port_net_type_text(const std::string& text) {
    switch (single_word_kind(text)) {
        case TokenKind::VarKeyword:
        case TokenKind::WireKeyword:
        case TokenKind::UWireKeyword:
        case TokenKind::TriKeyword:
        case TokenKind::Tri0Keyword:
        case TokenKind::Tri1Keyword:
        case TokenKind::WAndKeyword:
        case TokenKind::TriAndKeyword:
        case TokenKind::WOrKeyword:
        case TokenKind::TriOrKeyword:
        case TokenKind::TriRegKeyword:
        case TokenKind::Supply0Keyword:
        case TokenKind::Supply1Keyword:
            return true;
        default:
            return false;
    }
}

static bool is_port_data_type_text(const std::string& text) {
    switch (single_word_kind(text)) {
        case TokenKind::LogicKeyword:
        case TokenKind::RegKeyword:
        case TokenKind::BitKeyword:
        case TokenKind::ByteKeyword:
        case TokenKind::ShortIntKeyword:
        case TokenKind::IntKeyword:
        case TokenKind::LongIntKeyword:
        case TokenKind::IntegerKeyword:
        case TokenKind::TimeKeyword:
            return true;
        default:
            return false;
    }
}

static bool is_sign_qualifier_text(const std::string& text) {
    TokenKind kind = single_word_kind(text);
    return kind == TokenKind::SignedKeyword || kind == TokenKind::UnsignedKeyword;
}

static PortParsed parse_port(const std::string& raw, const FormatOptions& opts) {
    PortParsed r;
    std::string s = raw;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t p = 0;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t'))
        ++p;
    r.indent = s.substr(0, p);
    auto [code, comment] = split_code_line_comment_tokenized(s.substr(p));
    r.comment = std::move(comment);
    if (!code.empty() && (code.back() == ',' || code.back() == ';')) {
        r.terminator = std::string(1, code.back());
        code.pop_back();
        while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
            code.pop_back();
    }

    std::vector<std::string> toks =
        group_scope_resolution_tokens(group_bracket_tokens(code_tokens_for_alignment(code)));
    if (toks.empty())
        return r;

    if (!is_port_dir_text(toks[0]))
        return r;
    r.direction = toks[0];
    size_t idx = 1;

    // Check if string is a pure identifier (no commas or special chars)
    auto is_pure_id = [](const std::string& s) -> bool {
        if (s.empty() || (!std::isalpha((unsigned char)s[0]) && s[0] != '_'))
            return false;
        for (size_t k = 1; k < s.size(); ++k) {
            char c = s[k];
            if (c == ':' && k + 1 < s.size() && s[k + 1] == ':') {
                k += 1;
                continue;
            }
            if (!std::isalnum((unsigned char)c) && c != '_')
                return false;
        }
        return true;
    };

    if (idx < toks.size()) {
        const std::string& cand = toks[idx];
        bool pure_id = is_pure_id(cand);
        if (pure_id && cand[0] != '[' && !is_sign_qualifier_text(cand)) {
            bool is_builtin = is_port_builtin_type_text(cand);
            bool is_usertype = pure_id && idx + 1 < toks.size() && toks[idx + 1] != ",";
            if (is_builtin || is_usertype) {
                r.dtype = toks[idx++];
                if (is_port_net_type_text(cand) && idx < toks.size()) {
                    if (is_port_data_type_text(toks[idx])) {
                        r.dtype += " " + toks[idx++];
                    }
                }
            }
        }
    }
    if (idx < toks.size() && is_sign_qualifier_text(toks[idx]))
        r.qualifier = toks[idx++];
    if (idx < toks.size() && !toks[idx].empty() && toks[idx][0] == '[') {
        int depth = 0;
        while (idx < toks.size()) {
            auto& t = toks[idx];
            r.dim += t;
            for (char ch : t) {
                if (ch == '[')
                    ++depth;
                else if (ch == ']')
                    --depth;
            }
            ++idx;
            if (depth <= 0)
                break;
        }
    }
    if (idx >= toks.size())
        return r;

    // Remaining tokens are comma-separated names (possibly with unpacked dims/defaults)
    // Rebuild remaining as string and split by top-level comma
    std::string remaining;
    for (size_t k = idx; k < toks.size(); ++k) {
        if (k > idx)
            remaining += ' ';
        remaining += toks[k];
    }
    auto raw_names = split_top_level(remaining);
    if (raw_names.empty())
        return r;

    // Split "name trailing..." into (name, trailing)
    auto split_id_trail = [](const std::string& nm) -> std::pair<std::string, std::string> {
        size_t i = 0;
        while (i < nm.size() && (std::isalnum((unsigned char)nm[i]) || nm[i] == '_'))
            ++i;
        std::string name = nm.substr(0, i);
        while (i < nm.size() && (nm[i] == ' ' || nm[i] == '\t'))
            ++i;
        return {name, nm.substr(i)};
    };

    for (auto& rn : raw_names) {
        // trim
        size_t a = 0;
        while (a < rn.size() && (rn[a] == ' ' || rn[a] == '\t'))
            ++a;
        size_t b = rn.size();
        while (b > a && (rn[b - 1] == ' ' || rn[b - 1] == '\t'))
            --b;
        std::string nm = rn.substr(a, b - a);
        auto [name, trail] = split_id_trail(nm);
        if (!name.empty()) {
            r.names.push_back({name, normalize_bracket_spacing(trail, opts)});
        } else {
            r.names.push_back({nm, ""});
        }
    }
    r.dim = normalize_bracket_spacing(r.dim, opts);
    r.valid = !r.names.empty();
    return r;
}

static std::vector<std::string> align_port_pass_legacy(std::vector<std::string> lines,
                                                   const FormatOptions& opts) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);
    std::vector<std::string> out;
    size_t i = 0;
    bool module_header_region = false;
    auto starts_with_port_dir = [](const std::string& line) -> bool {
        auto toks = significant_tokens(line);
        if (toks.empty())
            return false;
        return is_port_direction_token(toks[0]);
    };
    auto is_port_decl_line = [&](int idx) -> bool {
        if (idx < 0 || idx >= (int)lines.size())
            return false;
        if (starts_with_port_dir(lines[idx]))
            return true;
        return false;
    };
    auto is_semi_only = [&](int idx) -> bool {
        if (idx < 0 || idx >= (int)lines.size())
            return false;
        auto toks = significant_tokens(lines[idx]);
        return toks.size() == 1 && tok_is(toks[0], ";", TokenKind::Semicolon);
    };
    auto is_standalone_comment = [&](const std::string& text) {
        for (const auto& tok : collect_lexer_tokens(text)) {
            if (tok_whitespace(tok))
                continue;
            return tok_comment(tok);
        }
        return false;
    };
    auto has_header_end = [](const std::string& line) {
        auto toks = significant_tokens(line);
        bool saw_close = false;
        for (const auto& tok : toks) {
            if (tok_is(tok, ")", TokenKind::CloseParenthesis))
                saw_close = true;
            else if (saw_close && tok_is(tok, ";", TokenKind::Semicolon))
                return true;
        }
        return false;
    };
    auto has_kind = [](const std::string& line, TokenKind kind) {
        for (const auto& tok : significant_tokens(line))
            if (tok.kind == kind)
                return true;
        return false;
    };
    auto is_header_start_line = [&](const std::string& line) {
        auto toks = significant_tokens(line);
        if (toks.empty())
            return false;
        bool starts_header = toks[0].kind == TokenKind::ModuleKeyword ||
                             toks[0].kind == TokenKind::MacromoduleKeyword ||
                             toks[0].kind == TokenKind::InterfaceKeyword ||
                             toks[0].kind == TokenKind::ProgramKeyword ||
                             toks[0].kind == TokenKind::TaskKeyword ||
                             toks[0].kind == TokenKind::FunctionKeyword;
        if (!starts_header || has_header_end(line))
            return false;
        return has_kind(line, TokenKind::OpenParenthesis) || !has_kind(line, TokenKind::Semicolon) ||
               has_kind(line, TokenKind::ImportKeyword);
    };
    while (i < lines.size()) {
        if (in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i])) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        if (module_header_region) {
            out.push_back(lines[i]);
            if (has_header_end(lines[i]))
                module_header_region = false;
            ++i;
            continue;
        }
        if (is_header_start_line(lines[i])) {
            module_header_region = true;
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        if (!is_port_decl_line((int)i)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        struct PortBlkEntry {
            std::string orig;
            std::string port_text;
            PortParsed parsed;
        };
        std::vector<PortBlkEntry> blk;
        size_t j = i;
        while (j < lines.size()) {
            if (line_has_pp_conditional(lines[j]))
                break;
            if (!is_port_decl_line((int)j)) {
                if (significant_tokens(lines[j]).empty() || is_standalone_comment(lines[j])) {
                    blk.push_back({lines[j], lines[j], PortParsed{}});
                    ++j;
                    continue;
                }
                break;
            }
            std::string fl = lines[j];
            std::string port_line = lines[j];
            ++j;
            if (is_semi_only((int)j)) {
                port_line += ";";
                ++j;
            }
            blk.push_back({fl, port_line, parse_port(port_line, opts)});
        }

        int md = 0, ms2_content = 0, mdim = 0;
        bool has_qualified_type = false;
        int np = 0;
        size_t max_slots = 0;
        for (auto& e : blk) {
            if (!e.parsed.valid)
                continue;
            ++np;
            md = std::max(md, (int)e.parsed.direction.size());
            std::string s2 = e.parsed.dtype + (e.parsed.qualifier.empty() ? "" : " " + e.parsed.qualifier);
            ms2_content = std::max(ms2_content, (int)s2.size());
            for (const auto& tok : significant_tokens(s2))
                has_qualified_type = has_qualified_type || tok.kind == TokenKind::DoubleColon;
            mdim = std::max(mdim, (int)e.parsed.dim.size());
            max_slots = std::max(max_slots, e.parsed.names.size());
        }
        if (!np) {
            for (auto& e : blk)
                out.push_back(e.orig);
            i = j;
            continue;
        }

        const auto& pd = opts.port_declaration;
        int pd_s1_min = tab_aligned_width(pd.section1_min_width, opts);
        int pd_s2_min = tab_aligned_width(pd.section2_min_width, opts);
        int pd_s3_min = tab_aligned_width(pd.section3_min_width, opts);
        int pd_s4_min = tab_aligned_width(pd.section4_min_width, opts);
        int pd_s5_min = tab_aligned_width(pd.section5_min_width, opts);

        int s1 = tab_aligned_width(std::max(pd_s1_min, md + 1), opts);
        int s2 = ms2_content > 0
                     ? tab_aligned_width(std::max(pd_s2_min,
                                                  ms2_content + (has_qualified_type ? 3 : 1)),
                                         opts)
                     : 0;
        int s3 = mdim > 0 ? tab_aligned_width(std::max(pd_s3_min, mdim + 1), opts) : 0;
        int s5_min = pd_s5_min;

        // Per-slot id and trailing widths
        std::vector<int> id_widths(max_slots, 0), trail_widths(max_slots, 0);
        for (size_t slot = 0; slot < max_slots; ++slot) {
            int max_id = 0, max_tr = 0;
            for (auto& e : blk) {
                if (!e.parsed.valid)
                    continue;
                if (slot < e.parsed.names.size()) {
                    max_id = std::max(max_id, (int)e.parsed.names[slot].first.size());
                    max_tr = std::max(max_tr, (int)e.parsed.names[slot].second.size());
                }
            }
            id_widths[slot] = tab_aligned_width(std::max(pd_s4_min, max_id + 1), opts);
            trail_widths[slot] = tab_aligned_width(std::max(s5_min, max_tr), opts);
        }

        auto pad = [](std::string s, int w) -> std::string {
            s.resize(std::max((int)s.size(), w), ' ');
            return s;
        };

        for (auto& e : blk) {
            const auto& pp = e.parsed;
            if (!pp.valid) {
                out.push_back(e.orig);
                continue;
            }
            int line_s1 = s1;
            int line_s2 = s2;
            int line_s3 = s3;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;
            if (opts.port_declaration.align_adaptive) {
                std::string tp = pp.dtype + (pp.qualifier.empty() ? "" : " " + pp.qualifier);
                // Cumulative minimum target end columns (fixed reference points).
                // Each section pads to max(target_end, actual_end).
                // Overflow on one line is absorbed by subsequent sections' slack,
                // so downstream sections stay aligned as long as overflow is small.
                int t1 = pd_s1_min;
                int t2 = t1 + (s2 > 0 ? pd_s2_min : 0);
                int t3 = t2 + (s3 > 0 ? pd_s3_min : 0);

                int e1 = tab_aligned_width(std::max(t1, (int)pp.direction.size() + 1), opts);
                line_s1 = e1;

                int e2 = e1;
                if (s2 > 0) {
                    int c2 = tp.empty() ? 0 : (int)tp.size() + 1;
                    e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                    line_s2 = e2 - e1;
                }

                if (s3 > 0) {
                    int c3 = pp.dim.empty() ? 0 : (int)pp.dim.size() + 1;
                    int e3 = tab_aligned_width(std::max(t3, e2 + c3), opts);
                    line_s3 = e3 - e2;
                }

                line_id_widths.clear();
                line_trail_widths.clear();
                for (const auto& [nm, tr] : pp.names) {
                    line_id_widths.push_back(
                        tab_aligned_width(std::max(pd_s4_min, (int)nm.size() + 1), opts));
                    line_trail_widths.push_back(
                        tab_aligned_width(std::max(s5_min, (int)tr.size()), opts));
                }
            }
            std::string line = pp.indent;
            line += pad(pp.direction, line_s1);
            if (line_s2 > 0) {
                std::string tp = pp.dtype + (pp.qualifier.empty() ? "" : " " + pp.qualifier);
                line += pad(tp, line_s2);
            }
            if (line_s3 > 0)
                line += pad(pp.dim, line_s3);

            // Emit per-slot names — mirrors Python _reassemble_port_line
            size_t nslots = pp.names.size();
            for (size_t slot = 0; slot < nslots; ++slot) {
                bool is_last = (slot == nslots - 1);
                const auto& nm = pp.names[slot].first;
                const auto& tr = pp.names[slot].second;
                // Pad name to slot id width
                if (slot < line_id_widths.size())
                    line += pad(nm, line_id_widths[slot]);
                else
                    line += nm;

                if (!is_last) {
                    // Non-last slot: pad trailing then ", "
                    if (!tr.empty() && s5_min > 0 && slot < line_trail_widths.size() &&
                        line_trail_widths[slot] > 1)
                        line += pad(tr, line_trail_widths[slot]) + ", ";
                    else if (slot < line_trail_widths.size())
                        line += pad(tr, line_trail_widths[slot]) + ", ";
                    else
                        line += tr + ", ";
                } else {
                    // Last slot — matches Python _reassemble_port_line last-slot logic
                    std::string term = pp.terminator.empty() ? ";" : pp.terminator;
                    if (!tr.empty() && s5_min > 0 && slot < line_trail_widths.size() &&
                        line_trail_widths[slot] > 1) {
                        line += pad(tr, line_trail_widths[slot]) + term;
                    } else {
                        if (slot < line_trail_widths.size())
                            line += pad(tr, line_trail_widths[slot]);
                        else
                            line += tr;
                        line += term;
                    }
                }
            }
            if (!pp.comment.empty())
                line += pp.comment;
            while (!line.empty() && line.back() == ' ')
                line.pop_back();
            out.push_back(line);
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Statement assignment alignment pass
// ---------------------------------------------------------------------------

static std::vector<std::string> align_assign_pass_legacy(std::vector<std::string> lines,
                                                     const FormatOptions& opts) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    auto find_op = [&](const std::string& line) -> std::pair<int, std::string> {
        static const std::vector<std::string> OPS = {"<<<=", ">>>=", "<<=", ">>=", "<=", "+=", "-=",
                                                     "*=",   "/=",   "%=",  "&=",  "|=", "^=", "="};
        int paren = 0;
        int bracket = 0;
        int brace = 0;
        for (const auto& tok : collect_lexer_tokens(line)) {
            if (tok_comment(tok))
                break;
            if (tok_whitespace(tok) || tok_directive(tok))
                continue;
            if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                ++paren;
            else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren > 0)
                --paren;
            else if (tok_is(tok, "[", TokenKind::OpenBracket))
                ++bracket;
            else if (tok_is(tok, "]", TokenKind::CloseBracket) && bracket > 0)
                --bracket;
            else if (tok_is(tok, "{", TokenKind::OpenBrace))
                ++brace;
            else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace > 0)
                --brace;
            if (paren == 0 && bracket == 0 && brace == 0 &&
                std::find(OPS.begin(), OPS.end(), tok_text(tok)) != OPS.end())
                return {tok_pos(tok), tok_text(tok)};
        }
        return {-1, ""};
    };

    auto is_var = [&](int line_idx) -> bool {
        auto toks = significant_tokens(lines[(size_t)line_idx]);
        if (toks.size() < 3 || !tok_is(toks.back(), ";", TokenKind::Semicolon))
            return false;
        if (is_port_direction_token(toks[0]))
            return false;
        size_t idx = 0;
        while (idx < toks.size() && is_var_prefix_token(toks[idx]))
            ++idx;
        if (idx >= toks.size())
            return false;
        if (is_var_builtin_type_token(toks[idx]))
            return true;
        return is_identifier(toks[idx]) && !is_keyword(toks[idx]) && idx + 1 < toks.size() &&
               (is_identifier(toks[idx + 1]) ||
                tok_is(toks[idx + 1], "[", TokenKind::OpenBracket) ||
                is_sign_qualifier_token(toks[idx + 1]));
    };
    auto starts_with_kind = [](const std::string& line, TokenKind kind) -> bool {
        for (const auto& tok : collect_lexer_tokens(line)) {
            if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
                continue;
            return tok.kind == kind;
        }
        return false;
    };
    auto starts_module_header = [](const std::string& line) -> bool {
        auto toks = significant_tokens(line);
        if (toks.empty() ||
            (toks[0].kind != TokenKind::ModuleKeyword &&
             toks[0].kind != TokenKind::InterfaceKeyword &&
             toks[0].kind != TokenKind::ProgramKeyword))
            return false;
        for (size_t i = 1; i + 1 < toks.size(); ++i) {
            if (tok_is(toks[i], "#", TokenKind::Hash) &&
                tok_is(toks[i + 1], "(", TokenKind::OpenParenthesis))
                return true;
        }
        return false;
    };
    auto is_module_header_parameter = [&](int line_idx) -> bool {
        if (!starts_with_kind(lines[(size_t)line_idx], TokenKind::ParameterKeyword))
            return false;
        for (int k = line_idx - 1; k >= 0; --k) {
            std::string trimmed = trim_left_copy(trim_right_copy(lines[(size_t)k]));
            if (trimmed.empty())
                continue;
            if (starts_module_header(trimmed))
                return true;
            auto toks = significant_tokens(trimmed);
            bool has_semicolon = false;
            bool has_close_open = false;
            for (size_t ti = 0; ti < toks.size(); ++ti) {
                has_semicolon = has_semicolon || tok_is(toks[ti], ";", TokenKind::Semicolon);
                if (ti + 1 < toks.size() && tok_is(toks[ti], ")", TokenKind::CloseParenthesis) &&
                    tok_is(toks[ti + 1], "(", TokenKind::OpenParenthesis))
                    has_close_open = true;
            }
            if (has_semicolon || has_close_open)
                return false;
        }
        return false;
    };

    std::vector<std::string> out;
    size_t i = 0;
    while (i < lines.size()) {
        if (in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i])) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        auto [p0, op0] = find_op(lines[i]);
        if (p0 < 0 || is_var((int)i) || is_module_header_parameter((int)i)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        size_t ind = 0;
        while (ind < lines[i].size() &&
               (lines[i][ind] == ' ' || lines[i][ind] == '\t'))
            ++ind;
        if (i > 0 && starts_with_kind(lines[i - 1], TokenKind::ForKeyword)) {
        size_t j = i;
        while (j < lines.size()) {
            const auto& lj = lines[j];
            size_t ij = 0;
            while (ij < lj.size() && (lj[ij] == ' ' || lj[ij] == '\t'))
                ++ij;
            if (in_disabled(line_starts[j], disabled) || line_has_pp_conditional(lj) || ij != ind || is_var((int)j) ||
                    is_module_header_parameter((int)j) || find_op(lj).first < 0)
                    break;
                out.push_back(lines[j]);
                ++j;
            }
            i = j;
            continue;
        }

        struct E {
            std::string line;
            int pos;
            std::string op;
            int lw;
        };
        std::vector<E> grp;
        size_t j = i;
        while (j < lines.size()) {
            const auto& lj = lines[j];
            if (lj.empty())
                break;
            if (in_disabled(line_starts[j], disabled) || line_has_pp_conditional(lj))
                break;
            if (is_var((int)j) || is_module_header_parameter((int)j))
                break;
            size_t ij = 0;
            while (ij < lj.size() && (lj[ij] == ' ' || lj[ij] == '\t'))
                ++ij;
            if (ij != ind)
                break;
            auto [pj, oj] = find_op(lj);
            if (pj < 0)
                break;
            std::string lhs = trim_right_copy(lj.substr(0, (size_t)pj));
            grp.push_back({lj, pj, oj, (int)(lhs.size() - ij)});
            ++j;
        }
        if (grp.empty()) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        int mx = opts.statement.lhs_min_width;
        for (auto& e : grp)
            mx = std::max(mx, e.lw);
        int col = mx + 1;
        if (opts.tab_align && opts.indent_size > 0)
            col = snap_to_indent_grid((int)ind + mx + 1, opts.indent_size) - (int)ind;
        for (auto& e : grp) {
            int sp = std::max(1, col - e.lw);
            std::string lhs = trim_right_copy(e.line.substr(0, (size_t)e.pos));
            size_t rs = (size_t)(e.pos + (int)e.op.size());
            std::string rhs = (rs < e.line.size()) ? trim_left_copy(e.line.substr(rs)) : "";
            out.push_back(lhs + std::string(sp, ' ') + e.op + " " + rhs);
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Variable declaration alignment pass — ported from _align_variable_declarations_pass
// ---------------------------------------------------------------------------

struct VarParsed {
    std::string indent, type_kw, qualifier, dim;
    std::vector<std::pair<std::string, std::string>> declarators; // (name, trailing)
    std::string comment;
};

static VarParsed* parse_var_line(const std::string& line, const FormatOptions& opts) {
    std::string stripped = line;
    while (!stripped.empty() && (stripped.back() == ' ' || stripped.back() == '\t'))
        stripped.pop_back();
    size_t ip = 0;
    while (ip < stripped.size() && (stripped[ip] == ' ' || stripped[ip] == '\t'))
        ++ip;
    std::string indent = stripped.substr(0, ip);
    auto [code, comment] = split_code_line_comment_tokenized(stripped.substr(ip));

    // Must end with ;
    if (code.empty() || code.back() != ';')
        return nullptr;
    code.pop_back();
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
        code.pop_back();

    std::vector<std::string> toks = group_bracket_tokens(code_tokens_for_alignment(code));
    if (toks.empty())
        return nullptr;
    auto sig_toks = significant_tokens(code);
    if (sig_toks.empty())
        return nullptr;

    if (is_port_direction_token(sig_toks[0]))
        return nullptr;

    size_t idx = 0;
    std::vector<std::string> type_parts;
    while (idx < toks.size() && idx < sig_toks.size() && is_var_prefix_token(sig_toks[idx])) {
        type_parts.push_back(toks[idx++]);
    }
    if (idx >= toks.size())
        return nullptr;

    if (idx < sig_toks.size() && is_var_builtin_type_token(sig_toks[idx])) {
        type_parts.push_back(toks[idx++]);
    } else {
        // User-defined type: must be an identifier not an SV keyword
        if (idx >= sig_toks.size() || !is_identifier(sig_toks[idx]) || is_keyword(sig_toks[idx]))
            return nullptr;
        if (idx + 1 >= toks.size())
            return nullptr;
        // Next must look like dimension, qualifier, or identifier
        bool ok = idx + 1 < sig_toks.size() &&
                  (tok_is(sig_toks[idx + 1], "[", TokenKind::OpenBracket) ||
                   is_identifier(sig_toks[idx + 1]) || is_sign_qualifier_token(sig_toks[idx + 1]));
        if (!ok)
            return nullptr;
        type_parts.push_back(toks[idx++]);
    }

    std::string type_kw;
    for (size_t k = 0; k < type_parts.size(); ++k) {
        if (k)
            type_kw += ' ';
        type_kw += type_parts[k];
    }

    // Optional qualifier
    std::string qualifier;
    if (idx < sig_toks.size() && is_sign_qualifier_token(sig_toks[idx]))
        qualifier = toks[idx++];

    // Optional packed dim
    std::string dim;
    if (idx < toks.size() && !toks[idx].empty() && toks[idx][0] == '[') {
        int depth = 0;
        while (idx < toks.size()) {
            dim += toks[idx];
            for (char c : toks[idx]) {
                if (c == '[')
                    ++depth;
                else if (c == ']')
                    --depth;
            }
            ++idx;
            if (depth <= 0)
                break;
        }
    }
    if (idx >= toks.size())
        return nullptr;

    // Remaining: comma-separated declarators
    std::string remaining;
    for (size_t k = idx; k < toks.size(); ++k) {
        if (k > idx)
            remaining += ' ';
        remaining += toks[k];
    }

    auto raw_names = split_top_level(remaining);
    if (raw_names.empty())
        return nullptr;

    // Validate: each name must start with [A-Za-z_]
    for (auto& rn : raw_names) {
        size_t a = 0;
        while (a < rn.size() && (rn[a] == ' ' || rn[a] == '\t'))
            ++a;
        if (a >= rn.size())
            continue;
        char fc = rn[a];
        if (!std::isalpha((unsigned char)fc) && fc != '_')
            return nullptr;
    }

    auto* vp = new VarParsed{indent, type_kw, qualifier, normalize_bracket_spacing(dim, opts),
                             {},     comment};
    for (auto& rn : raw_names) {
        size_t a = 0;
        while (a < rn.size() && (rn[a] == ' ' || rn[a] == '\t'))
            ++a;
        size_t b = rn.size();
        while (b > a && (rn[b - 1] == ' ' || rn[b - 1] == '\t'))
            --b;
        std::string nm = rn.substr(a, b - a);
        size_t ni = 0;
        while (ni < nm.size() && (std::isalnum((unsigned char)nm[ni]) || nm[ni] == '_'))
            ++ni;
        std::string dname = nm.substr(0, ni);
        while (ni < nm.size() && (nm[ni] == ' ' || nm[ni] == '\t'))
            ++ni;
        vp->declarators.push_back({dname, normalize_bracket_spacing(nm.substr(ni), opts)});
    }
    if (vp->declarators.empty()) {
        delete vp;
        return nullptr;
    }
    // Reject if any declarator looks like a function/instance call (has '(' in name or trailing)
    for (auto& decl : vp->declarators) {
        bool has_call_paren = false;
        for (const auto& tok : significant_tokens(decl.first + decl.second)) {
            if (tok_is(tok, "(", TokenKind::OpenParenthesis)) {
                has_call_paren = true;
                break;
            }
        }
        if (has_call_paren) {
            delete vp;
            return nullptr;
        }
    }
    return vp;
}

static std::vector<std::string> align_var_pass_legacy(std::vector<std::string> lines,
                                                  const FormatOptions& opts) {
    const auto& vo = opts.var_declaration;
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    auto is_var_idx = [&](int idx) -> bool {
        VarParsed* parsed = parse_var_line(lines[(size_t)idx], opts);
        if (!parsed)
            return false;
        delete parsed;
        return true;
    };

    std::vector<std::string> out;
    size_t i = 0;
    while (i < lines.size()) {
        // Enter a block only if this line is a variable declaration.
        if (in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i]) ||
            !is_var_idx((int)i)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        // Collect block
        struct BlkEntry {
            std::string orig;
            VarParsed* parsed;
        };
        std::vector<BlkEntry> block;
        size_t j = i;
        while (j < lines.size()) {
            const std::string& cur = lines[j];
            if (in_disabled(line_starts[j], disabled) || line_has_pp_conditional(cur))
                break;
            if (is_var_idx((int)j)) {
                block.push_back({lines[j], parse_var_line(cur, opts)});
                ++j;
                continue;
            }
            // Comment/blank lines pass through without breaking block
            bool comment_or_blank = true;
            for (const auto& tok : collect_lexer_tokens(cur)) {
                if (tok_whitespace(tok))
                    continue;
                comment_or_blank = tok_comment(tok);
                break;
            }
            if (comment_or_blank) {
                block.push_back({lines[j], nullptr});
                ++j;
                continue;
            }
            break;
        }

        // Count parseable entries
        int np = 0;
        for (auto& e : block)
            if (e.parsed)
                ++np;

        if (np <= 0) {
            for (auto& e : block) {
                out.push_back(e.orig);
                if (e.parsed)
                    delete e.parsed;
            }
            i = j;
            continue;
        }

        // Compute section widths
        int max_s1 = 0;
        for (auto& e : block) {
            if (!e.parsed)
                continue;
            std::string s1 =
                e.parsed->type_kw + (e.parsed->qualifier.empty() ? "" : " " + e.parsed->qualifier);
            max_s1 = std::max(max_s1, (int)s1.size());
        }
        int vo_s1_min = tab_aligned_width(vo.section1_min_width, opts);
        int vo_s2_min = tab_aligned_width(vo.section2_min_width, opts);
        int vo_s3_min = tab_aligned_width(vo.section3_min_width, opts);
        int vo_s4_min = tab_aligned_width(vo.section4_min_width, opts);

        int s1_w = tab_aligned_width(std::max(vo_s1_min, max_s1 + 1), opts);

        int max_dim = 0;
        for (auto& e : block)
            if (e.parsed)
                max_dim = std::max(max_dim, (int)e.parsed->dim.size());
        int s2_w = max_dim > 0 ? tab_aligned_width(std::max(vo_s2_min, max_dim + 1), opts) : 0;

        size_t max_slots = 0;
        for (auto& e : block)
            if (e.parsed)
                max_slots = std::max(max_slots, e.parsed->declarators.size());

        std::vector<int> id_widths(max_slots, 0), trail_widths(max_slots, 0);
        for (size_t slot = 0; slot < max_slots; ++slot) {
            int mx_id = 0, mx_tr = 0;
            for (auto& e : block) {
                if (!e.parsed)
                    continue;
                if (slot < e.parsed->declarators.size()) {
                    mx_id = std::max(mx_id, (int)e.parsed->declarators[slot].first.size());
                    mx_tr = std::max(mx_tr, (int)e.parsed->declarators[slot].second.size());
                }
            }
            id_widths[slot] = tab_aligned_width(std::max(vo_s3_min, mx_id + 1), opts);
            trail_widths[slot] = tab_aligned_width(std::max(vo_s4_min, mx_tr), opts);
        }

        auto pad_to = [](std::string s, int w) -> std::string {
            if ((int)s.size() < w)
                s.resize(w, ' ');
            return s;
        };

        for (auto& e : block) {
            if (!e.parsed) {
                out.push_back(e.orig);
                continue;
            }
            const auto& vp = *e.parsed;
            int line_s1_w = s1_w;
            int line_s2_w = s2_w;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;
            if (vo.align_adaptive) {
                std::string s1part = vp.type_kw + (vp.qualifier.empty() ? "" : " " + vp.qualifier);
                // Cumulative minimum target end columns — same model as port_declaration.
                int t1 = vo_s1_min;
                int t2 = t1 + (s2_w > 0 ? vo_s2_min : 0);

                int e1 = tab_aligned_width(std::max(t1, (int)s1part.size() + 1), opts);
                line_s1_w = e1;

                int e2 = e1;
                if (s2_w > 0) {
                    int c2 = vp.dim.empty() ? 0 : (int)vp.dim.size() + 1;
                    e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                    line_s2_w = e2 - e1;
                }

                // Keep declarator/trailing widths block-wide so a name or initializer that
                // overflows its minimum section still moves following semicolons consistently for
                // the block.
                line_trail_widths.clear();
                for (const auto& [_, tr] : vp.declarators)
                    line_trail_widths.push_back(
                        tab_aligned_width(std::max(vo_s4_min, (int)tr.size()), opts));
            }
            std::string ln = vp.indent;
            std::string s1part = vp.type_kw + (vp.qualifier.empty() ? "" : " " + vp.qualifier);
            ln += pad_to(s1part, line_s1_w);
            if (line_s2_w > 0)
                ln += pad_to(vp.dim, line_s2_w);
            size_t nd = vp.declarators.size();
            for (size_t k = 0; k < nd; ++k) {
                bool is_last = (k == nd - 1);
                const auto& nm = vp.declarators[k].first;
                const auto& tr = vp.declarators[k].second;
                if (!is_last) {
                    ln += pad_to(nm, line_id_widths[k]);
                    if (!tr.empty() && vo.section4_min_width > 0 && k < line_trail_widths.size() &&
                        line_trail_widths[k] > 1) {
                        ln += pad_to(tr, line_trail_widths[k]) + ", ";
                    } else if (k < line_trail_widths.size()) {
                        ln += pad_to(tr, line_trail_widths[k]) + ", ";
                    } else {
                        ln += tr + ", ";
                    }
                } else {
                    // Last slot — mirrors Python _reassemble_var_line last-slot logic
                    if (k < line_id_widths.size())
                        ln += pad_to(nm, line_id_widths[k]);
                    else
                        ln += nm;
                    if (!tr.empty() && vo.section4_min_width > 0 && k < line_trail_widths.size() &&
                        line_trail_widths[k] > 1) {
                        ln += pad_to(tr, line_trail_widths[k]) + ";";
                    } else {
                        if (k < line_trail_widths.size())
                            ln += pad_to(tr, line_trail_widths[k]);
                        ln += tr + ";";
                    }
                }
            }
            if (!vp.comment.empty())
                ln += vp.comment;
            while (!ln.empty() && ln.back() == ' ')
                ln.pop_back();
            out.push_back(ln);
            delete e.parsed;
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Module instantiation expansion pass
// ---------------------------------------------------------------------------

struct InstanceComments {
    std::string header;
    std::string param_comment; // line comment inside #(...) param block
    std::vector<std::pair<std::string, std::string>> leading_port_comments;
    std::vector<std::pair<std::string, std::string>> port_comments;
    std::vector<std::string> footer_comments;
    std::string trailing; // comment after closing );
    bool preserve_original{false};
};

static size_t find_line_comment_start(const std::string& line) {
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        char ch = line[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '/' && line[i + 1] == '/')
            return i;
    }
    return std::string::npos;
}

static std::string last_named_port_before_comment(const std::vector<Tok>& toks) {
    std::string last;
    for (size_t i = 0; i + 2 < toks.size(); ++i) {
        if (tok_whitespace(toks[i]) || tok_comment(toks[i]) || tok_directive(toks[i]) || tok_text(toks[i]) != ".")
            continue;
        size_t name_i = i + 1;
        while (name_i < toks.size() && tok_whitespace(toks[name_i]))
            ++name_i;
        if (name_i >= toks.size() || !is_identifier(toks[name_i]))
            continue;
        size_t open_i = name_i + 1;
        while (open_i < toks.size() && tok_whitespace(toks[open_i]))
            ++open_i;
        if (open_i < toks.size() && tok_is(toks[open_i], "(", TokenKind::OpenParenthesis))
            last = tok_text(toks[name_i]);
    }
    return last;
}

static std::string last_named_port_before_comment(const std::string& code) {
    return last_named_port_before_comment(collect_lexer_tokens(code));
}

static std::string first_named_port_in_code(const std::vector<Tok>& toks) {
    for (size_t i = 0; i + 2 < toks.size(); ++i) {
        if (tok_whitespace(toks[i]) || tok_comment(toks[i]) || tok_directive(toks[i]) || tok_text(toks[i]) != ".")
            continue;
        size_t name_i = i + 1;
        while (name_i < toks.size() && tok_whitespace(toks[name_i]))
            ++name_i;
        if (name_i >= toks.size() || !is_identifier(toks[name_i]))
            continue;
        size_t open_i = name_i + 1;
        while (open_i < toks.size() && tok_whitespace(toks[open_i]))
            ++open_i;
        if (open_i < toks.size() && tok_is(toks[open_i], "(", TokenKind::OpenParenthesis))
            return tok_text(toks[name_i]);
    }
    return "";
}

static std::string first_named_port_in_code(const std::string& code) {
    return first_named_port_in_code(collect_lexer_tokens(code));
}

// Collect lines from start until ')' at depth 0 followed by ';'
static bool collect_instance(const std::vector<std::string>& lines, size_t start, size_t& end_i,
                             std::string& flat, InstanceComments& comments) {
    std::vector<std::string> parts;
    int depth = 0;
    int param_depth = 0;
    size_t j = start;
    while (j < lines.size()) {
        std::string stripped = lines[j];
        size_t sp = 0;
        while (sp < stripped.size() && (stripped[sp] == ' ' || stripped[sp] == '\t'))
            ++sp;
        stripped = stripped.substr(sp);
        std::string code = stripped;
        size_t line_comment = std::string::npos;
        auto stripped_toks = collect_lexer_tokens(stripped);
        for (const auto& tok : stripped_toks) {
            if (tok_comment(tok) && starts_with_chars(tok_text(tok), '/', '/')) {
                line_comment = (size_t)tok_pos(tok);
                code = stripped.substr(0, line_comment);
                break;
            }
        }
        auto code_toks = line_comment == std::string::npos ? std::move(stripped_toks)
                                                           : collect_lexer_tokens(code);

        auto param_depth_after = [](int start_depth, const std::vector<Tok>& toks) {
            int pd = start_depth;
            bool saw_hash = false;
            for (const auto& tok : toks) {
                if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
                    continue;
                if (pd > 0) {
                    if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                        ++pd;
                    else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && pd > 0)
                        --pd;
                    continue;
                }
                if (tok_is(tok, "#", TokenKind::Hash)) {
                    saw_hash = true;
                } else if (saw_hash && tok_is(tok, "(", TokenKind::OpenParenthesis)) {
                    pd = 1;
                    saw_hash = false;
                } else if (saw_hash) {
                    saw_hash = false;
                }
            }
            return pd;
        };
        int next_param_depth = param_depth_after(param_depth, code_toks);

        if (line_comment != std::string::npos) {
            std::string comment = stripped.substr(line_comment);
            if (next_param_depth > 0) {
                // Comment inside #(...) param block — preserve order before closing )
                comments.preserve_original = true;
                if (comments.param_comment.empty())
                    comments.param_comment = comment;
                else
                    comments.param_comment += " " + comment;
            } else {
                // Check if this line closes the instance (; at depth 0)
                bool line_closes = false;
                {
                    int d = depth;
                    for (const auto& tok : code_toks) {
                        if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
                            continue;
                        if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                            ++d;
                        else if (tok_is(tok, ")", TokenKind::CloseParenthesis))
                            --d;
                        else if (tok_is(tok, ";", TokenKind::Semicolon) && d == 0) {
                            line_closes = true;
                            break;
                        }
                    }
                }
                if (line_closes) {
                    comments.trailing = comment;
                } else {
                    auto sig_toks = significant_tokens_from(code_toks);
                    std::string port = last_named_port_before_comment(code_toks);
                    if (!port.empty())
                        comments.port_comments.push_back({port, comment});
                    else if (sig_toks.empty())
                        code = comment;
                    else if (comments.header.empty())
                        comments.header = comment;
                }
            }
        }

        param_depth = next_param_depth;

        parts.push_back(code);
        for (const auto& tok : code_toks) {
            if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
                continue;
            if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(tok, ")", TokenKind::CloseParenthesis))
                --depth;
            else if (tok_is(tok, ";", TokenKind::Semicolon) && depth == 0) {
                end_i = j + 1;
                flat = "";
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k) {
                        bool prev_pp = line_has_pp_conditional(parts[k - 1]);
                        bool cur_pp = line_has_pp_conditional(parts[k]);
                        bool prev_comment = trim_left_copy(parts[k - 1]).rfind("//", 0) == 0;
                        bool cur_comment = trim_left_copy(parts[k]).rfind("//", 0) == 0;
                        flat += (prev_pp || cur_pp || prev_comment || cur_comment) ? '\n' : ' ';
                    }
                    flat += parts[k];
                }
                return true;
            }
        }
        ++j;
    }
    return false;
}

// Extract content of outermost (...) immediately before ;
static bool extract_port_list(const std::string& flat, std::string& port_list) {
    auto toks = significant_tokens(flat);
    if (toks.size() < 3 || !tok_is(toks.back(), ";", TokenKind::Semicolon))
        return false;
    if (!tok_is(toks[toks.size() - 2], ")", TokenKind::CloseParenthesis))
        return false;

    size_t close_i = toks.size() - 2;
    int depth = 1;
    size_t open_i = close_i;
    while (open_i > 0 && depth > 0) {
        --open_i;
        if (tok_is(toks[open_i], ")", TokenKind::CloseParenthesis))
            ++depth;
        else if (tok_is(toks[open_i], "(", TokenKind::OpenParenthesis))
            --depth;
    }
    if (depth != 0)
        return false;
    size_t start = (size_t)tok_pos(toks[open_i]) + tok_text(toks[open_i]).size();
    size_t end = (size_t)tok_pos(toks[close_i]);
    port_list = flat.substr(start, end - start);
    size_t a = 0;
    while (a < port_list.size() && std::isspace((unsigned char)port_list[a]))
        ++a;
    size_t b = port_list.size();
    while (b > a && std::isspace((unsigned char)port_list[b - 1]))
        --b;
    port_list = port_list.substr(a, b - a);
    return true;
}

static std::string take_leading_portlist_block_comments_tokenized(std::string& ports_str);

static std::string take_leading_portlist_block_comments(std::string& ports_str) {
    return take_leading_portlist_block_comments_tokenized(ports_str);
}

static void append_comment(std::string& dst, const std::string& comment) {
    if (dst.empty())
        dst = comment;
    else
        dst += " " + comment;
}

static void remove_block_comments_from_instance_port_list(std::string& port_list,
                                                          InstanceComments& comments) {
    std::string code;
    int paren_depth = 0;
    size_t cursor = 0;
    auto toks = collect_lexer_tokens(port_list);
    for (size_t ti = 0; ti < toks.size(); ++ti) {
        const auto& tok = toks[ti];
        size_t tok_start = (size_t)tok_pos(tok);
        size_t tok_end = tok_start + tok_text(tok).size();
        if (tok_start > cursor)
            code += port_list.substr(cursor, tok_start - cursor);

        if (tok_comment(tok) && starts_with_chars(tok_text(tok), '/', '*') && paren_depth == 0) {
            size_t next = ti + 1;
            while (next < toks.size() && tok_whitespace(toks[next]))
                ++next;
            if (next < toks.size() && tok_is(toks[next], "(", TokenKind::OpenParenthesis)) {
                code += tok_text(tok);
                cursor = tok_end;
                continue;
            }
            std::string port = last_named_port_before_comment(code);
            if (!port.empty()) {
                if (!comments.port_comments.empty() && comments.port_comments.back().first == port)
                    append_comment(comments.port_comments.back().second, tok_text(tok));
                else
                    comments.port_comments.push_back({port, tok_text(tok)});
            } else {
                append_comment(comments.header, tok_text(tok));
            }
            cursor = tok_end;
            continue;
        }

        code += tok_text(tok);
        cursor = tok_end;
        if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
            continue;
        if (tok_is(tok, "(", TokenKind::OpenParenthesis))
            ++paren_depth;
        else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren_depth > 0)
            --paren_depth;
    }
    if (cursor < port_list.size())
        code += port_list.substr(cursor);
    port_list = code;
}

// Parse named port connections .name(signal), ...
// Returns false if positional
static std::string trim_copy(const std::string& s);

struct InstancePortEntry {
    bool directive{false};
    bool comment{false};
    std::string text;
    std::string port;
    std::string sig;
};

static bool parse_named_ports(const std::string& port_list,
                              std::vector<InstancePortEntry>& entries) {
    auto toks = collect_lexer_tokens(port_list);
    size_t i = 0;
    auto skip_ws_commas = [&]() {
        while (i < toks.size() &&
               (tok_whitespace(toks[i]) || tok_is(toks[i], ",", TokenKind::Comma)))
            ++i;
    };

    while (true) {
        skip_ws_commas();
        if (i >= toks.size())
            break;
        if (tok_directive(toks[i]) || is_pp_conditional_text(tok_text(toks[i]))) {
            size_t directive_start = (size_t)tok_pos(toks[i]);
            size_t directive_end = port_list.find('\n', directive_start);
            if (directive_end == std::string::npos)
                directive_end = port_list.size();
            entries.push_back(
                {true, false,
                 trim_copy(port_list.substr(directive_start, directive_end - directive_start)), "",
                 ""});
            while (i < toks.size() && (size_t)tok_pos(toks[i]) < directive_end)
                ++i;
            continue;
        }
        if (tok_comment(toks[i])) {
            entries.push_back({false, true, trim_copy(tok_text(toks[i])), "", ""});
            ++i;
            continue;
        }
        if (tok_text(toks[i]) != ".")
            return false; // positional
        ++i;
        while (i < toks.size() && tok_whitespace(toks[i]))
            ++i;
        if (i >= toks.size() || !is_identifier(toks[i]))
            return false;
        std::string port_name = tok_text(toks[i]);
        ++i;
        while (i < toks.size() && tok_whitespace(toks[i]))
            ++i;
        while (i < toks.size() && tok_comment(toks[i]) && starts_with_chars(tok_text(toks[i]), '/', '*')) {
            port_name += " " + tok_text(toks[i]);
            ++i;
            while (i < toks.size() && tok_whitespace(toks[i]))
                ++i;
        }
        if (i >= toks.size() || !tok_is(toks[i], "(", TokenKind::OpenParenthesis))
            return false;
        size_t sig_start = (size_t)tok_pos(toks[i]) + tok_text(toks[i]).size();
        ++i;
        int depth = 1;
        size_t close_pos = std::string::npos;
        while (i < toks.size() && depth > 0) {
            if (tok_whitespace(toks[i]) || tok_comment(toks[i]) || tok_directive(toks[i])) {
                ++i;
                continue;
            }
            if (tok_is(toks[i], "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(toks[i], ")", TokenKind::CloseParenthesis) && --depth == 0) {
                close_pos = (size_t)tok_pos(toks[i]);
                ++i;
                break;
            }
            ++i;
        }
        if (close_pos == std::string::npos || close_pos < sig_start)
            return false;
        std::string sig = trim_copy(port_list.substr(sig_start, close_pos - sig_start));
        entries.push_back({false, false, "", port_name, sig});
    }
    for (const auto& entry : entries)
        if (!entry.directive && !entry.comment)
            return true;
    return false;
}

// Split flat into (module_type, param_block, inst_name) and identify the exact
// port-list parens belonging to that instance header.
static bool split_inst_parts(const std::string& flat, std::string& module_type,
                             std::string& param_block, std::string& inst_name,
                             std::string& inst_suffix, size_t& port_open,
                             size_t& port_close) {
    auto toks = significant_tokens(flat);
    size_t i = 0;
    if (i >= toks.size() || !is_identifier(toks[i]))
        return false;
    module_type = tok_text(toks[i++]);

    param_block = "";
        if (i < toks.size() && tok_is(toks[i], "#", TokenKind::Hash)) {
            size_t hash_i = i++;
            if (i >= toks.size() || !tok_is(toks[i], "(", TokenKind::OpenParenthesis))
                return false;
            int depth = 1;
            size_t close_i = i;
            while (++close_i < toks.size()) {
                if (tok_contains(toks[close_i], '('))
                    ++depth;
                else if (tok_contains(toks[close_i], ')') && --depth == 0)
                    break;
            }
        if (depth != 0)
            return false;
        size_t start = (size_t)tok_pos(toks[hash_i]);
        size_t end = (size_t)tok_pos(toks[close_i]) + tok_text(toks[close_i]).size();
        param_block = flat.substr(start, end - start);
        while (!param_block.empty() && (param_block.back() == ' ' || param_block.back() == '\t'))
            param_block.pop_back();
        i = close_i + 1;
    }

    if (i >= toks.size() || !is_identifier(toks[i]))
        return false;
    size_t inst_i = i++;
    inst_name = tok_text(toks[inst_i]);

    // Preserve unpacked instance dimensions between the instance name and
    // port list, e.g. "u_if[NUM_CH] ( ... )".
    size_t open_i = i;
    int bracket_depth = 0;
    while (open_i < toks.size()) {
        if (tok_is(toks[open_i], "[", TokenKind::OpenBracket))
            ++bracket_depth;
        else if (tok_is(toks[open_i], "]", TokenKind::CloseBracket) && bracket_depth > 0)
            --bracket_depth;
        else if (tok_is(toks[open_i], "(", TokenKind::OpenParenthesis) && bracket_depth == 0)
            break;
        ++open_i;
    }
    if (open_i >= toks.size())
        return false;

    int paren_depth = 1;
    size_t close_i = open_i;
    while (++close_i < toks.size()) {
        if (tok_is(toks[close_i], "(", TokenKind::OpenParenthesis))
            ++paren_depth;
        else if (tok_is(toks[close_i], ")", TokenKind::CloseParenthesis) &&
                 --paren_depth == 0)
            break;
    }
    if (paren_depth != 0 || close_i >= toks.size())
        return false;

    size_t after_close = close_i + 1;
    while (after_close < toks.size() && tok_comment(toks[after_close]))
        ++after_close;
    if (after_close >= toks.size() || !tok_is(toks[after_close], ";", TokenKind::Semicolon))
        return false;
    if (after_close + 1 != toks.size())
        return false;

    size_t suffix_start = (size_t)tok_pos(toks[inst_i]) + tok_text(toks[inst_i]).size();
    size_t suffix_end = (size_t)tok_pos(toks[open_i]);
    inst_suffix = flat.substr(suffix_start, suffix_end - suffix_start);
    size_t a = 0;
    while (a < inst_suffix.size() && (inst_suffix[a] == ' ' || inst_suffix[a] == '\t'))
        ++a;
    size_t b = inst_suffix.size();
    while (b > a && (inst_suffix[b - 1] == ' ' || inst_suffix[b - 1] == '\t'))
        --b;
    inst_suffix = inst_suffix.substr(a, b - a);
    port_open = (size_t)tok_pos(toks[open_i]);
    port_close = (size_t)tok_pos(toks[close_i]);
    return true;
}

static bool can_start_instance_candidate(const std::vector<Tok>& toks) {
    if (toks.size() < 3 || !is_identifier(toks[0]))
        return false;

    size_t i = 1;
    if (tok_is(toks[i], "#", TokenKind::Hash)) {
        ++i;
        if (i >= toks.size() || !tok_is(toks[i], "(", TokenKind::OpenParenthesis))
            return false;
        int depth = 1;
        while (++i < toks.size()) {
            if (tok_is(toks[i], "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(toks[i], ")", TokenKind::CloseParenthesis) && --depth == 0) {
                ++i;
                break;
            }
        }
        if (depth != 0)
            return false;
    }

    if (i >= toks.size() || !is_identifier(toks[i]))
        return false;
    ++i;

    int bracket_depth = 0;
    for (; i < toks.size(); ++i) {
        if (tok_is(toks[i], "[", TokenKind::OpenBracket)) {
            ++bracket_depth;
        } else if (tok_is(toks[i], "]", TokenKind::CloseBracket) && bracket_depth > 0) {
            --bracket_depth;
        } else if (tok_is(toks[i], "(", TokenKind::OpenParenthesis) && bracket_depth == 0) {
            return true;
        } else if (bracket_depth == 0) {
            return false;
        }
    }
    return false;
}

static std::vector<std::string> expand_instances_pass_legacy(std::vector<std::string> lines,
                                                        const FormatOptions& opts) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    const std::string port_indent(opts.instance.port_indent_level * opts.indent_size, ' ');
    int m_before = opts.instance.instance_port_name_width;
    int m_inside = opts.instance.instance_port_between_paren_width;
    bool adaptive = opts.instance.align_adaptive;

    std::vector<std::string> out;
    size_t i = 0;
    int pp_depth = 0;
    while (i < lines.size()) {
        const std::string& line = lines[i];
        std::string trimmed_line = trim_copy(line);
        bool pp_open = trimmed_line.rfind("`ifdef", 0) == 0 ||
                       trimmed_line.rfind("`ifndef", 0) == 0 ||
                       trimmed_line.rfind("`elsif", 0) == 0 ||
                       trimmed_line.rfind("`else", 0) == 0;
        bool pp_close = trimmed_line.rfind("`endif", 0) == 0;

        if (pp_depth > 0 || pp_open || pp_close ||
            in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i])) {
            out.push_back(lines[i]);
            if (trimmed_line.rfind("`ifdef", 0) == 0 || trimmed_line.rfind("`ifndef", 0) == 0)
                ++pp_depth;
            else if (pp_close && pp_depth > 0)
                --pp_depth;
            ++i;
            continue;
        }

        auto line_toks = significant_tokens(line);
        if (!can_start_instance_candidate(line_toks)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        // Extract leading whitespace
        std::string indent;
        size_t ii = 0;
        while (ii < line.size() && (line[ii] == ' ' || line[ii] == '\t'))
            indent += line[ii++];

        // Collect the full instantiation (may span lines) and parse
        size_t end_i;
        std::string flat;
        InstanceComments comments;
        if (!collect_instance(lines, i, end_i, flat, comments)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        if (comments.preserve_original) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        std::string module_type, param_block, inst_name, inst_suffix;
        size_t port_open = std::string::npos;
        size_t port_close = std::string::npos;
        if (!split_inst_parts(flat, module_type, param_block, inst_name, inst_suffix, port_open,
                              port_close)) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        if (port_open == std::string::npos || port_close == std::string::npos ||
            port_close < port_open) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }
        std::string port_list = trim_copy(flat.substr(port_open + 1, port_close - port_open - 1));
        std::string leading_comments = take_leading_portlist_block_comments(port_list);
        remove_block_comments_from_instance_port_list(port_list, comments);
        std::vector<InstancePortEntry> ports;
        if (!parse_named_ports(port_list, ports)) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }
        int max_port = 0, max_sig = 0;
        size_t port_count = 0;
        for (auto& entry : ports) {
            if (entry.directive || entry.comment)
                continue;
            ++port_count;
            max_port = std::max(max_port, (int)entry.port.size());
            max_sig = std::max(max_sig, (int)entry.sig.size());
        }
        int eff_before = std::max(1, m_before - max_port - 1);
        int eff_inside = std::max(0, m_inside - max_sig);
        if (!adaptive && opts.tab_align && opts.indent_size > 1) {
            auto snap = [&](int pos) { return snap_to_indent_grid(pos, opts.indent_size); };
            int base = (int)indent.size() + (int)port_indent.size() + 1 + max_port;
            int open_paren = snap(base + eff_before);
            eff_before = open_paren - base;
            int close_paren = snap(open_paren + 1 + max_sig + eff_inside);
            eff_inside = close_paren - open_paren - 1 - max_sig;
        }

        if (!comments.param_comment.empty() && !param_block.empty()) {
            // Split header: comment before param block closing ), then ) inst_name ( on next line
            size_t last_close = param_block.rfind(')');
            if (last_close != std::string::npos) {
                std::string param_inner = param_block.substr(0, last_close);
                while (!param_inner.empty() &&
                       (param_inner.back() == ' ' || param_inner.back() == '\t'))
                    param_inner.pop_back();
                out.push_back(indent + module_type + " " + param_inner + " " +
                              comments.param_comment);
            }
            std::string hdr = indent + ") " + inst_name + inst_suffix + " (";
            if (!leading_comments.empty())
                hdr += " " + leading_comments;
            if (!comments.header.empty())
                hdr += " " + comments.header;
            out.push_back(hdr);
        } else {
            std::string hdr = indent + module_type;
            if (!param_block.empty())
                hdr += " " + param_block;
            hdr += " " + inst_name + inst_suffix + " (";
            if (!leading_comments.empty())
                hdr += " " + leading_comments;
            if (!comments.header.empty())
                hdr += " " + comments.header;
            out.push_back(hdr);
        }
        size_t leading_comment_index = 0;
        size_t comment_index = 0;
        size_t port_index = 0;
        for (size_t k = 0; k < ports.size(); ++k) {
            auto& entry = ports[k];
            if (entry.directive) {
                out.push_back(indent + entry.text);
                continue;
            }
            if (entry.comment) {
                out.push_back(indent + port_indent + entry.text);
                continue;
            }
            const std::string& port = entry.port;
            const std::string& sig = entry.sig;
            while (leading_comment_index < comments.leading_port_comments.size() &&
                   comments.leading_port_comments[leading_comment_index].first == port) {
                out.push_back(indent + port_indent +
                              comments.leading_port_comments[leading_comment_index++].second);
            }
            std::string comma = (++port_index == port_count) ? "" : ",";
            std::string pline;
            if (adaptive) {
                int sb = std::max(1, m_before - (int)port.size() - 1);
                int si = std::max(0, m_inside - (int)sig.size());
                pline = indent + port_indent + "." + port + std::string(sb, ' ') + "(" + sig +
                        std::string(si, ' ') + ")" + comma;
            } else {
                std::string pname = port;
                pname.resize(std::max((int)pname.size(), max_port), ' ');
                std::string sname = sig;
                sname.resize(std::max((int)sname.size(), max_sig), ' ');
                pline = indent + port_indent + "." + pname + std::string(eff_before, ' ') + "(" +
                        sname + std::string(eff_inside, ' ') + ")" + comma;
            }
            while (!pline.empty() && pline.back() == ' ')
                pline.pop_back();
            if (comment_index < comments.port_comments.size() &&
                comments.port_comments[comment_index].first == port)
                pline += " " + comments.port_comments[comment_index++].second;
            out.push_back(pline);
        }
        for (const auto& comment : comments.footer_comments)
            out.push_back(indent + port_indent + comment);
        std::string close_line = indent + ");";
        if (!comments.trailing.empty())
            close_line += " " + comments.trailing;
        out.push_back(close_line);
        i = end_i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Function/task call formatting pass
// ---------------------------------------------------------------------------

static std::string trim_copy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        --b;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// Typedef enum and modport formatting passes
// ---------------------------------------------------------------------------

static bool collect_statement_lines(const std::vector<std::string>& lines, size_t start,
                                    size_t& end_i, std::string& flat) {
    if (start >= lines.size())
        return false;
    auto start_toks = significant_tokens(lines[start]);
    if (start_toks.empty() || line_has_pp_conditional(lines[start]))
        return false;
    if (is_indent_close(start_toks[0].kind) || start_toks[0].kind == TokenKind::BeginKeyword ||
        start_toks[0].kind == TokenKind::ForkKeyword || start_toks[0].kind == TokenKind::ElseKeyword)
        return false;
    {
        int paren = 0;
        int brace = 0;
        int bracket = 0;
        for (size_t ti = 0; ti < start_toks.size(); ++ti) {
            const auto& tok = start_toks[ti];
            if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                ++paren;
            else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren > 0)
                --paren;
            else if (tok_is(tok, "{", TokenKind::OpenBrace))
                ++brace;
            else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace > 0)
                --brace;
            else if (tok_is(tok, "[", TokenKind::OpenBracket))
                ++bracket;
            else if (tok_is(tok, "]", TokenKind::CloseBracket) && bracket > 0)
                --bracket;
            else if (tok_is(tok, ";", TokenKind::Semicolon))
                break;
            else if (paren == 0 && brace == 0 && bracket == 0 &&
                     tok_is(tok, ":", TokenKind::Colon) &&
                     (start_toks[0].kind == TokenKind::DefaultKeyword || ti == 1))
                return false;
        }
    }

    int paren = 0;
    int brace = 0;
    int bracket = 0;
    std::vector<std::string> parts;
    for (size_t i = start; i < lines.size(); ++i) {
        std::string trimmed = trim_copy(lines[i]);
        if (line_has_pp_conditional(trimmed))
            return false;
        parts.push_back(trimmed);
        for (const auto& tok : collect_lexer_tokens(trimmed)) {
            if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
                continue;
            if (tok_contains(tok, '('))
                ++paren;
            else if (tok_contains(tok, ')') && paren > 0)
                --paren;
            else if (tok_contains(tok, '{'))
                ++brace;
            else if (tok_contains(tok, '}') && brace > 0)
                --brace;
            else if (tok_contains(tok, '['))
                ++bracket;
            else if (tok_contains(tok, ']') && bracket > 0)
                --bracket;
            else if (tok_is(tok, ";", TokenKind::Semicolon) && paren == 0 && brace == 0 &&
                     bracket == 0) {
                end_i = i;
                flat.clear();
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k)
                        flat += ' ';
                    flat += parts[k];
                }
                return true;
            }
        }
    }
    return false;
}

struct EnumItemParsed {
    std::string name;
    std::string value;
    bool has_value{false};
};

static size_t find_top_level_equal(const std::string& text) {
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
            continue;
        if (tok_contains(tok, '('))
            ++paren;
        else if (tok_contains(tok, ')') && paren > 0)
            --paren;
        else if (tok_contains(tok, '['))
            ++bracket;
        else if (tok_contains(tok, ']') && bracket > 0)
            --bracket;
        else if (tok_contains(tok, '{'))
            ++brace;
        else if (tok_contains(tok, '}') && brace > 0)
            --brace;
        else if (tok_is(tok, "=", TokenKind::Unknown) && paren == 0 && bracket == 0 && brace == 0)
            return (size_t)tok_pos(tok);
    }
    return std::string::npos;
}

static std::string pad_right(std::string s, int width) {
    if ((int)s.size() < width)
        s.resize(width, ' ');
    return s;
}

static std::vector<std::string> format_enum_declaration_pass_legacy(std::vector<std::string> lines,
                                                               const FormatOptions& opts) {
    const auto& eo = opts.enum_declaration;
    const std::string enum_indent(opts.indent_size, ' ');

    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        auto first_line_toks = significant_tokens(line);
        if (first_line_toks.empty() || first_line_toks[0].kind != TokenKind::TypedefKeyword) {
            out.push_back(lines[i]);
            continue;
        }

        size_t end_i = i;
        std::string flat;
        if (!collect_statement_lines(lines, i, end_i, flat)) {
            out.push_back(lines[i]);
            continue;
        }

        std::string leading = line.substr(0, (size_t)tok_pos(first_line_toks[0]));
        auto toks = significant_tokens(flat);
        size_t typedef_i = 0;
        if (toks.empty() || toks[typedef_i].kind != TokenKind::TypedefKeyword) {
            out.push_back(lines[i]);
            continue;
        }
        size_t enum_i = typedef_i + 1;
        while (enum_i < toks.size() && toks[enum_i].kind != TokenKind::EnumKeyword)
            ++enum_i;
        if (enum_i >= toks.size()) {
            out.push_back(lines[i]);
            continue;
        }
        size_t open_i = enum_i + 1;
        while (open_i < toks.size() && !tok_is(toks[open_i], "{", TokenKind::OpenBrace))
            ++open_i;
        if (open_i >= toks.size()) {
            out.push_back(lines[i]);
            continue;
        }
        size_t open = (size_t)tok_pos(toks[open_i]);
        int depth = 1;
        size_t close_i = open_i;
        while (++close_i < toks.size()) {
            if (tok_contains(toks[close_i], '{'))
                ++depth;
            else if (tok_contains(toks[close_i], '}') && --depth == 0)
                break;
        }
        if (depth != 0 || close_i >= toks.size()) {
            out.push_back(lines[i]);
            continue;
        }
        size_t close = (size_t)tok_pos(toks[close_i]);
        size_t semi_i = close_i + 1;
        while (semi_i < toks.size() && !tok_is(toks[semi_i], ";", TokenKind::Semicolon))
            ++semi_i;
        if (semi_i >= toks.size()) {
            out.push_back(lines[i]);
            continue;
        }
        size_t semi = (size_t)tok_pos(toks[semi_i]);

        std::string prefix = trim_copy(flat.substr(0, open));
        std::string body = flat.substr(open + 1, close - open - 1);
        std::string suffix = trim_copy(flat.substr(close + 1, semi - close - 1));
        auto raw_items = split_top_level(body);
        std::vector<EnumItemParsed> items;
        for (auto& item : raw_items) {
            std::string t = trim_copy(item);
            if (t.empty())
                continue;
            size_t eq = find_top_level_equal(t);
            if (eq == std::string::npos) {
                items.push_back({t, "", false});
            } else {
                items.push_back({trim_copy(t.substr(0, eq)), trim_copy(t.substr(eq + 1)), true});
            }
        }
        if (items.empty()) {
            for (size_t k = i; k <= end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        int item_base_col = (int)leading.size() + opts.indent_size;
        int block_name_width = tab_aligned_width(eo.enum_name_min_width, opts);
        int block_value_width = tab_aligned_width(eo.enum_value_min_width, opts);
        if (eo.align && !eo.align_adaptive) {
            for (const auto& item : items) {
                block_name_width = std::max(block_name_width, (int)item.name.size() + 1);
                if (item.has_value)
                    block_value_width = std::max(block_value_width, (int)item.value.size());
            }
            if (opts.tab_align) {
                block_name_width =
                    snap_to_indent_grid(item_base_col + block_name_width, opts.indent_size) -
                    item_base_col;
                block_value_width =
                    snap_to_indent_grid(item_base_col + block_name_width + 2 + block_value_width,
                                        opts.indent_size) -
                    item_base_col - block_name_width - 2;
            }
        }

        out.push_back(leading + prefix + " {");
        for (size_t k = 0; k < items.size(); ++k) {
            const auto& item = items[k];
            std::string comma = (k + 1 < items.size()) ? "," : "";
            std::string rendered;
            if (!eo.align) {
                rendered = item.name + (item.has_value ? " = " + item.value : "");
            } else {
                int name_width = block_name_width;
                int value_width = block_value_width;
                if (eo.align_adaptive) {
                    name_width = std::max(tab_aligned_width(eo.enum_name_min_width, opts),
                                          (int)item.name.size() + 1);
                    if (item.has_value)
                        value_width = std::max(tab_aligned_width(eo.enum_value_min_width, opts),
                                               (int)item.value.size());
                    if (opts.tab_align) {
                        name_width =
                            snap_to_indent_grid(item_base_col + name_width, opts.indent_size) -
                            item_base_col;
                        value_width =
                            snap_to_indent_grid(item_base_col + name_width + 2 + value_width,
                                                opts.indent_size) -
                            item_base_col - name_width - 2;
                    }
                }
                rendered = pad_right(item.name, name_width);
                if (item.has_value)
                    rendered += "= " + pad_right(item.value, value_width);
            }
            std::string enum_line = leading + enum_indent + rendered + comma;
            if (comma.empty()) {
                while (!enum_line.empty() && enum_line.back() == ' ')
                    enum_line.pop_back();
            }
            out.push_back(enum_line);
        }
        out.push_back(leading + "} " + suffix + ";");
        i = end_i;
    }

    return out;
}

struct ModportItemParsed {
    std::string direction;
    std::string name;
};

struct ModportParsed {
    std::string name;
    std::vector<ModportItemParsed> items;
};

static bool parse_modport_statement(const std::string& flat, std::vector<ModportParsed>& modports) {
    auto toks = significant_tokens(flat);
    if (toks.empty() || toks[0].kind != TokenKind::ModPortKeyword)
        return false;
    if (!tok_is(toks.back(), ";", TokenKind::Semicolon))
        return false;
    size_t rest_start = (size_t)tok_pos(toks[0]) + tok_text(toks[0]).size();
    size_t semi = (size_t)tok_pos(toks.back());
    std::string rest = flat.substr(rest_start, semi - rest_start);
    auto entries = split_top_level(rest);
    auto is_modport_dir = [](TokenKind kind) {
        return kind == TokenKind::InputKeyword || kind == TokenKind::OutputKeyword ||
               kind == TokenKind::InOutKeyword || kind == TokenKind::RefKeyword ||
               kind == TokenKind::ImportKeyword || kind == TokenKind::ExportKeyword;
    };

    for (auto& entry_raw : entries) {
        std::string entry = trim_copy(entry_raw);
        if (entry.empty())
            continue;
        auto entry_toks = significant_tokens(entry);
        if (entry_toks.size() < 4 || !is_identifier(entry_toks[0]))
            return false;
        size_t open_i = 1;
        while (open_i < entry_toks.size() &&
               !tok_is(entry_toks[open_i], "(", TokenKind::OpenParenthesis))
            ++open_i;
        if (open_i >= entry_toks.size())
            return false;
        int depth = 1;
        size_t close_i = open_i;
        while (++close_i < entry_toks.size()) {
            if (tok_contains(entry_toks[close_i], '('))
                ++depth;
            else if (tok_contains(entry_toks[close_i], ')') && --depth == 0)
                break;
        }
        if (depth != 0 || close_i >= entry_toks.size())
            return false;

        ModportParsed mp;
        mp.name = tok_text(entry_toks[0]);
        size_t open = (size_t)tok_pos(entry_toks[open_i]);
        size_t close = (size_t)tok_pos(entry_toks[close_i]);
        auto items = split_top_level(entry.substr(open + 1, close - open - 1));
        for (auto& item_raw : items) {
            std::string item = trim_copy(item_raw);
            if (item.empty())
                continue;
            auto item_toks = significant_tokens(item);
            if (item_toks.empty())
                return false;
            std::string dir = tok_text(item_toks[0]);
            size_t remainder_start = (size_t)tok_pos(item_toks[0]) + tok_text(item_toks[0]).size();
            std::string remainder = trim_copy(item.substr(remainder_start));
            if (dir.empty() || remainder.empty() || !is_modport_dir(item_toks[0].kind))
                return false;
            mp.items.push_back({dir, remainder});
        }
        if (mp.items.empty())
            return false;
        modports.push_back(std::move(mp));
    }
    return !modports.empty();
}

static std::vector<std::string> format_modport_pass_legacy(std::vector<std::string> lines,
                                                      const FormatOptions& opts) {
    const auto& mo = opts.modport;
    const std::string item_indent(opts.indent_size, ' ');

    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        auto line_toks = significant_tokens(line);
        if (line_toks.empty() || line_toks[0].kind != TokenKind::ModPortKeyword) {
            out.push_back(lines[i]);
            continue;
        }
        size_t end_i = i;
        std::string flat;
        if (!collect_statement_lines(lines, i, end_i, flat)) {
            out.push_back(lines[i]);
            continue;
        }
        std::vector<ModportParsed> modports;
        if (!parse_modport_statement(flat, modports)) {
            for (size_t k = i; k <= end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        std::string leading = line.substr(0, (size_t)tok_pos(line_toks[0]));
        for (size_t mi = 0; mi < modports.size(); ++mi) {
            const auto& mp = modports[mi];
            int item_base_col = (int)leading.size() + opts.indent_size;
            int block_dir_width = tab_aligned_width(mo.direction_min_width, opts);
            int block_signal_width = tab_aligned_width(mo.signal_min_width, opts);
            if (mo.align) {
                for (const auto& item : mp.items) {
                    block_dir_width = std::max(block_dir_width, (int)item.direction.size() + 1);
                    if (!mo.align_adaptive)
                        block_signal_width = std::max(block_signal_width, (int)item.name.size());
                }
                if (opts.tab_align) {
                    block_dir_width =
                        snap_to_indent_grid(item_base_col + block_dir_width, opts.indent_size) -
                        item_base_col;
                    block_signal_width =
                        snap_to_indent_grid(item_base_col + block_dir_width + block_signal_width,
                                            opts.indent_size) -
                        item_base_col - block_dir_width;
                }
            }
            out.push_back(leading + "modport " + mp.name + " (");
            for (size_t pi = 0; pi < mp.items.size(); ++pi) {
                const auto& item = mp.items[pi];
                std::string comma = (pi + 1 < mp.items.size()) ? "," : "";
                std::string rendered;
                if (!mo.align) {
                    rendered = item.direction + " " + item.name;
                } else {
                    int dir_width = block_dir_width;
                    int signal_width = block_signal_width;
                    if (mo.align_adaptive) {
                        signal_width = std::max(tab_aligned_width(mo.signal_min_width, opts),
                                                (int)item.name.size());
                        if (opts.tab_align)
                            signal_width =
                                snap_to_indent_grid(item_base_col + dir_width + signal_width,
                                                    opts.indent_size) -
                                item_base_col - dir_width;
                    }
                    rendered =
                        pad_right(item.direction, dir_width) + pad_right(item.name, signal_width);
                }
                std::string port_line = leading + item_indent + rendered + comma;
                if (comma.empty()) {
                    while (!port_line.empty() && port_line.back() == ' ')
                        port_line.pop_back();
                }
                out.push_back(port_line);
            }
            out.push_back(leading + std::string(")") + (mi + 1 < modports.size() ? "," : ";"));
        }
        i = end_i;
    }

    return out;
}

static bool find_simple_call(const std::string& line, size_t& name_start, size_t& name_end,
                             size_t& open, size_t& close) {
    auto toks = collect_lexer_tokens(line);
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& tok = toks[i];
        if (tok_whitespace(tok) || tok_comment(tok))
            continue;
        bool callable = is_identifier(tok) || starts_with_chars(tok_text(tok), '`');
        if (!callable)
            continue;
        size_t prev = i;
        while (prev > 0 && tok_whitespace(toks[prev - 1]))
            --prev;
        size_t member_start = i;
        if (prev > 0 && tok_text(toks[prev - 1]) == ".") {
            size_t base = prev - 1;
            while (base > 0 && tok_whitespace(toks[base - 1]))
                --base;
            if (base == 0 || !is_identifier(toks[base - 1]))
                continue;
            member_start = base - 1;
        }
        size_t j = i + 1;
        while (j < toks.size() && tok_whitespace(toks[j]))
            ++j;
        if (j >= toks.size() || !tok_is(toks[j], "(", TokenKind::OpenParenthesis))
            continue;
        if (is_function_call_skip_token(tok))
            continue;
        int depth = 1;
        size_t k = j;
        while (++k < toks.size()) {
            if (tok_whitespace(toks[k]) || tok_comment(toks[k]) || tok_directive(toks[k]))
                continue;
            if (tok_contains(toks[k], '('))
                ++depth;
            else if (tok_contains(toks[k], ')') && --depth == 0)
                break;
        }
        if (depth != 0 || k >= toks.size())
            return false;
        name_start = (size_t)tok_pos(toks[member_start]);
        name_end = (size_t)tok_pos(tok) + tok_text(tok).size();
        open = (size_t)tok_pos(toks[j]);
        close = (size_t)tok_pos(toks[k]);
        return true;
    }
    return false;
}

static std::string render_call_single(const std::string& prefix, const std::string& name,
                                      const std::vector<std::string>& args,
                                      const std::string& suffix, const FunctionOptions& fo) {
    std::string r = prefix + name + (fo.space_before_paren ? " " : "") + "(";
    if (fo.space_inside_paren && !args.empty())
        r += " ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i)
            r += ", ";
        r += args[i];
    }
    if (fo.space_inside_paren && !args.empty())
        r += " ";
    r += ")" + suffix;
    return r;
}

static std::vector<std::string> format_function_calls_pass_legacy(std::vector<std::string> lines,
                                                                const FormatOptions& opts) {
    const auto& fo = opts.function;
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);

    std::vector<std::string> out;
    int pos = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        int line_start = pos;
        pos += (int)line.size() + 1; // +1 for the newline consumed by getline
        if (line.find('\n') != std::string::npos) {
            out.push_back(lines[li]);
            continue;
        }
        // Never reformat macro calls inside disabled regions or `define bodies.
        if (in_disabled(line_start, disabled) || line_has_pp_conditional(line)) {
            out.push_back(lines[li]);
            continue;
        }
        size_t ns = 0, ne = 0, op = 0, cl = 0;
        if (!find_simple_call(line, ns, ne, op, cl)) {
            out.push_back(lines[li]);
            continue;
        }
        // Skip if the matched call is inside a // comment
        {
            bool in_str = false;
            for (size_t ci = 0; ci < ns; ++ci) {
                if (line[ci] == '"' && (ci == 0 || line[ci - 1] != '\\'))
                    in_str = !in_str;
                if (!in_str && ci + 1 < line.size() && line[ci] == '/' && line[ci + 1] == '/') {
                    ns = std::string::npos;
                    break;
                }
            }
            if (ns == std::string::npos) {
                out.push_back(lines[li]);
                continue;
            }
        }
        if (ns < line.size() && line[ns] == '`') {
            out.push_back(lines[li]);
            continue;
        }
        std::string args_text = line.substr(op + 1, cl - op - 1);
        auto raw_args = split_top_level(args_text);
        std::vector<std::string> args;
        bool has_empty_arg = false;
        bool has_macro_arg = false;
        for (auto& a : raw_args) {
            auto t = trim_copy(a);
            if (!t.empty()) {
                if (t.find('`') != std::string::npos)
                    has_macro_arg = true;
                args.push_back(t);
            } else {
                has_empty_arg = true;
            }
        }
        if (has_empty_arg && !(raw_args.size() == 1 && args.empty())) {
            out.push_back(lines[li]);
            continue;
        }

        std::string prefix = line.substr(0, ns);
        std::string name = line.substr(ns, ne - ns);
        std::string suffix = line.substr(cl + 1);
        auto prefix_toks = significant_tokens(prefix);
        bool bare_id_prefix = prefix_toks.size() == 1 && is_identifier(prefix_toks[0]);
        bool declaration_prefix = false;
        for (const auto& tok : prefix_toks) {
            declaration_prefix = declaration_prefix || tok.kind == TokenKind::FunctionKeyword ||
                                 tok.kind == TokenKind::TaskKeyword ||
                                 tok.kind == TokenKind::ModuleKeyword ||
                                 tok.kind == TokenKind::ClassKeyword;
        }
        if (bare_id_prefix || declaration_prefix) {
            out.push_back(lines[li]);
            continue;
        }
        std::string single = render_call_single(prefix, name, args, suffix, fo);

        bool do_break = false;
        if (has_macro_arg && args.size() > 1) {
            do_break = true;
        } else if (fo.break_policy == "always") {
            do_break = !args.empty();
        } else if (fo.break_policy == "auto") {
            do_break = ((int)single.size() > fo.line_length) ||
                       (fo.arg_count >= 0 && (int)args.size() >= fo.arg_count);
        }
        if (!do_break || fo.break_policy == "never") {
            out.push_back(single);
            continue;
        }

        std::string open_text = prefix + name + (fo.space_before_paren ? " " : "") + "(";
        if (fo.layout == "hanging") {
            std::string hang(open_text.size(), ' ');
            std::string r = open_text;
            for (size_t i = 0; i < args.size(); ++i) {
                if (i)
                    r += "\n" + hang;
                r += args[i];
                if (i + 1 < args.size())
                    r += ",";
            }
            r += ")" + suffix;
            out.push_back(r);
        } else {
            std::string base_indent(prefix.size(), ' ');
            std::string arg_indent = base_indent + std::string(std::max(0, opts.indent_size), ' ');
            std::string r = open_text + "\n";
            for (size_t i = 0; i < args.size(); ++i) {
                r += arg_indent + args[i];
                if (i + 1 < args.size())
                    r += ",";
                r += "\n";
            }
            r += base_indent + ")" + suffix;
            out.push_back(r);
        }
    }
    return out;
}

static bool collect_statement_lines_pp_aware(const std::vector<std::string>& lines,
                                             const PPContext& pp, size_t start, size_t& end_i,
                                             std::string& flat_nonpp) {
    int paren = 0;
    int brace = 0;
    int bracket = 0;
    std::vector<std::string> parts;
    for (size_t i = start; i < lines.size(); ++i) {
        std::string trimmed = trim_copy(lines[i]);
        if (pp_line_has_conditional(pp, i)) {
            continue;
        }
        parts.push_back(trimmed);
        for (const auto& tok : collect_lexer_tokens(trimmed)) {
            if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
                continue;
            if (tok_contains(tok, '('))
                ++paren;
            else if (tok_contains(tok, ')') && paren > 0)
                --paren;
            else if (tok_contains(tok, '{'))
                ++brace;
            else if (tok_contains(tok, '}') && brace > 0)
                --brace;
            else if (tok_contains(tok, '['))
                ++bracket;
            else if (tok_contains(tok, ']') && bracket > 0)
                --bracket;
            else if (tok_is(tok, ";", TokenKind::Semicolon) && paren == 0 && brace == 0 &&
                     bracket == 0) {
                end_i = i;
                flat_nonpp.clear();
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k)
                        flat_nonpp += ' ';
                    flat_nonpp += parts[k];
                }
                return true;
            }
        }
    }
    return false;
}

static std::vector<std::string> format_pp_conditional_function_calls_pass_legacy(
    std::vector<std::string> lines, const FormatOptions& opts, const PPContext& pp) {
    const auto& fo = opts.function;
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    struct Piece {
        bool directive{false};
        std::string text;
        bool comma{false};
    };

    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (in_disabled(line_starts[i], disabled) || pp_line_has_conditional(pp, i)) {
            out.push_back(lines[i]);
            continue;
        }

        size_t end_i = i;
        auto push_collected_range = [&]() {
            for (size_t k = i; k <= end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
        };
        std::string flat;
        if (!collect_statement_lines_pp_aware(lines, pp, i, end_i, flat) || end_i == i) {
            out.push_back(lines[i]);
            continue;
        }
        if (!pp_range_has_conditional(pp, i, end_i)) {
            push_collected_range();
            continue;
        }

        size_t ns = 0, ne = 0, op = 0, cl = 0;
        if (!find_simple_call(flat, ns, ne, op, cl) || ns >= flat.size() || flat[ns] == '`') {
            push_collected_range();
            continue;
        }

        auto prefix_toks = significant_tokens(flat.substr(0, ns));
        bool declaration_prefix = false;
        for (const auto& tok : prefix_toks) {
            declaration_prefix = declaration_prefix || tok.kind == TokenKind::FunctionKeyword ||
                                 tok.kind == TokenKind::TaskKeyword ||
                                 tok.kind == TokenKind::ModuleKeyword ||
                                 tok.kind == TokenKind::ClassKeyword;
        }
        if (declaration_prefix) {
            push_collected_range();
            continue;
        }

        std::string leading;
        while (leading.size() < lines[i].size() &&
               (lines[i][leading.size()] == ' ' || lines[i][leading.size()] == '\t'))
            leading += lines[i][leading.size()];
        std::string prefix = flat.substr(0, op);
        std::string suffix = flat.substr(cl + 1);
        std::string open_text = leading + prefix + (fo.space_before_paren ? " " : "") + "(";
        std::string hang = fo.layout == "hanging"
                               ? std::string(open_text.size(), ' ')
                               : leading + std::string(std::max(0, opts.indent_size), ' ');

        std::vector<Piece> pieces;
        bool saw_call_open = false;
        for (size_t k = i; k <= end_i; ++k) {
            std::string trimmed = trim_copy(lines[k]);
            if (pp_line_has_conditional(pp, k)) {
                pieces.push_back({true, trimmed, false});
                continue;
            }
            std::string segment = trimmed;
            if (!saw_call_open) {
                if (segment.rfind(prefix + "(", 0) != 0) {
                    pieces.clear();
                    break;
                }
                segment = segment.substr(prefix.size() + 1);
                saw_call_open = true;
            }
            std::string tail = ")" + suffix;
            if (k == end_i && segment.size() >= tail.size() &&
                segment.compare(segment.size() - tail.size(), tail.size(), tail) == 0) {
                segment.erase(segment.size() - tail.size());
            }
            for (auto& arg : split_top_level_args_with_commas(segment)) {
                std::string t = trim_copy(arg.text);
                if (!t.empty())
                    pieces.push_back({false, t, arg.comma});
            }
        }
        if (pieces.empty()) {
            push_collected_range();
            continue;
        }

        bool has_arg = false;
        std::string rendered = open_text;
        for (const auto& piece : pieces) {
            if (piece.directive) {
                rendered += "\n" + piece.text;
                continue;
            }
            if (has_arg)
                rendered += "\n" + hang;
            rendered += piece.text;
            if (piece.comma)
                rendered += ",";
            has_arg = true;
        }
        rendered += ")" + suffix;
        out.push_back(rendered);
        i = end_i;
    }
    return out;
}

static std::vector<std::string> format_multiline_macro_arg_calls_pass_legacy(
    std::vector<std::string> lines, const FormatOptions& opts) {
    const auto& fo = opts.function;
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i])) {
            out.push_back(lines[i]);
            continue;
        }
        if (trim_copy(lines[i]).empty()) {
            out.push_back(lines[i]);
            continue;
        }

        size_t end_i = i;
        auto push_collected_range = [&]() {
            for (size_t k = i; k <= end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
        };
        std::string flat;
        if (!collect_statement_lines(lines, i, end_i, flat) || end_i == i) {
            out.push_back(lines[i]);
            continue;
        }
        if (range_has_pp_conditional(lines, i, end_i)) {
            push_collected_range();
            continue;
        }
        bool disabled_range = false;
        for (size_t k = i; k <= end_i; ++k)
            disabled_range = disabled_range || in_disabled(line_starts[k], disabled);
        if (disabled_range) {
            push_collected_range();
            continue;
        }

        size_t ns = 0, ne = 0, op = 0, cl = 0;
        if (!find_simple_call(flat, ns, ne, op, cl)) {
            push_collected_range();
            continue;
        }
        std::string args_text = flat.substr(op + 1, cl - op - 1);
        if (args_text.find('`') == std::string::npos) {
            push_collected_range();
            continue;
        }

        auto prefix_toks = significant_tokens(flat.substr(0, ns));
        bool declaration_prefix = false;
        for (const auto& tok : prefix_toks) {
            declaration_prefix = declaration_prefix || tok.kind == TokenKind::FunctionKeyword ||
                                 tok.kind == TokenKind::TaskKeyword ||
                                 tok.kind == TokenKind::ModuleKeyword ||
                                 tok.kind == TokenKind::ClassKeyword;
        }
        if (declaration_prefix || flat[ns] == '`') {
            push_collected_range();
            continue;
        }

        auto raw_args = split_top_level(args_text);
        std::vector<std::string> args;
        for (auto& arg : raw_args) {
            std::string trimmed = trim_copy(arg);
            if (!trimmed.empty())
                args.push_back(trimmed);
        }
        if (args.size() <= 1) {
            push_collected_range();
            continue;
        }

        std::string leading;
        while (leading.size() < lines[i].size() &&
               (lines[i][leading.size()] == ' ' || lines[i][leading.size()] == '\t'))
            leading += lines[i][leading.size()];
        std::string prefix = leading + flat.substr(0, ns);
        std::string name = flat.substr(ns, ne - ns);
        std::string suffix = flat.substr(cl + 1);
        std::string open_text = prefix + name + (fo.space_before_paren ? " " : "") + "(";
        std::string hang(open_text.size(), ' ');
        std::string r = open_text + args[0];
        for (size_t k = 1; k < args.size(); ++k)
            r += ",\n" + hang + args[k];
        r += ")" + suffix;
        out.push_back(r);
        i = end_i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Module port-list formatting pass
// ---------------------------------------------------------------------------

// Given a single line that starts a module declaration,
// extract prefix (including '('), ports_str (between outermost parens), suffix (')...;').
// Returns false if not a single-line module header.
static bool extract_single_line_module_header(const std::string& line, std::string& prefix,
                                              std::string& ports_str, std::string& suffix_str) {
    auto toks = significant_tokens(line);
    if (toks.size() < 4 ||
        (toks[0].kind != TokenKind::ModuleKeyword &&
         toks[0].kind != TokenKind::MacromoduleKeyword &&
         toks[0].kind != TokenKind::InterfaceKeyword))
        return false;

    size_t i = 1;
    if (i >= toks.size() || !is_identifier(toks[i]))
        return false;
    ++i;

    if (i < toks.size() && tok_is(toks[i], "#", TokenKind::Hash)) {
        ++i;
        if (i >= toks.size() || !tok_is(toks[i], "(", TokenKind::OpenParenthesis))
            return false;
        int depth = 1;
        while (i + 1 < toks.size() && depth > 0) {
            ++i;
            if (tok_is(toks[i], "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(toks[i], ")", TokenKind::CloseParenthesis))
                --depth;
        }
        if (depth != 0)
            return false;
        ++i;
    }

    while (i < toks.size() && toks[i].kind == TokenKind::ImportKeyword) {
        while (i < toks.size() && !tok_is(toks[i], ";", TokenKind::Semicolon))
            ++i;
        if (i >= toks.size())
            return false;
        ++i;
    }

    if (i < toks.size() && tok_is(toks[i], "#", TokenKind::Hash)) {
        ++i;
        if (i >= toks.size() || !tok_is(toks[i], "(", TokenKind::OpenParenthesis))
            return false;
        int depth = 1;
        while (i + 1 < toks.size() && depth > 0) {
            ++i;
            if (tok_is(toks[i], "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(toks[i], ")", TokenKind::CloseParenthesis))
                --depth;
        }
        if (depth != 0)
            return false;
        ++i;
    }

    if (i >= toks.size() || !tok_is(toks[i], "(", TokenKind::OpenParenthesis))
        return false;
    size_t open_paren = (size_t)tok_pos(toks[i]);

    int depth = 1;
    size_t close_index = i;
    while (close_index + 1 < toks.size() && depth > 0) {
        ++close_index;
        if (tok_is(toks[close_index], "(", TokenKind::OpenParenthesis))
            ++depth;
        else if (tok_is(toks[close_index], ")", TokenKind::CloseParenthesis))
            --depth;
    }
    if (depth != 0 || close_index >= toks.size())
        return false;
    size_t close_paren = (size_t)tok_pos(toks[close_index]);

    size_t semi_index = close_index + 1;
    if (semi_index >= toks.size() || !tok_is(toks[semi_index], ";", TokenKind::Semicolon))
        return false;
    if (semi_index + 1 != toks.size())
        return false;
    size_t semi_pos = (size_t)tok_pos(toks[semi_index]);

    prefix = line.substr(0, open_paren + 1);
    ports_str = line.substr(open_paren + 1, close_paren - open_paren - 1);
    size_t suffix_end = line.find('\n', semi_pos);
    suffix_str = line.substr(close_paren,
                             (suffix_end == std::string::npos ? line.size() : suffix_end) -
                                 close_paren);
    return true;
}

static bool is_module_header_start_tokenized(const std::string& line) {
    auto toks = significant_tokens(line);
    return !toks.empty() &&
           (toks[0].kind == TokenKind::ModuleKeyword ||
            toks[0].kind == TokenKind::MacromoduleKeyword ||
            toks[0].kind == TokenKind::InterfaceKeyword);
}

static bool could_start_module_header(const std::string& line) {
    return is_module_header_start_tokenized(line);
}

struct ModuleHeaderScan {
    int paren{0};
    int brace{0};
    int bracket{0};
    bool saw_paren{false};
    bool saw_import_before_port_list{false};
};

static bool scan_module_header_line(ModuleHeaderScan& scan, const std::string& line) {
    for (const auto& tok : collect_lexer_tokens(line)) {
        if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
            continue;

        if (!scan.saw_paren && tok.kind == TokenKind::ImportKeyword)
            scan.saw_import_before_port_list = true;

        if (tok_is(tok, "(", TokenKind::OpenParenthesis)) {
            scan.saw_paren = true;
            ++scan.paren;
        }
        else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && scan.paren > 0)
            --scan.paren;
        else if (tok_is(tok, "{", TokenKind::OpenBrace) ||
                 tok.kind == TokenKind::ApostropheOpenBrace)
            ++scan.brace;
        else if (tok_is(tok, "}", TokenKind::CloseBrace) && scan.brace > 0)
            --scan.brace;
        else if (tok_is(tok, "[", TokenKind::OpenBracket))
            ++scan.bracket;
        else if (tok_is(tok, "]", TokenKind::CloseBracket) && scan.bracket > 0)
            --scan.bracket;
        else if (tok_is(tok, ";", TokenKind::Semicolon) && scan.paren == 0 && scan.brace == 0 &&
                 scan.bracket == 0) {
            if (!scan.saw_paren && scan.saw_import_before_port_list)
                continue;
            return true;
        }
    }
    return false;
}

static std::vector<std::string> split_top_level_tokenized(const std::string& text) {
    std::vector<std::string> parts;
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    size_t start = 0;
    for (const auto& tok : significant_tokens(text)) {
        if (tok_is(tok, "(", TokenKind::OpenParenthesis))
            ++paren;
        else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren > 0)
            --paren;
        else if (tok_is(tok, "[", TokenKind::OpenBracket))
            ++bracket;
        else if (tok_is(tok, "]", TokenKind::CloseBracket) && bracket > 0)
            --bracket;
        else if (tok_is(tok, "{", TokenKind::OpenBrace))
            ++brace;
        else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace > 0)
            --brace;
        else if (tok_is(tok, ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
            parts.push_back(text.substr(start, (size_t)tok_pos(tok) - start));
            start = (size_t)tok_pos(tok) + tok_text(tok).size();
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

static std::string trim_all_copy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a]))
        ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> split_module_ports_tokenized(const std::string& text) {
    std::vector<std::string> parts;
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    size_t start = 0;
    for (const auto& tok : significant_tokens(text)) {
        if (tok_is(tok, "(", TokenKind::OpenParenthesis))
            ++paren;
        else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren > 0)
            --paren;
        else if (tok_is(tok, "[", TokenKind::OpenBracket))
            ++bracket;
        else if (tok_is(tok, "]", TokenKind::CloseBracket) && bracket > 0)
            --bracket;
        else if (tok_is(tok, "{", TokenKind::OpenBrace))
            ++brace;
        else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace > 0)
            --brace;
        else if (tok_is(tok, ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
            size_t end = (size_t)tok_pos(tok);
            size_t next = (size_t)tok_pos(tok) + tok_text(tok).size();
            size_t p = next;
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t'))
                ++p;
            if (p + 1 < text.size() && text[p] == '/' && text[p + 1] == '/') {
                size_t line_end = text.find('\n', p + 2);
                next = (line_end == std::string::npos) ? text.size() : line_end + 1;
                parts.push_back(text.substr(start, end - start) + " " +
                                text.substr(p, ((line_end == std::string::npos) ? text.size()
                                                                                 : line_end) -
                                                   p));
                start = next;
                continue;
            }
            parts.push_back(text.substr(start, end - start));
            start = next;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

static bool is_standalone_comment_line(const std::string& text) {
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (tok_whitespace(tok))
            continue;
        return tok_comment(tok);
    }
    return false;
}

static bool is_line_comment_token(const Tok& tok) {
    return tok_comment(tok) && starts_with_chars(tok_text(tok), '/', '/');
}

static size_t first_line_comment_start_tokenized(const std::string& text) {
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (is_line_comment_token(tok))
            return (size_t)tok_pos(tok);
    }
    return std::string::npos;
}

static bool token_starts_physical_line(const std::string& text, const Tok& tok) {
    size_t p = (size_t)tok_pos(tok);
    while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t'))
        --p;
    return p == 0 || text[p - 1] == '\n';
}

static size_t first_standalone_line_comment_tokenized(const std::string& text) {
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (is_line_comment_token(tok) && token_starts_physical_line(text, tok))
            return (size_t)tok_pos(tok);
    }
    return std::string::npos;
}

static size_t token_end(const Tok& tok) {
    return (size_t)tok_pos(tok) + tok_text(tok).size();
}

static size_t line_end_after_token(const std::string& text, const Tok& tok) {
    size_t end = token_end(tok);
    while (end < text.size() && text[end] != '\n')
        ++end;
    return end;
}

static void erase_extra_comma_before_line_comment(std::string& line) {
    auto toks = significant_tokens(line);
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        if (!tok_is(toks[i], ",", TokenKind::Comma) || !tok_is(toks[i + 1], ",", TokenKind::Comma))
            continue;
        size_t comment = first_line_comment_start_tokenized(line);
        if (comment != std::string::npos && (size_t)tok_pos(toks[i + 1]) < comment) {
            line.erase((size_t)tok_pos(toks[i + 1]), token_end(toks[i + 1]) - (size_t)tok_pos(toks[i + 1]));
            return;
        }
    }
}

static void remove_space_before_comma_token(std::string& line, size_t comma_pos) {
    if (comma_pos == 0 || comma_pos > line.size())
        return;
    size_t before = comma_pos;
    while (before > 0 && (line[before - 1] == ' ' || line[before - 1] == '\t'))
        --before;
    if (before < comma_pos)
        line.erase(before, comma_pos - before);
}

static void normalize_trailing_comma_spacing(std::string& line) {
    auto toks = significant_tokens(line);
    size_t comment = first_line_comment_start_tokenized(line);
    for (const auto& tok : toks) {
        if (!tok_is(tok, ",", TokenKind::Comma))
            continue;
        size_t comma = (size_t)tok_pos(tok);
        if (comment == std::string::npos || comma < comment)
            remove_space_before_comma_token(line, comma);
    }
}

static bool remove_last_code_comma(std::string& line) {
    size_t comment = first_line_comment_start_tokenized(line);
    auto toks = significant_tokens(line);
    for (size_t i = toks.size(); i > 0; --i) {
        const auto& tok = toks[i - 1];
        if (comment != std::string::npos && (size_t)tok_pos(tok) >= comment)
            continue;
        if (!tok_is(tok, ",", TokenKind::Comma))
            return false;
        line.erase((size_t)tok_pos(tok), token_end(tok) - (size_t)tok_pos(tok));
        return true;
    }
    return false;
}

static bool is_simple_identifier_tokenized(const std::string& text) {
    auto toks = significant_tokens(text);
    return toks.size() == 1 && is_identifier(toks[0]);
}

enum class PortListEntryKind { Port, Comment, Directive, Other };

static bool is_comma_eligible_portlist_entry(PortListEntryKind kind) {
    return kind != PortListEntryKind::Comment && kind != PortListEntryKind::Directive;
}

static PortListEntryKind classify_portlist_entry(const std::string& text) {
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (tok_whitespace(tok))
            continue;
        if (tok_comment(tok))
            return PortListEntryKind::Comment;
        if (is_line_directive(tok))
            return PortListEntryKind::Directive;
        if (is_port_direction_token(tok))
            return PortListEntryKind::Port;
        return PortListEntryKind::Other;
    }
    return PortListEntryKind::Other;
}

static std::string take_leading_portlist_block_comments_tokenized(std::string& ports_str) {
    size_t erase_end = 0;
    std::string comments;
    for (const auto& tok : collect_lexer_tokens(ports_str)) {
        if (tok_whitespace(tok))
            continue;
        if (!tok_comment(tok) || !starts_with_chars(tok_text(tok), '/', '*'))
            break;
        comments += ports_str.substr(erase_end, (size_t)tok_pos(tok) + tok_text(tok).size() - erase_end);
        erase_end = (size_t)tok_pos(tok) + tok_text(tok).size();
    }
    if (erase_end > 0)
        ports_str.erase(0, erase_end);
    return comments;
}

static std::string leading_horizontal_whitespace_tokenized(const std::string& line) {
    for (const auto& tok : collect_lexer_tokens(line)) {
        if (tok_whitespace(tok))
            continue;
        return line.substr(0, (size_t)tok_pos(tok));
    }
    return line;
}

static bool find_module_parameter_block(const std::string& prefix, size_t& hash_pos,
                                        size_t& param_open, size_t& param_close) {
    auto toks = significant_tokens(prefix);
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        if (!tok_is(toks[i], "#", TokenKind::Hash))
            continue;
        size_t j = i + 1;
        if (j >= toks.size() || !tok_is(toks[j], "(", TokenKind::OpenParenthesis))
            continue;

        int depth = 1;
        for (size_t k = j + 1; k < toks.size(); ++k) {
            if (tok_is(toks[k], "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(toks[k], ")", TokenKind::CloseParenthesis) && --depth == 0) {
                hash_pos = (size_t)tok_pos(toks[i]);
                param_open = (size_t)tok_pos(toks[j]);
                param_close = (size_t)tok_pos(toks[k]);
                return true;
            }
        }
        return false;
    }
    return false;
}

static std::string format_module_parameter_prefix(const std::string& prefix,
                                                  const std::string& leading_ws,
                                                  const FormatOptions& opts) {
    size_t hash_pos = 0, param_open = 0, param_close = 0;
    if (!find_module_parameter_block(prefix, hash_pos, param_open, param_close))
        return prefix;

    std::string before_hash = trim_right_copy(prefix.substr(0, hash_pos));
    bool has_import = false;
    for (const auto& tok : significant_tokens(before_hash)) {
        if (tok.kind == TokenKind::ImportKeyword) {
            has_import = true;
            break;
        }
    }
    bool split_import_parameter_header =
        before_hash.find('\n') != std::string::npos && has_import;
    if (!split_import_parameter_header)
        before_hash += " ";
    std::string params_str = prefix.substr(param_open + 1, param_close - param_open - 1);
    std::string after_params = trim_copy(prefix.substr(param_close + 1));
    auto after_toks = significant_tokens(after_params);
    if (after_toks.size() != 1 || !tok_is(after_toks[0], "(", TokenKind::OpenParenthesis))
        return prefix;

    auto params = split_top_level(params_str);
    std::vector<std::string> trimmed_params;
    for (auto& p : params) {
        std::string trimmed = trim_copy(p);
        if (!trimmed.empty())
            trimmed_params.push_back(std::move(trimmed));
    }
    if (trimmed_params.empty())
        return prefix;

    if (opts.module.parameter_layout == "hanging") {
        std::string open = before_hash + "#(";
        size_t last_nl = open.rfind('\n');
        size_t hang_width = (last_nl == std::string::npos) ? open.size() : open.size() - last_nl - 1;
        std::string hang(hang_width, ' ');
        // Re-indent a param's lines with hang, stripping previous indent.
        // indent_first: whether to prepend hang to the first content line.
        auto normalize_param = [&](const std::string& raw, bool indent_first) {
            // Count leading \n's — first one is always the line break from
            // the flattened input (not a real blank line).
            size_t first_non_nl = raw.find_first_not_of('\n');
            if (first_non_nl == std::string::npos)
                return raw;
            size_t blanks = first_non_nl > 0 ? first_non_nl - 1 : 0;
            std::string leading_nls(blanks, '\n');

            std::string content = raw.substr(first_non_nl);
            // Split into lines, strip per-line indentation, re-indent
            std::vector<std::string> lines;
            std::istringstream ss(content);
            std::string l;
            while (std::getline(ss, l))
                lines.push_back(l);
            std::string result;
            bool first_content = true;
            for (auto& line : lines) {
                std::string trimmed = trim_copy(line);
                if (trimmed.empty()) {
                    result += "\n";
                } else {
                    if (!result.empty())
                        result += "\n";
                    if (first_content && !indent_first)
                        result += trimmed;
                    else
                        result += hang + trimmed;
                    first_content = false;
                }
            }
            return leading_nls + result;
        };
        std::string out = open + normalize_param(trimmed_params[0], false);
        for (size_t i = 1; i < trimmed_params.size(); ++i)
            out += ",\n" + normalize_param(trimmed_params[i], true);
        out += ")(";
        return out;
    }

    std::string param_indent = leading_ws + std::string(opts.indent_size, ' ');
    std::string out = before_hash + "#(\n";
    auto normalize_block_param = [&](const std::string& raw) {
        std::vector<std::string> raw_lines;
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            raw_lines.push_back(trim_copy(line));
        }
        if (!raw.empty() && raw.back() == '\n')
            raw_lines.push_back("");

        if (!raw_lines.empty() && raw_lines.front().empty())
            raw_lines.erase(raw_lines.begin());
        while (!raw_lines.empty() && raw_lines.back().empty())
            raw_lines.pop_back();

        std::string result;
        for (size_t line_idx = 0; line_idx < raw_lines.size(); ++line_idx) {
            if (line_idx)
                result += "\n";
            if (!raw_lines[line_idx].empty())
                result += param_indent + raw_lines[line_idx];
        }
        return result;
    };
    for (size_t i = 0; i < trimmed_params.size(); ++i) {
        out += normalize_block_param(trimmed_params[i]);
        if (i + 1 < trimmed_params.size())
            out += ",";
        out += "\n";
    }
    out += leading_ws + ")(";
    return out;
}

static bool line_has_token_kind(const std::string& line, TokenKind kind) {
    for (const auto& tok : significant_tokens(line))
        if (tok.kind == kind)
            return true;
    return false;
}

static bool token_range_has_pp_conditional(const std::vector<Tok>& tokens, size_t first,
                                           size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]))
            continue;
        if (is_line_directive(tokens[i]) && is_pp_conditional(tok_directive_kind(tokens[i])))
            return true;
    }
    return false;
}

static bool token_range_disabled_or_passthrough(const std::vector<Tok>& tokens, size_t first,
                                                size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (!tok_whitespace(tokens[i]) && (tokens[i].fmt_disabled || tokens[i].fmt_passthrough))
            return true;
    }
    return false;
}

static std::vector<size_t> code_sig_indices(const std::vector<Tok>& tokens, size_t first,
                                            size_t end) {
    std::vector<size_t> out;
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (!tok_whitespace(tokens[i]) && !tok_comment(tokens[i]) && !tok_directive(tokens[i]))
            out.push_back(i);
    }
    return out;
}

static size_t next_code_sig(const std::vector<Tok>& tokens, size_t first, size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i)
        if (!tok_whitespace(tokens[i]) && !tok_comment(tokens[i]) && !tok_directive(tokens[i]))
            return i;
    return SIZE_MAX;
}

static size_t prev_code_sig(const std::vector<Tok>& tokens, size_t first, size_t before) {
    if (before > tokens.size())
        before = tokens.size();
    for (size_t i = before; i > first; --i) {
        size_t idx = i - 1;
        if (!tok_whitespace(tokens[idx]) && !tok_comment(tokens[idx]) && !tok_directive(tokens[idx]))
            return idx;
    }
    return SIZE_MAX;
}

static size_t matching_close_paren(const std::vector<Tok>& tokens, size_t open_idx,
                                   size_t end_limit) {
    int depth = 0;
    for (size_t i = open_idx; i < end_limit && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis))
            ++depth;
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis)) {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return SIZE_MAX;
}

static void format_class_extends_parameter_pass(std::vector<Tok>& tokens,
                                                const FormatOptions& opts) {
    auto starts = tok_line_starts(tokens);
    for (size_t li = 0; li < starts.size(); ++li) {
        size_t line_start = starts[li];
        size_t line_end = (li + 1 < starts.size()) ? starts[li + 1] : tokens.size();
        if (token_range_disabled_or_passthrough(tokens, line_start, line_end) ||
            token_range_has_pp_conditional(tokens, line_start, line_end))
            continue;

        auto sigs = code_sig_indices(tokens, line_start, line_end);
        if (sigs.empty() || tokens[sigs[0]].kind != TokenKind::ClassKeyword)
            continue;
        bool has_extends = false;
        for (size_t idx : sigs)
            has_extends = has_extends || tokens[idx].kind == TokenKind::ExtendsKeyword;
        if (!has_extends)
            continue;

        size_t hash_idx = SIZE_MAX, open_idx = SIZE_MAX;
        for (size_t si = 0; si + 1 < sigs.size(); ++si) {
            if (tok_is(tokens[sigs[si]], "#", TokenKind::Hash) &&
                tok_is(tokens[sigs[si + 1]], "(", TokenKind::OpenParenthesis)) {
                hash_idx = sigs[si];
                open_idx = sigs[si + 1];
                break;
            }
        }
        if (hash_idx == SIZE_MAX)
            continue;
        size_t close_idx = matching_close_paren(tokens, open_idx, line_end);
        if (close_idx == SIZE_MAX)
            continue;

        std::vector<size_t> param_starts;
        size_t first_param = next_code_sig(tokens, open_idx + 1, close_idx);
        if (first_param != SIZE_MAX)
            param_starts.push_back(first_param);
        int paren = 0, bracket = 0, brace = 0;
        for (size_t i = open_idx + 1; i < close_idx; ++i) {
            if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
                continue;
            if (tok_contains(tokens[i], '(')) ++paren;
            else if (tok_contains(tokens[i], ')') && paren > 0) --paren;
            else if (tok_contains(tokens[i], '[')) ++bracket;
            else if (tok_contains(tokens[i], ']') && bracket > 0) --bracket;
            else if (tok_contains(tokens[i], '{')) ++brace;
            else if (tok_contains(tokens[i], '}') && brace > 0) --brace;
            else if (tok_is(tokens[i], ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
                size_t n = next_code_sig(tokens, i + 1, close_idx);
                if (n != SIZE_MAX)
                    param_starts.push_back(n);
            }
        }
        if (param_starts.size() <= 1)
            continue;

        int base_indent = tokens[sigs[0]].fmt_indent;
        tokens[hash_idx].fmt_newline_before = false;
        tokens[hash_idx].fmt_spaces_before = 1;
        tokens[open_idx].fmt_newline_before = false;
        tokens[open_idx].fmt_spaces_before = 0;

        if (opts.module.parameter_layout == "block") {
            for (size_t pidx : param_starts) {
                tokens[pidx].fmt_newline_before = true;
                tokens[pidx].fmt_blank_lines = 0;
                tokens[pidx].fmt_indent = base_indent + 1;
                tokens[pidx].fmt_spaces_before = 0;
            }
            tokens[close_idx].fmt_newline_before = true;
            tokens[close_idx].fmt_blank_lines = 0;
            tokens[close_idx].fmt_indent = base_indent;
            tokens[close_idx].fmt_spaces_before = 0;
        } else {
            tokens[param_starts[0]].fmt_newline_before = false;
            tokens[param_starts[0]].fmt_spaces_before = 0;
            int hang_cols = 0;
            for (size_t i = line_start; i <= open_idx && i < tokens.size(); ++i) {
                if (tok_whitespace(tokens[i]))
                    continue;
                if (i == line_start)
                    hang_cols += tokens[i].fmt_indent * opts.indent_size + tokens[i].fmt_spaces_before;
                else
                    hang_cols += tokens[i].fmt_spaces_before;
                hang_cols += (int)tok_text(tokens[i]).size();
            }
            int hang_indent = opts.indent_size > 0 ? hang_cols / opts.indent_size : 0;
            int hang_spaces = std::max(0, hang_cols - hang_indent * std::max(0, opts.indent_size));
            for (size_t pi = 1; pi < param_starts.size(); ++pi) {
                tokens[param_starts[pi]].fmt_newline_before = true;
                tokens[param_starts[pi]].fmt_blank_lines = 0;
                tokens[param_starts[pi]].fmt_indent = hang_indent;
                tokens[param_starts[pi]].fmt_spaces_before = hang_spaces;
            }
            tokens[close_idx].fmt_newline_before = false;
            tokens[close_idx].fmt_spaces_before = 0;
        }
    }
}

struct HeaderPortEntry {
    size_t start{SIZE_MAX};
    size_t end{SIZE_MAX};
    size_t comma{SIZE_MAX};
    bool valid{false};
    bool ansi{false};
    size_t direction{SIZE_MAX};
    size_t dtype_start{SIZE_MAX};
    size_t dtype_end{SIZE_MAX};
    size_t qualifier{SIZE_MAX};
    size_t dim_start{SIZE_MAX};
    size_t dim_end{SIZE_MAX};
    size_t name{SIZE_MAX};
    size_t trail_start{SIZE_MAX};
    size_t trail_end{SIZE_MAX};
    std::string direction_text;
    std::string dtype_text;
    std::string qualifier_text;
    std::string dim_text;
    std::string name_text;
    std::string trail_text;
};

static std::string token_join_compact(const std::vector<Tok>& tokens, size_t first, size_t end) {
    std::string out;
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (!out.empty() && tok_text(tokens[i]) != "]" && tok_text(tokens[i]) != ")" &&
            tok_text(tokens[i]) != "," && tok_text(tokens[i]) != ";" && tok_text(tokens[i]) != ":" &&
            out.back() != '[' && out.back() != '(' && out.back() != ':' && tok_text(tokens[i]) != "::")
            out += ' ';
        if (tok_text(tokens[i]) == "::" && !out.empty() && out.back() == ' ')
            out.pop_back();
        out += tok_text(tokens[i]);
    }
    return out;
}

static HeaderPortEntry parse_header_port_entry(const std::vector<Tok>& tokens,
                                               const std::vector<size_t>& raw_sigs,
                                               const FormatOptions& opts) {
    HeaderPortEntry e{};
    std::vector<size_t> sigs;
    for (size_t idx : raw_sigs) {
        if (!tok_is(tokens[idx], ",", TokenKind::Comma))
            sigs.push_back(idx);
        else
            e.comma = idx;
    }
    if (sigs.empty())
        return e;
    e.start = sigs.front();
    e.end = sigs.back() + 1;
    e.valid = true;
    if (!is_port_direction_token(tokens[sigs[0]]))
        return e;
    e.ansi = true;
    e.direction = sigs[0];
    e.direction_text = tok_text(tokens[e.direction]);
    size_t si = 1;

    auto pure_id = [&](size_t idx) {
        const std::string& x = tok_text(tokens[idx]);
        if (x.empty() || (!std::isalpha((unsigned char)x[0]) && x[0] != '_' && x[0] != '$' && x[0] != '`'))
            return false;
        return is_identifier(tokens[idx]) || is_keyword(tokens[idx]);
    };

    if (si < sigs.size()) {
        bool builtin = is_var_builtin_type_token(tokens[sigs[si]]) || is_port_net_type_text(tok_text(tokens[sigs[si]]));
        bool usertype = pure_id(sigs[si]) && si + 1 < sigs.size() && !tok_is(tokens[sigs[si + 1]], ",", TokenKind::Comma);
        if ((builtin || usertype) && !is_sign_qualifier_token(tokens[sigs[si]]) &&
            !tok_is(tokens[sigs[si]], "[", TokenKind::OpenBracket)) {
            e.dtype_start = sigs[si];
            ++si;
            if (is_port_net_type_text(tok_text(tokens[e.dtype_start])) && si < sigs.size() &&
                is_port_data_type_text(tok_text(tokens[sigs[si]])))
                ++si;
            while (si + 1 < sigs.size() && tok_text(tokens[sigs[si]]) == "::")
                si += 2;
            e.dtype_end = (si < sigs.size()) ? sigs[si] : sigs.back() + 1;
            e.dtype_text = token_join_compact(tokens, e.dtype_start, e.dtype_end);
        }
    }
    if (si < sigs.size() && is_sign_qualifier_token(tokens[sigs[si]])) {
        e.qualifier = sigs[si++];
        e.qualifier_text = tok_text(tokens[e.qualifier]);
    }
    if (si < sigs.size() && tok_is(tokens[sigs[si]], "[", TokenKind::OpenBracket)) {
        e.dim_start = sigs[si];
        int depth = 0;
        while (si < sigs.size()) {
            if (tok_is(tokens[sigs[si]], "[", TokenKind::OpenBracket)) ++depth;
            else if (tok_is(tokens[sigs[si]], "]", TokenKind::CloseBracket)) --depth;
            ++si;
            if (depth <= 0)
                break;
        }
        e.dim_end = (si < sigs.size()) ? sigs[si] : sigs.back() + 1;
        e.dim_text = normalize_bracket_spacing(token_join_compact(tokens, e.dim_start, e.dim_end), opts);
    }
    if (si >= sigs.size()) {
        e.valid = false;
        return e;
    }
    e.name = sigs[si++];
    e.name_text = tok_text(tokens[e.name]);
    e.trail_start = si < sigs.size() ? sigs[si] : SIZE_MAX;
    e.trail_end = sigs.back() + 1;
    if (e.trail_start != SIZE_MAX)
        e.trail_text = normalize_bracket_spacing(token_join_compact(tokens, e.trail_start, e.trail_end), opts);
    return e;
}

static void align_header_ports_metadata(std::vector<Tok>& tokens,
                                        const std::vector<HeaderPortEntry>& entries,
                                        const FormatOptions& opts) {
    int md = 0, ms2 = 0, mdim = 0;
    bool has_qualified_type = false;
    int np = 0;
    for (const auto& e : entries) {
        if (!e.valid || !e.ansi)
            continue;
        ++np;
        md = std::max(md, (int)e.direction_text.size());
        std::string s2 = e.dtype_text + (e.qualifier_text.empty() ? "" : " " + e.qualifier_text);
        ms2 = std::max(ms2, (int)s2.size());
        has_qualified_type = has_qualified_type || s2.find("::") != std::string::npos;
        mdim = std::max(mdim, (int)e.dim_text.size());
    }
    if (!np)
        return;
    const auto& pd = opts.port_declaration;
    int s1 = tab_aligned_width(std::max(tab_aligned_width(pd.section1_min_width, opts), md + 1), opts);
    int s2 = ms2 > 0 ? tab_aligned_width(std::max(tab_aligned_width(pd.section2_min_width, opts), ms2 + (has_qualified_type ? 3 : 1)), opts) : 0;
    int s3 = mdim > 0 ? tab_aligned_width(std::max(tab_aligned_width(pd.section3_min_width, opts), mdim + 1), opts) : 0;
    int idw = 0, trailw = 0;
    for (const auto& e : entries) {
        if (!e.valid || !e.ansi)
            continue;
        idw = std::max(idw, (int)e.name_text.size());
        trailw = std::max(trailw, (int)e.trail_text.size());
    }
    int s4 = tab_aligned_width(std::max(tab_aligned_width(pd.section4_min_width, opts), idw + 1), opts);
    int s5 = tab_aligned_width(std::max(tab_aligned_width(pd.section5_min_width, opts), trailw), opts);

    for (const auto& e : entries) {
        if (!e.valid || !e.ansi)
            continue;
        int line_s1 = s1, line_s2 = s2, line_s3 = s3, line_s4 = s4, line_s5 = s5;
        std::string type_part = e.dtype_text + (e.qualifier_text.empty() ? "" : " " + e.qualifier_text);
        if (pd.align_adaptive) {
            int t1 = tab_aligned_width(pd.section1_min_width, opts);
            int t2 = t1 + (s2 > 0 ? tab_aligned_width(pd.section2_min_width, opts) : 0);
            int t3 = t2 + (s3 > 0 ? tab_aligned_width(pd.section3_min_width, opts) : 0);
            int e1 = tab_aligned_width(std::max(t1, (int)e.direction_text.size() + 1), opts);
            line_s1 = e1;
            int e2 = e1;
            if (s2 > 0) {
                int c2 = type_part.empty() ? 0 : (int)type_part.size() + 1;
                e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                line_s2 = e2 - e1;
            }
            if (s3 > 0) {
                int c3 = e.dim_text.empty() ? 0 : (int)e.dim_text.size() + 1;
                int e3 = tab_aligned_width(std::max(t3, e2 + c3), opts);
                line_s3 = e3 - e2;
            }
            line_s4 = tab_aligned_width(std::max(tab_aligned_width(pd.section4_min_width, opts), (int)e.name_text.size() + 1), opts);
            line_s5 = tab_aligned_width(std::max(tab_aligned_width(pd.section5_min_width, opts), (int)e.trail_text.size()), opts);
        }
        if (e.dtype_start != SIZE_MAX || e.qualifier != SIZE_MAX) {
            size_t type_first = e.dtype_start != SIZE_MAX ? e.dtype_start : e.qualifier;
            tokens[type_first].fmt_spaces_before = std::max(1, line_s1 - (int)e.direction_text.size());
            if (e.dim_start != SIZE_MAX) {
                tokens[e.dim_start].fmt_spaces_before = std::max(1, line_s2 - (int)type_part.size());
                tokens[e.name].fmt_spaces_before = std::max(1, line_s3 - (int)e.dim_text.size());
            } else {
                int pad = std::max(1, line_s2 - (int)type_part.size()) + (line_s3 > 0 ? line_s3 : 0);
                tokens[e.name].fmt_spaces_before = pad;
            }
        } else if (e.dim_start != SIZE_MAX) {
            tokens[e.dim_start].fmt_spaces_before = std::max(1, line_s1 - (int)e.direction_text.size()) +
                                                    (line_s2 > 0 ? line_s2 : 0);
            tokens[e.name].fmt_spaces_before = std::max(1, line_s3 - (int)e.dim_text.size());
        } else {
            int pad = std::max(1, line_s1 - (int)e.direction_text.size()) +
                      (line_s2 > 0 ? line_s2 : 0) + (line_s3 > 0 ? line_s3 : 0);
            tokens[e.name].fmt_spaces_before = pad;
        }
        if (e.trail_start != SIZE_MAX)
            tokens[e.trail_start].fmt_spaces_before = std::max(0, line_s4 - (int)e.name_text.size());
        if (e.comma != SIZE_MAX) {
            tokens[e.comma].fmt_newline_before = false;
            tokens[e.comma].fmt_spaces_before = e.trail_text.empty() ? 0 : std::max(0, line_s5 - (int)e.trail_text.size());
        }
    }
}

static void format_portlist_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].fmt_disabled || tokens[i].fmt_passthrough)
            continue;
        bool header_kw = tokens[i].kind == TokenKind::ModuleKeyword ||
                         tokens[i].kind == TokenKind::MacromoduleKeyword ||
                         tokens[i].kind == TokenKind::InterfaceKeyword ||
                         tokens[i].kind == TokenKind::ProgramKeyword;
        if (!header_kw)
            continue;

        size_t port_open = SIZE_MAX;
        int paren = 0, bracket = 0, brace = 0;
        for (size_t j = i + 1; j < tokens.size(); ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) {
                size_t prev = prev_code_sig(tokens, i, j);
                if (paren == 0 && !(prev != SIZE_MAX && tok_is(tokens[prev], "#", TokenKind::Hash))) {
                    port_open = j;
                    break;
                }
                ++paren;
            } else if (tok_is(tokens[j], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
            else if (tok_is(tokens[j], "[", TokenKind::OpenBracket)) ++bracket;
            else if (tok_is(tokens[j], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
            else if (tok_is(tokens[j], "{", TokenKind::OpenBrace)) ++brace;
            else if (tok_is(tokens[j], "}", TokenKind::CloseBrace) && brace > 0) --brace;
            else if (tok_is(tokens[j], ";", TokenKind::Semicolon) && paren == 0 && bracket == 0 && brace == 0) {
                bool has_import = false;
                for (size_t k = i; k < j; ++k)
                    if (tokens[k].kind == TokenKind::ImportKeyword)
                        has_import = true;
                size_t next = next_code_sig(tokens, j + 1, tokens.size());
                if (!(has_import && next != SIZE_MAX &&
                      (tok_is(tokens[next], "(", TokenKind::OpenParenthesis) ||
                       tok_is(tokens[next], "#", TokenKind::Hash))))
                    break;
            } else if ((tokens[j].kind == TokenKind::EndModuleKeyword ||
                      tokens[j].kind == TokenKind::EndInterfaceKeyword ||
                      tokens[j].kind == TokenKind::EndProgramKeyword) && paren == 0)
                break;
        }
        if (port_open == SIZE_MAX)
            continue;
        size_t port_close = matching_close_paren(tokens, port_open, tokens.size());
        if (port_close == SIZE_MAX)
            continue;
        size_t semi = next_code_sig(tokens, port_close + 1, tokens.size());
        if (semi == SIZE_MAX || !tok_is(tokens[semi], ";", TokenKind::Semicolon))
            continue;
        if (token_range_has_pp_conditional(tokens, i, semi + 1) ||
            token_range_disabled_or_passthrough(tokens, i, semi + 1))
            continue;

        std::vector<size_t> sigs = code_sig_indices(tokens, i, semi + 1);
        int base_indent = tokens[i].fmt_indent;

        // Format module/interface parameter block immediately before the port list.
        size_t hash_idx = SIZE_MAX, param_open = SIZE_MAX, param_close = SIZE_MAX;
        for (size_t si = 0; si + 1 < sigs.size(); ++si) {
            if (sigs[si] >= port_open)
                break;
            if (tok_is(tokens[sigs[si]], "#", TokenKind::Hash) &&
                tok_is(tokens[sigs[si + 1]], "(", TokenKind::OpenParenthesis)) {
                hash_idx = sigs[si];
                param_open = sigs[si + 1];
                param_close = matching_close_paren(tokens, param_open, port_open);
                break;
            }
        }
        if (hash_idx != SIZE_MAX && param_close != SIZE_MAX) {
            std::vector<size_t> params;
            size_t first_param = next_code_sig(tokens, param_open + 1, param_close);
            if (first_param != SIZE_MAX)
                params.push_back(first_param);
            int p_paren = 0, p_bracket = 0, p_brace = 0;
            for (size_t j = param_open + 1; j < param_close; ++j) {
                if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                    continue;
                if (tok_contains(tokens[j], '(')) ++p_paren;
                else if (tok_contains(tokens[j], ')') && p_paren > 0) --p_paren;
                else if (tok_contains(tokens[j], '[')) ++p_bracket;
                else if (tok_contains(tokens[j], ']') && p_bracket > 0) --p_bracket;
                else if (tok_contains(tokens[j], '{')) ++p_brace;
                else if (tok_contains(tokens[j], '}') && p_brace > 0) --p_brace;
                else if (tok_is(tokens[j], ",", TokenKind::Comma) && p_paren == 0 && p_bracket == 0 && p_brace == 0) {
                    size_t n = next_code_sig(tokens, j + 1, param_close);
                    if (n != SIZE_MAX)
                        params.push_back(n);
                }
            }
            tokens[hash_idx].fmt_spaces_before = 1;
            tokens[param_open].fmt_spaces_before = 0;
            if (opts.module.parameter_layout == "block") {
                for (size_t pidx : params) {
                    tokens[pidx].fmt_newline_before = true;
                    tokens[pidx].fmt_blank_lines = 0;
                    tokens[pidx].fmt_indent = base_indent + 1;
                    tokens[pidx].fmt_spaces_before = 0;
                }
                tokens[param_close].fmt_newline_before = true;
                tokens[param_close].fmt_blank_lines = 0;
                tokens[param_close].fmt_indent = base_indent;
                tokens[param_close].fmt_spaces_before = 0;
            } else if (params.size() > 1) {
                tokens[params[0]].fmt_newline_before = false;
                tokens[params[0]].fmt_spaces_before = 0;
                int hang_cols = 0;
                for (size_t j = i; j <= param_open && j < tokens.size(); ++j) {
                    if (tok_whitespace(tokens[j]))
                        continue;
                    if (j == i)
                        hang_cols += tokens[j].fmt_indent * opts.indent_size + tokens[j].fmt_spaces_before;
                    else
                        hang_cols += tokens[j].fmt_spaces_before;
                    hang_cols += (int)tok_text(tokens[j]).size();
                }
                int hang_indent = opts.indent_size > 0 ? hang_cols / opts.indent_size : 0;
                int hang_spaces = std::max(0, hang_cols - hang_indent * std::max(0, opts.indent_size));
                for (size_t pi = 1; pi < params.size(); ++pi) {
                    tokens[params[pi]].fmt_newline_before = true;
                    tokens[params[pi]].fmt_blank_lines = 0;
                    tokens[params[pi]].fmt_indent = hang_indent;
                    tokens[params[pi]].fmt_spaces_before = hang_spaces;
                }
            }
        }

        std::vector<std::vector<size_t>> raw_entries;
        std::vector<size_t> current;
        paren = bracket = brace = 0;
        for (size_t j = port_open + 1; j < port_close; ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) ++paren;
            else if (tok_is(tokens[j], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
            else if (tok_is(tokens[j], "[", TokenKind::OpenBracket)) ++bracket;
            else if (tok_is(tokens[j], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
            else if (tok_is(tokens[j], "{", TokenKind::OpenBrace)) ++brace;
            else if (tok_is(tokens[j], "}", TokenKind::CloseBrace) && brace > 0) --brace;
            current.push_back(j);
            if (tok_is(tokens[j], ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
                raw_entries.push_back(current);
                current.clear();
            }
        }
        if (!current.empty())
            raw_entries.push_back(current);
        if (raw_entries.empty())
            continue;

        std::vector<HeaderPortEntry> entries;
        bool ansi = false;
        for (const auto& raw : raw_entries) {
            HeaderPortEntry e = parse_header_port_entry(tokens, raw, opts);
            if (e.valid) {
                ansi = ansi || e.ansi;
                entries.push_back(e);
            }
        }
        if (entries.empty())
            continue;

        tokens[port_open].fmt_spaces_before = 0;
        for (size_t ei = 0; ei < entries.size(); ++ei) {
            auto& e = entries[ei];
            if (!e.valid)
                continue;
            bool put_newline = true;
            if (!ansi && opts.module.non_ansi_port_per_line_enabled &&
                opts.module.non_ansi_port_per_line > 1) {
                put_newline = (ei % (size_t)opts.module.non_ansi_port_per_line) == 0;
            }
            tokens[e.start].fmt_newline_before = put_newline;
            tokens[e.start].fmt_blank_lines = 0;
            tokens[e.start].fmt_indent = base_indent + 1;
            tokens[e.start].fmt_spaces_before = put_newline ? 0 : 1;
        }
        for (size_t j = port_open + 1; j < port_close; ++j) {
            if (!tok_comment(tokens[j]))
                continue;
            if (tokens[j].fmt_newline_before) {
                tokens[j].fmt_newline_before = true;
                tokens[j].fmt_blank_lines = 0;
                tokens[j].fmt_indent = base_indent + 1;
                tokens[j].fmt_spaces_before = 0;
            } else {
                tokens[j].fmt_spaces_before = 1;
            }
        }
        tokens[port_close].fmt_newline_before = true;
        tokens[port_close].fmt_blank_lines = 0;
        tokens[port_close].fmt_indent = base_indent;
        tokens[port_close].fmt_spaces_before = 0;
        tokens[semi].fmt_newline_before = false;
        tokens[semi].fmt_spaces_before = 0;

        if (ansi && opts.port_declaration.align)
            align_header_ports_metadata(tokens, entries, opts);

        i = semi;
    }
}

// ---------------------------------------------------------------------------
// Formatter pipeline phases
// ---------------------------------------------------------------------------
//
// The formatter is organized as:
//   Phase 1: token-level spacing, line breaks, and indentation   (token loop)
//   Phase 2: structural layout / reflow of multiline constructs  (post-token)
//   Phase 3: line-group alignment                                (v2 bridge)
//
// Post-token passes operate on plain line vectors.  Passes that need syntactic
// roles classify the relevant lines locally from lexer tokens.

// ---------------------------------------------------------------------------
// Function/task declaration formatting pass
// ---------------------------------------------------------------------------
static std::vector<std::string> format_function_declaration_pass_legacy(
    std::vector<std::string> lines, const FormatOptions& opts) {
    const auto& fd = opts.function_declaration;
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);

    std::vector<std::string> out;
    int pos = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        int line_start = pos;
        pos += (int)line.size() + 1;
        if (in_disabled(line_start, disabled) || line_has_pp_conditional(line)) {
            out.push_back(line);
            continue;
        }
        auto toks = significant_tokens(line);
        if (toks.empty() || (toks[0].kind != TokenKind::FunctionKeyword &&
                             toks[0].kind != TokenKind::TaskKeyword)) {
            out.push_back(line);
            continue;
        }
        size_t indent_end = (size_t)tok_pos(toks[0]);
        size_t open_i = 1;
        while (open_i < toks.size() && !tok_is(toks[open_i], "(", TokenKind::OpenParenthesis))
            ++open_i;
        if (open_i >= toks.size()) {
            out.push_back(line);
            continue;
        }
        int depth = 1;
        size_t close_i = open_i;
        while (++close_i < toks.size()) {
            if (tok_contains(toks[close_i], '('))
                ++depth;
            else if (tok_contains(toks[close_i], ')') && --depth == 0)
                break;
        }
        if (depth != 0 || close_i >= toks.size()) {
            out.push_back(line);
            continue;
        }
        // Check if worth breaking (short lines stay single-line)
        if ((int)line.size() <= fd.line_length) {
            out.push_back(line);
            continue;
        }
        // Split ports
        size_t open = (size_t)tok_pos(toks[open_i]);
        size_t close = (size_t)tok_pos(toks[close_i]);
        std::string args_text = line.substr(open + 1, close - open - 1);
        auto raw_args = split_top_level(args_text);
        std::vector<std::string> args;
        for (auto& a : raw_args) {
            auto t = trim_copy(a);
            if (!t.empty())
                args.push_back(t);
        }
        if (args.empty()) {
            out.push_back(line);
            continue;
        }
        std::string prefix = line.substr(0, open);
        std::string suffix = line.substr(close + 1);
        std::string base_indent = line.substr(0, indent_end);
        std::string arg_indent = base_indent + std::string(std::max(0, opts.indent_size), ' ');

        if (fd.layout == "hanging") {
            std::string open_text = prefix + "(";
            std::string hang(open_text.size(), ' ');
            std::string r = open_text;
            for (size_t i = 0; i < args.size(); ++i) {
                if (i)
                    r += "\n" + hang;
                r += args[i];
                if (i + 1 < args.size())
                    r += ",";
            }
            r += ")" + suffix;
            out.push_back(r);
        } else {
            // block layout
            std::string r = prefix + "(\n";
            for (size_t i = 0; i < args.size(); ++i) {
                r += arg_indent + args[i];
                if (i + 1 < args.size())
                    r += ",";
                r += "\n";
            }
            r += base_indent + ")" + suffix;
            out.push_back(r);
        }
    }
    return out;
}


static std::vector<std::string> split_semicolon_statements_pass(std::vector<std::string> lines) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    std::vector<std::string> out;
    for (size_t li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        std::string trimmed_line = trim_right_copy(line);
        if (in_disabled(line_starts[li], disabled) || line_has_pp_conditional(line) ||
            (!trimmed_line.empty() && trimmed_line.back() == '\\')) {
            out.push_back(line);
            continue;
        }
        std::vector<size_t> split_points;
        int paren = 0;
        int bracket = 0;
        int brace = 0;
        for (const auto& tok : collect_lexer_tokens(line)) {
            if (tok_comment(tok))
                break;
            if (tok_whitespace(tok) || tok_directive(tok))
                continue;
            if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                ++paren;
            else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren > 0)
                --paren;
            else if (tok_is(tok, "[", TokenKind::OpenBracket))
                ++bracket;
            else if (tok_is(tok, "]", TokenKind::CloseBracket) && bracket > 0)
                --bracket;
            else if (tok_is(tok, "{", TokenKind::OpenBrace))
                ++brace;
            else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace > 0)
                --brace;
            if (paren == 0 && bracket == 0 && brace == 0 && tok_is(tok, ";", TokenKind::Semicolon)) {
                size_t rest = (size_t)tok_pos(tok) + tok_text(tok).size();
                while (rest < line.size() && (line[rest] == ' ' || line[rest] == '\t'))
                    ++rest;
                if (rest < line.size() && line.compare(rest, 2, "//") != 0 &&
                    line.compare(rest, 2, "/*") != 0)
                    split_points.push_back((size_t)tok_pos(tok) + tok_text(tok).size());
            }
        }
        if (split_points.empty()) {
            out.push_back(line);
            continue;
        }
        size_t start = 0;
        std::string indent;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            indent += line[start++];
        start = 0;
        for (size_t point : split_points) {
            out.push_back(trim_right_copy(line.substr(start, point - start)));
            start = point;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
                ++start;
        }
        if (start < line.size())
            out.push_back(indent + trim_left_copy(line.substr(start)));
    }
    return out;
}

static std::vector<std::string> format_covergroup_pass_legacy(std::vector<std::string> lines,
                                                         const FormatOptions& opts) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    std::vector<std::string> out;
    int covergroup_depth = 0;
    std::string covergroup_indent;
    int covergroup_brace_depth = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        auto toks = significant_tokens(line);

        bool disabled_line = in_disabled(line_starts[li], disabled) || line_has_pp_conditional(line);

        if (!disabled_line && !toks.empty() && toks[0].kind == TokenKind::CoverGroupKeyword) {
            size_t end_i = li;
            std::string flat = trim_copy(line);
            bool found_semicolon = false;
            while (end_i < lines.size()) {
                for (const auto& tok : significant_tokens(lines[end_i])) {
                    if (tok_is(tok, ";", TokenKind::Semicolon)) {
                        found_semicolon = true;
                        break;
                    }
                }
                if (found_semicolon)
                    break;
                if (end_i + 1 >= lines.size() || line_has_pp_conditional(lines[end_i + 1]) ||
                    in_disabled(line_starts[end_i + 1], disabled))
                    break;
                ++end_i;
                flat += " " + trim_copy(lines[end_i]);
            }
            if (found_semicolon) {
                std::string leading = leading_horizontal_whitespace_tokenized(line);
                size_t semi = flat.rfind(';');
                std::string decl = semi == std::string::npos ? flat : trim_copy(flat.substr(0, semi));
                std::string suffix = semi == std::string::npos ? "" : trim_copy(flat.substr(semi + 1));

                size_t sample_pos = decl.find("with function sample");
                if (sample_pos != std::string::npos) {
                    size_t open = decl.find('(', sample_pos);
                    size_t close = decl.rfind(')');
                    if (open != std::string::npos && close != std::string::npos && close > open) {
                        std::string before = trim_right_copy(decl.substr(0, open));
                        std::string args_text = decl.substr(open + 1, close - open - 1);
                        auto raw_args = split_top_level(args_text);
                        std::vector<std::string> args;
                        for (auto& arg : raw_args) {
                            std::string trimmed = trim_copy(arg);
                            if (!trimmed.empty())
                                args.push_back(trimmed);
                        }
                        std::string rendered = before + "(";
                        if (!args.empty() &&
                            (opts.function_declaration.layout == "block" ||
                             (int)(before.size() + args_text.size() + 3) >
                                 opts.function_declaration.line_length)) {
                            std::string arg_indent =
                                leading + std::string(opts.indent_size, ' ');
                            rendered += "\n";
                            for (size_t ai = 0; ai < args.size(); ++ai) {
                                rendered += arg_indent + args[ai];
                                if (ai + 1 < args.size())
                                    rendered += ",";
                                rendered += "\n";
                            }
                            rendered += leading + ")";
                        } else {
                            for (size_t ai = 0; ai < args.size(); ++ai) {
                                if (ai)
                                    rendered += ", ";
                                rendered += args[ai];
                            }
                            rendered += ")";
                        }
                        decl = rendered + trim_copy(decl.substr(close + 1));
                    }
                }

                out.push_back(leading + decl + ";" + suffix);
                ++covergroup_depth;
                covergroup_indent = leading;
                covergroup_brace_depth = 0;
                li = end_i;
                continue;
            }
        }

        bool in_covergroup = covergroup_depth > 0;
        bool has_cover_item = false;
        for (const auto& tok : toks) {
            if (is_covergroup_item_keyword(tok.kind))
                has_cover_item = true;
        }

        if (!disabled_line && in_covergroup && has_cover_item) {
            size_t open_pos = std::string::npos;
            bool split_body = false;
            for (size_t ti = 0; ti < toks.size(); ++ti) {
                if (tok_is(toks[ti], "{", TokenKind::OpenBrace)) {
                    open_pos = (size_t)tok_pos(toks[ti]);
                    break;
                }
                if (toks[ti].kind == TokenKind::CoverPointKeyword ||
                    toks[ti].kind == TokenKind::CrossKeyword)
                    split_body = true;
            }

            if (open_pos != std::string::npos) {
                size_t body_start = open_pos + 1;
                std::string body = trim_copy(line.substr(body_start));
                size_t close_rel = body.find('}');
                std::string close_suffix;
                if (close_rel != std::string::npos) {
                    close_suffix = trim_copy(body.substr(close_rel + 1));
                    body = trim_copy(body.substr(0, close_rel));
                }
                if (body.find('`') != std::string::npos)
                    split_body = true;

                if (split_body && !body.empty()) {
                    std::string leading =
                        covergroup_indent + std::string(std::max(0, opts.indent_size), ' ');
                    std::string item_indent = leading + std::string(opts.indent_size, ' ');
                    out.push_back(leading + trim_left_copy(trim_right_copy(line.substr(0, body_start))));
                    out.push_back(item_indent + body);
                    if (close_rel != std::string::npos)
                        out.push_back(leading + "}" + close_suffix);
                    else
                        ++covergroup_brace_depth;

                    for (const auto& tok : toks) {
                        if (tok.kind == TokenKind::CoverGroupKeyword)
                            ++covergroup_depth;
                        else if (tok.kind == TokenKind::EndGroupKeyword)
                            covergroup_depth = std::max(0, covergroup_depth - 1);
                    }
                    continue;
                }
            }
        }

        if (!disabled_line && in_covergroup) {
            if (trim_copy(line).empty()) {
                out.push_back("");
                continue;
            }
            bool starts_with_close_brace = false;
            for (const auto& tok : toks) {
                if (tok_is(tok, "}", TokenKind::CloseBrace)) {
                    starts_with_close_brace = true;
                    break;
                }
                if (!tok_whitespace(tok) && !tok_comment(tok))
                    break;
            }
            bool is_endgroup_line = !toks.empty() && toks[0].kind == TokenKind::EndGroupKeyword;
            int line_depth = covergroup_brace_depth;
            if (starts_with_close_brace)
                line_depth = std::max(0, line_depth - 1);
            int indent_levels = is_endgroup_line ? 0 : 1 + line_depth;
            out.push_back(covergroup_indent +
                          std::string(indent_levels * std::max(0, opts.indent_size), ' ') +
                          trim_left_copy(line));
            for (const auto& tok : toks) {
                if (tok_is(tok, "{", TokenKind::OpenBrace))
                    ++covergroup_brace_depth;
                else if (tok_is(tok, "}", TokenKind::CloseBrace) && covergroup_brace_depth > 0)
                    --covergroup_brace_depth;
            }
        } else {
            out.push_back(line);
        }
        for (const auto& tok : toks) {
            if (tok.kind == TokenKind::CoverGroupKeyword)
                ++covergroup_depth;
            else if (tok.kind == TokenKind::EndGroupKeyword) {
                covergroup_depth = std::max(0, covergroup_depth - 1);
                if (covergroup_depth == 0)
                    covergroup_brace_depth = 0;
            }
        }
    }
    std::vector<std::string> normalized;
    bool pending_covergroup_decl = false;
    bool in_covergroup_body = false;
    std::string body_base_indent;
    int body_brace_depth = 0;
    for (const auto& out_line : out) {
        std::string trimmed = trim_copy(out_line);
        if (trimmed.rfind("covergroup ", 0) == 0 || trimmed == "covergroup") {
            pending_covergroup_decl = true;
            in_covergroup_body = false;
            body_base_indent = leading_horizontal_whitespace_tokenized(out_line);
            body_brace_depth = 0;
            normalized.push_back(out_line);
            if (trimmed.find(';') != std::string::npos) {
                pending_covergroup_decl = false;
                in_covergroup_body = true;
            }
            continue;
        }
        if (pending_covergroup_decl) {
            normalized.push_back(out_line);
            if (trimmed.find(';') != std::string::npos) {
                pending_covergroup_decl = false;
                in_covergroup_body = true;
            }
            continue;
        }
        if (!in_covergroup_body) {
            normalized.push_back(out_line);
            continue;
        }
        if (trimmed.empty()) {
            normalized.push_back("");
            continue;
        }
        bool is_endgroup = trimmed.rfind("endgroup", 0) == 0;
        int line_depth = body_brace_depth;
        if (!trimmed.empty() && trimmed[0] == '}')
            line_depth = std::max(0, line_depth - 1);
        int indent_levels = is_endgroup ? 0 : 1 + line_depth;
        normalized.push_back(body_base_indent +
                             std::string(indent_levels * std::max(0, opts.indent_size), ' ') +
                             trimmed);

        bool in_string = false;
        bool escaped = false;
        for (char c : trimmed) {
            if (in_string) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    in_string = false;
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == '{')
                ++body_brace_depth;
            else if (c == '}' && body_brace_depth > 0)
                --body_brace_depth;
        }
        if (is_endgroup) {
            in_covergroup_body = false;
            body_brace_depth = 0;
        }
    }
    return normalized;
}

static std::vector<std::string> format_constraint_dist_pass_legacy(std::vector<std::string> lines,
                                                              const FormatOptions& opts) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    std::vector<std::string> out;
    size_t i = 0;
    while (i < lines.size()) {
        if (in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i]) ||
            !line_has_token_kind(lines[i], TokenKind::DistKeyword)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        size_t end = i;
        std::string flat = trim_copy(lines[i]);
        int brace_depth = 0;
        bool saw_open = false;
        for (;;) {
            for (const auto& tok : collect_lexer_tokens(lines[end])) {
                if (tok_whitespace(tok) || tok_comment(tok) || tok_directive(tok))
                    continue;
                if (tok_is(tok, "{", TokenKind::OpenBrace)) {
                    ++brace_depth;
                    saw_open = true;
                } else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace_depth > 0) {
                    --brace_depth;
                }
            }
            if (saw_open && brace_depth == 0)
                break;
            if (end + 1 >= lines.size() || line_has_pp_conditional(lines[end + 1]) ||
                in_disabled(line_starts[end + 1], disabled))
                break;
            ++end;
            flat += " " + trim_copy(lines[end]);
        }

        auto toks = significant_tokens(flat);
        size_t dist_i = toks.size(), open_i = toks.size(), close_i = toks.size();
        for (size_t ti = 0; ti < toks.size(); ++ti) {
            if (toks[ti].kind == TokenKind::DistKeyword) {
                dist_i = ti;
                break;
            }
        }
        if (dist_i == toks.size()) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        for (size_t ti = dist_i + 1; ti < toks.size(); ++ti) {
            if (tok_is(toks[ti], "{", TokenKind::OpenBrace)) {
                open_i = ti;
                break;
            }
        }
        if (open_i == toks.size()) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        int depth = 1;
        for (size_t ti = open_i + 1; ti < toks.size(); ++ti) {
            if (tok_is(toks[ti], "{", TokenKind::OpenBrace))
                ++depth;
            else if (tok_is(toks[ti], "}", TokenKind::CloseBrace) && --depth == 0) {
                close_i = ti;
                break;
            }
        }
        if (close_i == toks.size()) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        size_t open_pos = (size_t)tok_pos(toks[open_i]);
        size_t close_pos = (size_t)tok_pos(toks[close_i]);
        std::string body = flat.substr(open_pos + 1, close_pos - open_pos - 1);
        auto raw_items = split_top_level(body);
        std::vector<std::string> items;
        for (auto& item : raw_items) {
            std::string trimmed = trim_copy(item);
            if (!trimmed.empty())
                items.push_back(trimmed);
        }
        if (items.size() <= 1) {
            for (size_t k = i; k <= end; ++k)
                out.push_back(lines[k]);
            i = end + 1;
            continue;
        }

        std::string leading = leading_horizontal_whitespace_tokenized(lines[i]);
        std::string item_indent = leading + std::string(opts.indent_size, ' ');
        std::string header = trim_right_copy(flat.substr(0, open_pos));
        if (opts.statement.begin_newline) {
            out.push_back(leading + header);
            out.push_back(leading + "{");
        } else {
            out.push_back(leading + header + " {");
        }
        for (size_t k = 0; k < items.size(); ++k) {
            out.push_back(item_indent + items[k] + (k + 1 < items.size() ? "," : ""));
        }
        out.push_back(leading + "}" + trim_copy(flat.substr(close_pos + 1)));
        i = end + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// `define continuation backslash alignment pass
// ---------------------------------------------------------------------------
// Groups consecutive lines ending with '\' and aligns the '\' to a common
// column (max content width + 1 space).

static std::vector<std::string> align_define_continuation_pass(std::vector<std::string> lines,
                                                                 const FormatOptions& opts) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);
    auto ends_with_bs = [](const std::string& ln) -> bool {
        for (auto toks = collect_lexer_tokens(ln); !toks.empty();) {
            Tok tok = toks.back();
            toks.pop_back();
            if (tok_whitespace(tok))
                continue;
            std::string text = tok_text(tok);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.pop_back();
            return !text.empty() && text.back() == '\\';
        }
        return false;
    };
    auto content_width = [](const std::string& ln) -> size_t {
        size_t e = 0;
        for (const auto& tok : collect_lexer_tokens(ln)) {
            if (!tok_whitespace(tok))
                e = (size_t)tok_pos(tok) + tok_text(tok).size();
        }
        if (e > 0 && ln[e - 1] == '\\')
            --e;
        while (e > 0 && (ln[e - 1] == ' ' || ln[e - 1] == '\t'))
            --e;
        return e;
    };

    std::vector<std::string> out;
    size_t i = 0;
    while (i < lines.size()) {
        if (line_has_pp_conditional(lines[i]) || !ends_with_bs(lines[i])) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        size_t start = i;
        while (i < lines.size() && !line_has_pp_conditional(lines[i]) &&
               ends_with_bs(lines[i]))
            ++i;
        // lines[start..i) all end with '\'
        size_t max_cw = 0;
        for (size_t k = start; k < i; ++k)
            max_cw = std::max(max_cw, content_width(lines[k]));
        int bs_col = opts.tab_align
                         ? snap_to_indent_grid((int)max_cw + 1, opts.indent_size)
                         : (int)max_cw + 1;
        for (size_t k = start; k < i; ++k) {
            size_t cw = content_width(lines[k]);
            int pad = bs_col - (int)cw;
            if (pad < 1)
                pad = 1;
            out.push_back(lines[k].substr(0, cw) + std::string(pad, ' ') + "\\");
        }
    }
    return out;
}


static void verify_safe_mode_unchanged(const std::string& source, const std::string& formatted,
                                       const FormatOptions& opts) {
    if (!opts.safe_mode)
        return;

    auto strip = [](const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s)
            if (!std::isspace((unsigned char)c))
                r += c;
        return r;
    };
    if (strip(source) != strip(formatted))
        throw SafeModeError(
            "Formatter safe-mode: non-whitespace content changed — formatting aborted");
}

// ---------------------------------------------------------------------------
// tok_line_starts — returns start indices of each logical line
// ---------------------------------------------------------------------------
static std::vector<size_t> tok_line_starts(const std::vector<Tok>& tokens) {
    std::vector<size_t> starts;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i == 0 || (tokens[i].fmt_newline_before && !tok_whitespace(tokens[i]))) {
            starts.push_back(i);
        }
    }
    return starts;
}

// ---------------------------------------------------------------------------
// align_define_continuation_pass_v2 — token-metadata version
//
// Aligns trailing continuation backslashes for consecutive macro/body lines.
// The pass keeps token text immutable: the '\' is a standalone token and this
// pass changes only fmt_spaces_before on that token.
//
// Example:
//
//   `define FOO(a) a = 1; \
//     b = 2; \
//     long_name = 3
//
// becomes:
//
//   `define FOO(a) a = 1; \
//     b = 2;              \
//     long_name = 3
//
// The target backslash column is max(content width before '\') + 1, optionally
// snapped to the indent grid when tab_align is enabled.
// ---------------------------------------------------------------------------
static void align_define_continuation_pass_v2(std::vector<Tok>& tokens,
                                               const FormatOptions& opts) {
    auto starts = tok_line_starts(tokens);

    // For each logical line, determine: (a) whether it ends with backslash,
    // (b) the content width before the backslash, (c) whether it has pp conditional.
    // A "line end" token is the last non-whitespace token before the next line start.

    struct LineInfo {
        size_t start_idx;       // first token index
        size_t end_idx;         // one past last token index
        bool ends_with_bs;      // last significant token text ends with '\'
        bool has_pp_cond;       // line contains pp conditional
        bool disabled;          // line is in disabled region
        size_t bs_tok_idx;      // index of the backslash-ending token
        int content_col;        // rendered column of content before backslash
    };

    std::vector<LineInfo> line_infos;
    line_infos.reserve(starts.size());

    for (size_t li = 0; li < starts.size(); ++li) {
        size_t s = starts[li];
        size_t e = (li + 1 < starts.size()) ? starts[li + 1] : tokens.size();

        LineInfo info{};
        info.start_idx = s;
        info.end_idx = e;
        info.ends_with_bs = false;
        info.has_pp_cond = false;
        info.disabled = false;
        info.bs_tok_idx = 0;
        info.content_col = 0;

        // Scan line tokens
        size_t last_sig = s; // last significant (non-whitespace) token
        bool found_sig = false;
        for (size_t j = s; j < e; ++j) {
            if (tok_whitespace(tokens[j]))
                continue;
            if (tokens[j].fmt_disabled) {
                info.disabled = true;
                break;
            }
            found_sig = true;
            last_sig = j;
            if (tok_directive(tokens[j]) && is_pp_conditional(tok_directive_kind(tokens[j])))
                info.has_pp_cond = true;
        }

        if (found_sig && !info.disabled) {
            if (tok_text(tokens[last_sig]) == "\\") {
                info.ends_with_bs = true;
                info.bs_tok_idx = last_sig;

                // Compute rendered column of content before backslash.
                // Walk tokens on this line, accumulating column position.
                int col = tokens[s].fmt_indent * opts.indent_size;
                for (size_t j = s; j < e; ++j) {
                    if (tok_whitespace(tokens[j]))
                        continue;
                    if (j == last_sig) {
                        // Backslash is its own immutable token. col is already
                        // the rendered content column before the backslash.
                        info.content_col = col;
                        break;
                    }
                    col += tokens[j].fmt_spaces_before;
                    col += (int)tok_text(tokens[j]).size();
                }
            }
        }

        line_infos.push_back(info);
    }

    // Group consecutive lines that end with backslash (excluding disabled/pp-conditional)
    size_t i = 0;
    while (i < line_infos.size()) {
        if (line_infos[i].disabled || line_infos[i].has_pp_cond || !line_infos[i].ends_with_bs) {
            ++i;
            continue;
        }
        size_t group_start = i;
        while (i < line_infos.size() && !line_infos[i].disabled &&
               !line_infos[i].has_pp_cond && line_infos[i].ends_with_bs)
            ++i;
        // line_infos[group_start..i) is a group of continuation lines
        int max_cw = 0;
        for (size_t k = group_start; k < i; ++k)
            max_cw = std::max(max_cw, line_infos[k].content_col);

        int bs_col = opts.tab_align
                         ? snap_to_indent_grid(max_cw + 1, opts.indent_size)
                         : max_cw + 1;

        for (size_t k = group_start; k < i; ++k) {
            auto& info = line_infos[k];
            auto& bs_tok = tokens[info.bs_tok_idx];

            // Recompute how many spaces we need between content and backslash
            int pad = bs_col - info.content_col;
            if (pad < 1)
                pad = 1;

            // Keep token text immutable. Alignment is represented as spacing
            // metadata before the standalone "\" token.
            bs_tok.fmt_spaces_before = pad;
        }
    }
}

// ---------------------------------------------------------------------------
// align_assign_pass_v2 — token-based assignment alignment
// ---------------------------------------------------------------------------
static void align_assign_pass_v2(std::vector<Tok>& tokens, const FormatOptions& opts) {
    if (!opts.statement.align)
        return;

    // --- Build logical lines from token stream ---
    struct Line {
        size_t first_sig;       // index of first significant (non-ws) token
        size_t end;             // one-past-last token index for this line
        int indent_level;
        bool disabled;
        bool has_pp_cond;
        bool is_var;
        bool is_module_header_param;
        size_t assign_op_idx;   // index of assignment op token (SIZE_MAX if none)
        int lhs_inline_width;   // inline width from first_sig to just before assign_op
        bool empty;             // no significant tokens
        bool prev_starts_for;   // previous line starts with ForKeyword
        bool in_enum;           // line is inside an enum { } body
    };

    std::vector<Line> lines;

    // Walk tokens building line boundaries
    // A line starts at each non-ws token that has fmt_newline_before=true, or is the very first
    // non-ws token.
    bool next_is_line_start = true;
    size_t cur_line_first_sig = SIZE_MAX;
    bool align_pending_enum_brace = false;
    std::vector<bool> align_brace_is_enum;

    auto finalize_line = [&](size_t end_idx) {
        if (cur_line_first_sig == SIZE_MAX) {
            // empty line
            lines.push_back({SIZE_MAX, end_idx, 0, false, false, false, false, SIZE_MAX, 0, true, false, false});
            return;
        }
        Line ln{};
        ln.first_sig = cur_line_first_sig;
        ln.end = end_idx;
        ln.indent_level = tokens[cur_line_first_sig].fmt_indent;
        ln.disabled = false;
        ln.has_pp_cond = false;
        ln.empty = false;
        ln.in_enum = !align_brace_is_enum.empty() && align_brace_is_enum.back();

        // Scan tokens on this line
        std::vector<size_t> sig_indices;
        int paren = 0, bracket = 0, brace = 0;
        ln.assign_op_idx = SIZE_MAX;

        for (size_t k = cur_line_first_sig; k < end_idx; ++k) {
            const auto& tok = tokens[k];
            if (tok_whitespace(tok))
                continue;
            if (tok.fmt_disabled)
                ln.disabled = true;
            if (tok_directive(tok) && is_pp_conditional(tok_directive_kind(tok)))
                ln.has_pp_cond = true;

            sig_indices.push_back(k);
        }

        // Find assignment op at depth 0
        if (!ln.disabled && !ln.has_pp_cond) {
            static const std::vector<std::string> OPS = {"<<<=", ">>>=", "<<=", ">>=", "<=", "+=", "-=",
                                                         "*=",   "/=",   "%=",  "&=",  "|=", "^=", "="};
            paren = 0; bracket = 0; brace = 0;
            for (size_t si = 0; si < sig_indices.size(); ++si) {
                const auto& tok = tokens[sig_indices[si]];
                if (tok_comment(tok))
                    break;
                if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                    ++paren;
                else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren > 0)
                    --paren;
                else if (tok_is(tok, "[", TokenKind::OpenBracket))
                    ++bracket;
                else if (tok_is(tok, "]", TokenKind::CloseBracket) && bracket > 0)
                    --bracket;
                else if (tok_is(tok, "{", TokenKind::OpenBrace))
                    ++brace;
                else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace > 0)
                    --brace;
                if (paren == 0 && bracket == 0 && brace == 0 &&
                    std::find(OPS.begin(), OPS.end(), tok_text(tok)) != OPS.end()) {
                    ln.assign_op_idx = sig_indices[si];
                    break;
                }
            }
        }

        // is_var check
        ln.is_var = false;
        {
            // Collect non-ws, non-comment, non-directive significant tokens
            std::vector<size_t> code_sigs;
            for (auto idx : sig_indices) {
                const auto& tok = tokens[idx];
                if (!tok_comment(tok) && !tok_directive(tok))
                    code_sigs.push_back(idx);
            }
            if (code_sigs.size() >= 3 &&
                tok_is(tokens[code_sigs.back()], ";", TokenKind::Semicolon) &&
                !is_port_direction_token(tokens[code_sigs[0]])) {
                size_t ci = 0;
                while (ci < code_sigs.size() && is_var_prefix_token(tokens[code_sigs[ci]]))
                    ++ci;
                if (ci < code_sigs.size()) {
                    if (is_var_builtin_type_token(tokens[code_sigs[ci]])) {
                        ln.is_var = true;
                    } else if (is_identifier(tokens[code_sigs[ci]]) &&
                               !is_keyword(tokens[code_sigs[ci]]) &&
                               ci + 1 < code_sigs.size() &&
                               (is_identifier(tokens[code_sigs[ci + 1]]) ||
                                tok_is(tokens[code_sigs[ci + 1]], "[", TokenKind::OpenBracket) ||
                                is_sign_qualifier_token(tokens[code_sigs[ci + 1]]))) {
                        ln.is_var = true;
                    }
                }
            }
        }

        // is_module_header_parameter check
        ln.is_module_header_param = false;
        if (ln.assign_op_idx != SIZE_MAX && !sig_indices.empty() &&
            tokens[sig_indices[0]].kind == TokenKind::ParameterKeyword) {
            // Walk backward through previous lines
            for (int prev = (int)lines.size() - 1; prev >= 0; --prev) {
                const auto& pl = lines[(size_t)prev];
                if (pl.empty)
                    continue;
                if (pl.first_sig == SIZE_MAX)
                    continue;
                // Check if previous line is comment-only
                bool is_comment_only = true;
                for (size_t k = pl.first_sig; k < pl.end; ++k) {
                    if (tok_whitespace(tokens[k])) continue;
                    if (!tok_comment(tokens[k])) { is_comment_only = false; break; }
                }
                if (is_comment_only)
                    continue;

                // Check if it starts with module/interface/program and has #(
                std::vector<size_t> prev_sigs;
                for (size_t k = pl.first_sig; k < pl.end; ++k) {
                    if (!tok_whitespace(tokens[k]) && !tok_comment(tokens[k]) && !tok_directive(tokens[k]))
                        prev_sigs.push_back(k);
                }
                if (!prev_sigs.empty() &&
                    (tokens[prev_sigs[0]].kind == TokenKind::ModuleKeyword ||
                     tokens[prev_sigs[0]].kind == TokenKind::InterfaceKeyword ||
                     tokens[prev_sigs[0]].kind == TokenKind::ProgramKeyword)) {
                    for (size_t pi = 1; pi + 1 < prev_sigs.size(); ++pi) {
                        if (tok_is(tokens[prev_sigs[pi]], "#", TokenKind::Hash) &&
                            tok_is(tokens[prev_sigs[pi + 1]], "(", TokenKind::OpenParenthesis)) {
                            ln.is_module_header_param = true;
                            break;
                        }
                    }
                }
                // Check for semicolon or )( which would mean NOT a header
                bool has_semi = false, has_close_open = false;
                for (size_t pi = 0; pi < prev_sigs.size(); ++pi) {
                    has_semi = has_semi || tok_is(tokens[prev_sigs[pi]], ";", TokenKind::Semicolon);
                    if (pi + 1 < prev_sigs.size() &&
                        tok_is(tokens[prev_sigs[pi]], ")", TokenKind::CloseParenthesis) &&
                        tok_is(tokens[prev_sigs[pi + 1]], "(", TokenKind::OpenParenthesis))
                        has_close_open = true;
                }
                if (has_semi || has_close_open)
                    break;
                if (ln.is_module_header_param)
                    break;
            }
        }

        // Compute lhs_inline_width — width of the LHS expression before the assignment op.
        // For continuous assignments ("assign <expr> = ..."), skip the leading "assign" keyword
        // so that lhs_min_width applies to the actual LHS expression, not the "assign" prefix.
        ln.lhs_inline_width = 0;
        if (ln.assign_op_idx != SIZE_MAX) {
            bool first = true;
            bool skip_next = tokens[ln.first_sig].kind == TokenKind::AssignKeyword;
            for (size_t k = ln.first_sig; k < ln.end; ++k) {
                if (tok_whitespace(tokens[k]))
                    continue;
                if (k == ln.assign_op_idx)
                    break;
                if (skip_next) {
                    skip_next = false;
                    continue;
                }
                if (first) {
                    ln.lhs_inline_width += (int)tok_text(tokens[k]).size();
                    first = false;
                } else {
                    ln.lhs_inline_width += tokens[k].fmt_spaces_before + (int)tok_text(tokens[k]).size();
                }
            }
        }

        ln.prev_starts_for = false;
        lines.push_back(ln);
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& tok = tokens[i];
        if (tok_whitespace(tok)) {
            if (tok.fmt_newline_before)
                next_is_line_start = true;
            continue;
        }
        if (tok.fmt_newline_before && i > 0) {
            // Close previous line
            finalize_line(i);
            cur_line_first_sig = SIZE_MAX;
            next_is_line_start = false; // we handle it right here
        }
        if (next_is_line_start || cur_line_first_sig == SIZE_MAX) {
            if (cur_line_first_sig == SIZE_MAX)
                cur_line_first_sig = i;
            next_is_line_start = false;
        }
        if (cur_line_first_sig == SIZE_MAX)
            cur_line_first_sig = i;
        // Track enum brace nesting so finalize_line can mark enum-body lines
        if (tok.kind == TokenKind::EnumKeyword) {
            align_pending_enum_brace = true;
        } else if (tok_is(tok, "{", TokenKind::OpenBrace)) {
            align_brace_is_enum.push_back(align_pending_enum_brace);
            align_pending_enum_brace = false;
        } else if (tok_is(tok, "}", TokenKind::CloseBrace)) {
            if (!align_brace_is_enum.empty())
                align_brace_is_enum.pop_back();
        }
    }
    // Finalize last line
    if (cur_line_first_sig != SIZE_MAX)
        finalize_line(tokens.size());

    // Set prev_starts_for for each line
    for (size_t li = 1; li < lines.size(); ++li) {
        const auto& prev = lines[li - 1];
        if (!prev.empty && prev.first_sig != SIZE_MAX) {
            lines[li].prev_starts_for = (tokens[prev.first_sig].kind == TokenKind::ForKeyword);
        }
    }

    // --- Group and align ---
    size_t li = 0;
    while (li < lines.size()) {
        const auto& ln = lines[li];
        if (ln.empty || ln.disabled || ln.has_pp_cond || ln.is_var ||
            ln.is_module_header_param || ln.assign_op_idx == SIZE_MAX || ln.in_enum) {
            ++li;
            continue;
        }

        // Check: if previous line starts with 'for', skip this line (no grouping)
        if (ln.prev_starts_for) {
            // Consume all consecutive same-indent assign lines after 'for'
            size_t j = li;
            while (j < lines.size() && !lines[j].empty && !lines[j].disabled &&
                   !lines[j].has_pp_cond && !lines[j].is_var &&
                   !lines[j].is_module_header_param &&
                   lines[j].assign_op_idx != SIZE_MAX &&
                   lines[j].indent_level == ln.indent_level) {
                ++j;
            }
            li = j;
            continue;
        }

        // Collect group of consecutive alignable lines
        size_t j = li;
        while (j < lines.size()) {
            const auto& lj = lines[j];
            if (lj.empty || lj.disabled || lj.has_pp_cond || lj.is_var ||
                lj.is_module_header_param || lj.assign_op_idx == SIZE_MAX)
                break;
            if (lj.indent_level != ln.indent_level)
                break;
            if (j > li && lj.prev_starts_for)
                break;
            ++j;
        }

        // op_gap: cols between widest LHS and the operator.
        // With space_after=true, the trailing space after the op provides visual separation,
        // so op_gap=1 suffices. With space_after=false, we add an extra col before the op to
        // keep the RHS content at the same visual column as when space_after=true.
        bool space_before = binary_space_before(opts.spacing.assignment_operator_spacing);
        bool space_after  = binary_space_after(opts.spacing.assignment_operator_spacing);
        int  op_gap       = space_after ? 1 : 2;
        int  min_sp       = space_before ? 1 : 0;

        if (j - li < 2) {
            // Single line: still apply lhs_min_width if needed
            if (j - li == 1 && opts.statement.lhs_min_width > 0) {
                const auto& sl = lines[li];
                int base_width = opts.statement.align_adaptive
                    ? opts.statement.lhs_min_width
                    : std::max(opts.statement.lhs_min_width, sl.lhs_inline_width);
                int target = base_width + (space_before ? 1 : 0);
                if (opts.tab_align && opts.indent_size > 0)
                    target = snap_to_indent_grid(sl.indent_level * opts.indent_size + target, opts.indent_size) - sl.indent_level * opts.indent_size;
                int sp = std::max(min_sp, target - sl.lhs_inline_width);
                tokens[sl.assign_op_idx].fmt_spaces_before = sp;
            }
            li = j;
            continue;
        }

        // Compute alignment
        // With align_adaptive, fix target at lhs_min_width — wide lines get min_sp only.
        int max_lhs = opts.statement.lhs_min_width;
        if (!opts.statement.align_adaptive) {
            for (size_t k = li; k < j; ++k)
                max_lhs = std::max(max_lhs, lines[k].lhs_inline_width);
        }

        int target = max_lhs + op_gap;
        if (opts.tab_align && opts.indent_size > 0)
            target = snap_to_indent_grid(ln.indent_level * opts.indent_size + max_lhs + op_gap, opts.indent_size) - ln.indent_level * opts.indent_size;

        for (size_t k = li; k < j; ++k) {
            int sp = std::max(min_sp, target - lines[k].lhs_inline_width);
            tokens[lines[k].assign_op_idx].fmt_spaces_before = sp;
        }

        li = j;
    }
}

// ---------------------------------------------------------------------------
// align_var_pass_v2 — token-based variable declaration alignment
// ---------------------------------------------------------------------------

struct VarTokenParsed {
    bool valid{false};
    size_t first_sig;         // first significant token index
    size_t line_end;          // one-past-last token index

    // Type+qualifier section: tokens [first_sig .. type_qual_end)
    size_t type_qual_end;     // exclusive end (next sig token index after type+qual)
    std::string type_qual_str;
    int type_qual_inline_width;

    // Dimension section (packed dims)
    size_t dim_start;         // SIZE_MAX if none
    size_t dim_end;           // exclusive end
    std::string dim_str;
    int dim_inline_width;

    // Declarators
    struct Declarator {
        size_t name_idx;       // index of first token of declarator name
        std::string name_str;
        size_t trail_start;    // first token of trailing (unpacked dim, = init)
        size_t trail_end;      // exclusive end (before comma or semicolon)
        std::string trail_str;
    };
    std::vector<Declarator> declarators;

    // Trailing comment
    size_t semicolon_idx;     // terminating semicolon token
    size_t comment_idx;       // SIZE_MAX if none
    std::string comment_str;
};

static VarTokenParsed parse_var_tokens(const std::vector<Tok>& tokens, size_t line_start,
                                        size_t line_end, const FormatOptions& opts) {
    VarTokenParsed result{};

    // Collect significant (non-ws, non-comment, non-directive) token indices and all non-ws indices
    std::vector<size_t> sig_indices;   // code tokens (no ws, no comment, no directive)
    std::vector<size_t> all_nonsig;    // all non-ws indices (for comment detection)
    size_t comment_idx = SIZE_MAX;

    for (size_t k = line_start; k < line_end; ++k) {
        if (tok_whitespace(tokens[k]))
            continue;
        if (tok_comment(tokens[k])) {
            if (comment_idx == SIZE_MAX)
                comment_idx = k;
            continue;
        }
        if (tok_directive(tokens[k]))
            continue;
        sig_indices.push_back(k);
    }

    if (sig_indices.size() < 3)
        return result;
    if (!tok_is(tokens[sig_indices.back()], ";", TokenKind::Semicolon))
        return result;

    // Remove the semicolon from consideration
    size_t semi_idx = sig_indices.back();
    sig_indices.pop_back();

    if (is_port_direction_token(tokens[sig_indices[0]]))
        return result;

    // Parse type prefix
    size_t idx = 0;
    std::vector<size_t> type_parts_indices;
    while (idx < sig_indices.size() && is_var_prefix_token(tokens[sig_indices[idx]])) {
        type_parts_indices.push_back(sig_indices[idx]);
        ++idx;
    }
    if (idx >= sig_indices.size())
        return result;

    if (is_var_builtin_type_token(tokens[sig_indices[idx]])) {
        type_parts_indices.push_back(sig_indices[idx]);
        ++idx;
    } else {
        if (!is_identifier(tokens[sig_indices[idx]]) || is_keyword(tokens[sig_indices[idx]]))
            return result;
        if (idx + 1 >= sig_indices.size())
            return result;
        bool ok = (is_identifier(tokens[sig_indices[idx + 1]]) ||
                   tok_is(tokens[sig_indices[idx + 1]], "[", TokenKind::OpenBracket) ||
                   is_sign_qualifier_token(tokens[sig_indices[idx + 1]]));
        if (!ok)
            return result;
        type_parts_indices.push_back(sig_indices[idx]);
        ++idx;
    }

    // Handle scope resolution (e.g., pkg::type_t)
    while (idx < sig_indices.size() && tok_text(tokens[sig_indices[idx]]) == "::" &&
           idx + 1 < sig_indices.size()) {
        type_parts_indices.push_back(sig_indices[idx]);     // ::
        type_parts_indices.push_back(sig_indices[idx + 1]); // type name
        idx += 2;
    }

    // Build type_qual_str
    std::string type_kw_str;
    for (size_t k = 0; k < type_parts_indices.size(); ++k) {
        if (k > 0) type_kw_str += ' ';
        type_kw_str += tok_text(tokens[type_parts_indices[k]]);
    }

    // Optional qualifier (signed/unsigned)
    std::string qualifier_str;
    if (idx < sig_indices.size() && is_sign_qualifier_token(tokens[sig_indices[idx]])) {
        qualifier_str = tok_text(tokens[sig_indices[idx]]);
        ++idx;
    }

    result.type_qual_str = type_kw_str + (qualifier_str.empty() ? "" : " " + qualifier_str);

    // Compute type_qual_end: the token index of the next sig token after type+qual
    if (idx < sig_indices.size())
        result.type_qual_end = sig_indices[idx];
    else
        return result; // nothing after type — not a valid var decl

    // Compute type_qual_inline_width: width from first_sig to type_qual_end
    result.type_qual_inline_width = (int)result.type_qual_str.size();

    // Optional packed dimension
    result.dim_start = SIZE_MAX;
    result.dim_end = SIZE_MAX;
    result.dim_inline_width = 0;
    if (idx < sig_indices.size() &&
        tok_is(tokens[sig_indices[idx]], "[", TokenKind::OpenBracket)) {
        result.dim_start = sig_indices[idx];
        int depth = 0;
        std::string dim_str;
        size_t dim_first = idx;
        while (idx < sig_indices.size()) {
            const auto& tok = tokens[sig_indices[idx]];
            if (idx > dim_first) dim_str += ' ';  // space between bracket tokens
            dim_str += tok_text(tok);
            for (char c : tok_text(tok)) {
                if (c == '[') ++depth;
                else if (c == ']') --depth;
            }
            ++idx;
            if (depth <= 0)
                break;
        }
        result.dim_end = (idx < sig_indices.size()) ? sig_indices[idx] : semi_idx;
        // Rebuild dim_str from actual token range to include macro/directive tokens
        // (sig_indices skips directives, so `MACRO in dims would be missed above)
        std::string actual_dim;
        for (size_t k = result.dim_start; k < result.dim_end; ++k) {
            if (tok_whitespace(tokens[k])) continue;
            if (!actual_dim.empty()) actual_dim += ' ';
            actual_dim += tok_text(tokens[k]);
        }
        result.dim_str = normalize_bracket_spacing(actual_dim, opts);
        result.dim_inline_width = (int)result.dim_str.size();
    }

    if (idx >= sig_indices.size())
        return result; // no declarators

    // Remaining tokens are declarators separated by commas at depth 0
    // Parse declarators
    int paren = 0, bracket2 = 0, brace = 0;
    size_t decl_start = idx;

    auto flush_declarator = [&](size_t end_exclusive) {
        if (decl_start >= end_exclusive)
            return;
        // First token of this declarator should be the name
        size_t name_si = decl_start;
        size_t name_tok = sig_indices[name_si];
        const auto& name_token = tokens[name_tok];

        // Validate: must start with [A-Za-z_]
        if (tok_text(name_token).empty() ||
            (!std::isalpha((unsigned char)tok_text(name_token)[0]) && tok_text(name_token)[0] != '_'))
            return;

        std::string name_str = tok_text(name_token);

        // Everything after the name is trailing (unpacked dims, = init, etc.)
        std::string trail_str;
        size_t trail_start_si = name_si + 1;
        for (size_t k = trail_start_si; k < end_exclusive; ++k) {
            if (k > trail_start_si) trail_str += ' ';
            trail_str += tok_text(tokens[sig_indices[k]]);
        }
        trail_str = normalize_bracket_spacing(trail_str, opts);

        // Check for function call (has '(' in trailing)
        for (size_t k = trail_start_si; k < end_exclusive; ++k) {
            if (tok_is(tokens[sig_indices[k]], "(", TokenKind::OpenParenthesis))
                return; // reject
        }
        // Also check for '(' in name+trailing combined
        if (tok_is(tokens[name_tok], "(", TokenKind::OpenParenthesis))
            return;

        size_t trail_start_tok = (trail_start_si < end_exclusive) ? sig_indices[trail_start_si] : semi_idx;
        size_t trail_end_tok = (end_exclusive > 0 && end_exclusive <= sig_indices.size()) ?
            ((end_exclusive < sig_indices.size()) ? sig_indices[end_exclusive] : semi_idx) : semi_idx;

        result.declarators.push_back({name_tok, name_str, trail_start_tok, trail_end_tok, trail_str});
    };

    for (size_t k = idx; k < sig_indices.size(); ++k) {
        const auto& tok = tokens[sig_indices[k]];
        if (tok_is(tok, "(", TokenKind::OpenParenthesis)) ++paren;
        else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
        else if (tok_is(tok, "[", TokenKind::OpenBracket)) ++bracket2;
        else if (tok_is(tok, "]", TokenKind::CloseBracket) && bracket2 > 0) --bracket2;
        else if (tok_is(tok, "{", TokenKind::OpenBrace)) ++brace;
        else if (tok_is(tok, "}", TokenKind::CloseBrace) && brace > 0) --brace;
        else if (tok_is(tok, ",", TokenKind::Comma) && paren == 0 && bracket2 == 0 && brace == 0) {
            flush_declarator(k);
            decl_start = k + 1;
        }
    }
    flush_declarator(sig_indices.size());

    if (result.declarators.empty())
        return result;

    result.valid = true;
    result.first_sig = sig_indices[0];
    result.line_end = line_end;
    result.semicolon_idx = semi_idx;
    result.comment_idx = comment_idx;
    if (comment_idx != SIZE_MAX) {
        result.comment_str = " ";
        for (size_t k = comment_idx; k < line_end; ++k) {
            if (tok_whitespace(tokens[k])) continue;
            if (tok_comment(tokens[k])) {
                result.comment_str += tok_text(tokens[k]);
                break;
            }
        }
    }

    return result;
}

static void align_var_pass_v2(std::vector<Tok>& tokens, const FormatOptions& opts) {
    if (!opts.var_declaration.align)
        return;

    const auto& vo = opts.var_declaration;

    // Build logical lines
    struct Line {
        size_t start;   // first token index (may be ws)
        size_t end;     // one-past-last
        size_t first_sig;
        bool disabled;
        bool has_pp_cond;
        bool is_comment_or_blank;
        VarTokenParsed parsed;
    };

    std::vector<Line> lines;
    bool next_is_line_start = true;
    size_t cur_line_start = 0;
    size_t cur_first_sig = SIZE_MAX;

    auto finalize_line = [&](size_t end_idx) {
        Line ln{};
        ln.start = cur_line_start;
        ln.end = end_idx;
        ln.first_sig = cur_first_sig;
        ln.disabled = false;
        ln.has_pp_cond = false;
        ln.is_comment_or_blank = true;

        for (size_t k = cur_line_start; k < end_idx; ++k) {
            if (tok_whitespace(tokens[k])) continue;
            if (tokens[k].fmt_disabled) ln.disabled = true;
            if (tok_directive(tokens[k]) && is_pp_conditional(tok_directive_kind(tokens[k])))
                ln.has_pp_cond = true;

            if (!tok_comment(tokens[k]))
                ln.is_comment_or_blank = false;
        }
        if (cur_first_sig == SIZE_MAX)
            ln.is_comment_or_blank = true;

        if (!ln.disabled && !ln.has_pp_cond && !ln.is_comment_or_blank && cur_first_sig != SIZE_MAX) {
            ln.parsed = parse_var_tokens(tokens, cur_first_sig, end_idx, opts);
        }

        lines.push_back(std::move(ln));
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& tok = tokens[i];
        if (tok_whitespace(tok)) {
            if (tok.fmt_newline_before)
                next_is_line_start = true;
            continue;
        }
        if (tok.fmt_newline_before && i > 0) {
            finalize_line(i);
            cur_line_start = i;
            cur_first_sig = SIZE_MAX;
            next_is_line_start = false;
        }
        if (next_is_line_start) {
            cur_line_start = i;
            cur_first_sig = SIZE_MAX;
            next_is_line_start = false;
        }
        if (cur_first_sig == SIZE_MAX)
            cur_first_sig = i;
    }
    if (cur_first_sig != SIZE_MAX || cur_line_start < tokens.size())
        finalize_line(tokens.size());

    // Group consecutive lines where var declarations are found
    // (comments/blanks pass through without breaking the group)
    size_t li = 0;
    while (li < lines.size()) {
        if (lines[li].disabled || lines[li].has_pp_cond || !lines[li].parsed.valid) {
            ++li;
            continue;
        }

        // Collect block
        struct BlkEntry {
            size_t line_idx;
            bool is_var;
        };
        std::vector<BlkEntry> block;
        size_t j = li;
        while (j < lines.size()) {
            const auto& lj = lines[j];
            if (lj.disabled || lj.has_pp_cond)
                break;
            if (lj.parsed.valid) {
                block.push_back({j, true});
                ++j;
                continue;
            }
            if (lj.is_comment_or_blank) {
                block.push_back({j, false});
                ++j;
                continue;
            }
            break;
        }

        // Count parseable entries
        int np = 0;
        for (auto& e : block)
            if (e.is_var) ++np;

        if (np <= 0) {
            li = j;
            continue;
        }

        // Compute section widths
        int max_s1 = 0;
        for (auto& e : block) {
            if (!e.is_var) continue;
            max_s1 = std::max(max_s1, (int)lines[e.line_idx].parsed.type_qual_str.size());
        }
        int vo_s1_min = tab_aligned_width(vo.section1_min_width, opts);
        int vo_s2_min = tab_aligned_width(vo.section2_min_width, opts);
        int vo_s3_min = tab_aligned_width(vo.section3_min_width, opts);
        int vo_s4_min = tab_aligned_width(vo.section4_min_width, opts);

        int s1_w = tab_aligned_width(std::max(vo_s1_min, max_s1 + 1), opts);

        int max_dim = 0;
        for (auto& e : block) {
            if (!e.is_var) continue;
            max_dim = std::max(max_dim, (int)lines[e.line_idx].parsed.dim_str.size());
        }
        int s2_w = max_dim > 0 ? tab_aligned_width(std::max(vo_s2_min, max_dim + 1), opts) : 0;

        size_t max_slots = 0;
        for (auto& e : block)
            if (e.is_var)
                max_slots = std::max(max_slots, lines[e.line_idx].parsed.declarators.size());

        std::vector<int> id_widths(max_slots, 0), trail_widths(max_slots, 0);
        for (size_t slot = 0; slot < max_slots; ++slot) {
            int mx_id = 0, mx_tr = 0;
            for (auto& e : block) {
                if (!e.is_var) continue;
                const auto& vp = lines[e.line_idx].parsed;
                if (slot < vp.declarators.size()) {
                    mx_id = std::max(mx_id, (int)vp.declarators[slot].name_str.size());
                    mx_tr = std::max(mx_tr, (int)vp.declarators[slot].trail_str.size());
                }
            }
            id_widths[slot] = tab_aligned_width(std::max(vo_s3_min, mx_id + 1), opts);
            trail_widths[slot] = tab_aligned_width(std::max(vo_s4_min, mx_tr), opts);
        }

        // Apply alignment to tokens
        for (auto& e : block) {
            if (!e.is_var) continue;
            const auto& vp = lines[e.line_idx].parsed;

            int line_s1_w = s1_w;
            int line_s2_w = s2_w;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;

            if (vo.align_adaptive) {
                std::string s1part = vp.type_qual_str;
                int t1 = vo_s1_min;
                int t2 = t1 + (s2_w > 0 ? vo_s2_min : 0);

                int e1 = tab_aligned_width(std::max(t1, (int)s1part.size() + 1), opts);
                line_s1_w = e1;

                int e2 = e1;
                if (s2_w > 0) {
                    int c2 = vp.dim_str.empty() ? 0 : (int)vp.dim_str.size() + 1;
                    e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                    line_s2_w = e2 - e1;
                }

                line_trail_widths.clear();
                for (const auto& decl : vp.declarators)
                    line_trail_widths.push_back(
                        tab_aligned_width(std::max(vo_s4_min, (int)decl.trail_str.size()), opts));
            }

            // Now set fmt_spaces_before on the tokens to achieve alignment
            // Section 1 → type+qualifier: pad to line_s1_w
            // The token after type+qualifier gets fmt_spaces_before adjusted
            size_t next_after_type = vp.type_qual_end;
            int s1_spaces = std::max(1, line_s1_w - (int)vp.type_qual_str.size());
            tokens[next_after_type].fmt_spaces_before = s1_spaces;

            // Section 2 → dim: pad to line_s2_w (if dims exist in block)
            if (line_s2_w > 0 && vp.dim_start != SIZE_MAX) {
                // dim tokens are already in place, need to pad to line_s2_w
                // The first declarator name comes after dim
                if (!vp.declarators.empty()) {
                    int dim_space = std::max(1, line_s2_w - (int)vp.dim_str.size());
                    tokens[vp.declarators[0].name_idx].fmt_spaces_before = dim_space;
                }
            } else if (line_s2_w > 0 && vp.dim_start == SIZE_MAX) {
                // No dim on this line but block has dims — add s2_w padding to name
                if (!vp.declarators.empty()) {
                    int existing = tokens[vp.declarators[0].name_idx].fmt_spaces_before;
                    tokens[vp.declarators[0].name_idx].fmt_spaces_before = existing + line_s2_w;
                }
            }

            // Section 3+4 → declarator names and trailing
            size_t nd = vp.declarators.size();
            for (size_t k = 0; k < nd; ++k) {
                bool is_last = (k == nd - 1);
                const auto& decl = vp.declarators[k];

                // Name width padding: for non-last declarators and last one if id_widths available
                if (k < line_id_widths.size() && !decl.trail_str.empty()) {
                    // Need to set spacing on trail_start token
                    if (decl.trail_start != decl.trail_end && decl.trail_start < tokens.size()) {
                        int name_pad = std::max(1, line_id_widths[k] - (int)decl.name_str.size());
                        tokens[decl.trail_start].fmt_spaces_before = name_pad;
                    }
                } else if (k < line_id_widths.size() && decl.trail_str.empty()) {
                    // No trailing: for non-last, need to pad name to id_width + trail_width before comma
                    // The comma/semicolon spacing is handled differently
                }

                if (!is_last) {
                    // After trail, there's a comma — find it and set spacing for next decl name
                    // The comma token is between this decl's trail_end and next decl's name_idx
                    if (k + 1 < nd) {
                        // Set spacing before next declarator's name
                        // (accounts for ", " between declarators)
                    }
                } else {
                    // Last declarator: align the semicolon as the end of section 4.
                    if (vp.semicolon_idx < tokens.size()) {
                        int id_w = (k < line_id_widths.size()) ? line_id_widths[k]
                                                               : (int)decl.name_str.size() + 1;
                        int trail_w = (k < line_trail_widths.size()) ? line_trail_widths[k] : 0;
                        int pad = 1;
                        if (decl.trail_str.empty())
                            pad = std::max(1, id_w - (int)decl.name_str.size() + trail_w);
                        else
                            pad = std::max(0, trail_w - (int)decl.trail_str.size());
                        tokens[vp.semicolon_idx].fmt_spaces_before = pad;
                    }
                }
            }
        }

        li = j;
    }
}


// ---------------------------------------------------------------------------
// render_tokens — build output string from metadata-annotated tokens
// ---------------------------------------------------------------------------
static std::string render_tokens(const std::vector<Tok>& tokens, const FormatOptions& opts) {
    std::string out;
    size_t total_len = 0;
    for (const auto& t : tokens) total_len += tok_text(t).size();
    out.reserve(total_len + total_len / 4);

    const std::string indent_unit(opts.indent_size, ' ');
    bool at_bol = true;

    for (const auto& tok : tokens) {
        if (tok.fmt_passthrough) {
            if (tok.fmt_newline_before) {
                if (!at_bol) out += '\n';
                for (int k = 0; k < tok.fmt_blank_lines; ++k)
                    out += '\n';
                at_bol = true;
            }
            if (!at_bol && tok.fmt_spaces_before > 0)
                out.append(tok.fmt_spaces_before, ' ');
            out += tok_text(tok);
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            continue;
        }
        if (tok_whitespace(tok)) continue;

        if (tok.fmt_newline_before) {
            if (!at_bol) out += '\n';
            for (int k = 0; k < tok.fmt_blank_lines; ++k)
                out += '\n';
            at_bol = true;
        }

        if (at_bol && !tok_text(tok).empty()) {
            for (int k = 0; k < tok.fmt_indent; ++k)
                out += indent_unit;
            if (tok.fmt_spaces_before > 0)
                out.append(tok.fmt_spaces_before, ' ');
            at_bol = false;
        } else if (!at_bol && tok.fmt_spaces_before > 0) {
            out.append(tok.fmt_spaces_before, ' ');
        }

        out += tok_text(tok);
        if (!tok_text(tok).empty() && tok_text(tok).back() == '\n')
            at_bol = true;
    }

    return out;
}

// ---------------------------------------------------------------------------
// basic_formatting — set fmt_* fields on each token
// ---------------------------------------------------------------------------
static void basic_formatting(std::vector<Tok>& tokens, const std::string& input,
                                    const FormatOptions& opts) {
    // Pass 0 is the formatter's structural scan.  It does not rewrite token text;
    // it annotates each token with enough layout metadata for render_tokens():
    //
    //   fmt_indent          indentation level if the token starts a rendered line
    //   fmt_spaces_before   horizontal spacing before the token on the same line
    //   fmt_newline_before  whether render_tokens() should break before the token
    //   fmt_blank_lines     preserved/capped blank lines attached to that break
    //   fmt_disabled        token belongs to a format-off region
    //   fmt_passthrough     token should be emitted exactly as collected
    //
    // Later passes can still adjust this metadata, but this pass establishes the
    // baseline line breaking and indentation by walking the token stream once and
    // maintaining lightweight parser state (indent stack, grouping depths, macro
    // roles, preprocessor conditionals, case labels, etc.).
    auto disabled = find_disabled(input);
    const MacroClassifier macro_classifier(opts.macros);

    // Current logical indentation level.  indent_stack mirrors every construct
    // that increments indent_level so the corresponding close token can subtract
    // the exact delta (important because the outermost module/package/interface
    // indent can be configured independently from ordinary block indentation).
    int indent_level = 0;
    std::vector<int> indent_stack;

    // True when the next emitted non-whitespace token will be at the beginning of
    // a rendered line.  This is separate from original source newlines: source
    // whitespace is consumed and converted into fmt_* metadata.
    bool at_bol = true;

    // Bracket depth for packed/unpacked dimensions and selects.  While non-zero,
    // some spacing and wrapping rules are intentionally suppressed so expressions
    // such as [WIDTH-1:0] are kept compact.
    int dim_depth = 0;

    // Parenthesis stack.  paren_depth is the numeric nesting level; paren_stack
    // records what kind of parenthesis each open token introduced; and
    // for_header_stack marks parentheses belonging to for/foreach headers, where
    // semicolons should not terminate statements or force a newline.
    int paren_depth = 0;
    std::vector<ParenKind> paren_stack;
    std::vector<bool> for_header_stack;

    // Tracks unmatched "do" keywords so "end while (...)" / "while (...)" tails
    // are formatted as part of the do-while construct rather than as an ordinary
    // while statement starting a new single-statement body.
    int do_depth = 0;

    // Deferred layout requests.  Many tokens (for example semicolons, begin/end,
    // comments, or macro invocations) decide that the *next* significant token
    // must start a new line; pending_nl carries that decision until the next
    // non-whitespace token.  original_newline_before_token remembers whether the
    // source had a newline immediately before the current token, which is needed
    // to distinguish inline comments from standalone comments.
    bool pending_nl = false;
    bool original_newline_before_token = false;

    // Number of blank lines to preserve before the next token.  It is capped by
    // opts.blank_lines_between_items when whitespace is consumed.
    int blank_pend = 0;

    // Set while handling a split preprocessor conditional such as a directive
    // token followed by its condition expression.  The condition expression is
    // passed through so macro/preprocessor spelling and spacing are preserved.
    bool in_pp_cond = false;

    // Format-disabled regions are mostly passthrough.  after_dis bridges the
    // transition back to normal formatting, while in_define_disabled keeps a
    // multi-token `define inside a disabled region verbatim until its newline.
    bool after_dis = false;
    bool in_define_disabled = false;

    // begin/end/fork labels are written as "begin : label".  This small state
    // machine detects the colon and the label token so the label stays attached
    // to the block opener/closer before normal newline handling resumes.
    int block_label_state = 0;

    // Struct/union and constraint bodies use braces, but unlike expression braces
    // their contents should be indented as blocks.  *_pend indicates that the
    // next "{" opens such a block; constraint_depth also handles nested
    // constraint single-statement bodies.
    bool struct_pend = false;
    bool enum_pend = false;
    bool constraint_pend = false;
    int constraint_depth = 0;

    // case handling needs to distinguish the case expression, case item labels,
    // and ternary ?: operators inside case items.  case_conditional_depth counts
    // pending '?' tokens so a ':' in a ternary expression is not mistaken for a
    // case label separator.
    bool case_expr_pending = false;
    int case_expr_depth = -1;
    int case_depth = 0;
    int case_conditional_depth = 0;
    bool case_label_pending_nl = false;

    // After if/for/foreach/while/repeat, the closing ')' of the control
    // expression should schedule either a begin block or a single-statement body
    // on the next token.
    bool control_expr_pending = false;
    int control_expr_depth = -1;

    // Single-statement body indentation.  single_stmt_pending means a control
    // construct has just ended and the next token may need one extra indent;
    // single_stmt_active means that temporary indent has been applied and should
    // be removed at the statement terminator or statement-like macro.
    bool single_stmt_pending = false;
    bool single_stmt_active = false;

    // True only while formatting the trailing while of a do-while construct.
    bool do_while_tail = false;
    int define_spaces_pending = 0;

    // import/export and extern declarations can contain function/task keywords
    // that must not open normal function/task indentation scopes.
    bool in_import_export_decl = false;
    bool in_extern_decl = false;

    // Macro invocations can behave like expressions, statements, declarations,
    // or synthetic block delimiters.  Function-like macros are tracked until
    // their argument list closes; object-like macros are completed immediately.
    struct ActiveMacro {
        MacroClassification classification;
        bool wait_open{false};
        int paren_depth{-1};
    };
    std::vector<ActiveMacro> active_macros;
    bool macro_wrap_pending = false;
    bool function_macro_newline_candidate = false;
    int function_macro_newline_depth = -1;
    bool prev_macro_role_valid = false;
    MacroRole prev_macro_role = MacroRole::ObjectLikeExpr;
    bool whitespace_macro_passthrough = false;
    bool whitespace_macro_seen_open = false;
    int whitespace_macro_paren_depth = 0;
    MacroClassification whitespace_macro_class;
    Tok whitespace_macro_prev;

    // Brace stack classifies "{" scopes.  Only struct/constraint braces affect
    // indentation here; expression/concatenation braces are recorded as "other"
    // so nested closes still pair correctly.
    std::vector<std::string> brace_stk;

    // Previous significant token state.  prev_at_procedural helps classify
    // event-control parentheses following procedural @.
    const Tok* prev = nullptr;
    bool prev_at_procedural = false;

    // -----------------------------------------------------------------------
    // Helper lambdas
    // -----------------------------------------------------------------------
    auto next_significant = [&](size_t idx) -> const Tok* {
        // Look ahead past lexer whitespace and comments.  This is deliberately
        // shallow: pass0 only needs immediate context such as whether a macro is
        // followed by an argument list.
        for (size_t j = idx + 1; j < tokens.size(); ++j) {
            if (!tok_whitespace(tokens[j]) && !tok_comment(tokens[j]))
                return &tokens[j];
        }
        return nullptr;
    };
    auto macro_force_own_line = [](MacroRole role) {
        // Declaration/control/block macros are treated like structural tokens,
        // so they should not be silently appended after prior code.
        return role == MacroRole::DeclarationLike || role == MacroRole::ControlFlowLike ||
               role == MacroRole::BlockBeginLike || role == MacroRole::BlockEndLike;
    };
    auto macro_newline_after = [](MacroRole role) {
        // Statement-ish macros terminate a logical line even when they do not
        // contain a literal semicolon in the token stream.
        return role == MacroRole::StatementLike || role == MacroRole::DeclarationLike ||
               role == MacroRole::ControlFlowLike || role == MacroRole::BlockBeginLike ||
               role == MacroRole::BlockEndLike;
    };
    auto finish_macro_invocation = [&](const MacroClassification& classification) {
        // Apply the effects of a completed macro invocation.  For function-like
        // macros this is called when the matching ')' closes; for object-like
        // macros it is called at the macro token itself.
        if (macro_newline_after(classification.role)) {
            pending_nl = true;
            macro_wrap_pending = true;
        }
        if (classification.role == MacroRole::ControlFlowLike)
            single_stmt_pending = true;
        if (classification.role == MacroRole::BlockBeginLike) {
            indent_level += 1;
            indent_stack.push_back(1);
        }
        if ((classification.role == MacroRole::StatementLike ||
             classification.role == MacroRole::DeclarationLike) &&
            single_stmt_active) {
            indent_level = std::max(0, indent_level - 1);
            single_stmt_active = false;
        }
    };
    auto apply_newline = [&](size_t idx) {
        // Materialize a deferred newline/blank-line request on the current token.
        // Once attached to fmt_* metadata, the pending request is consumed.
        if (pending_nl || blank_pend > 0) {
            tokens[idx].fmt_newline_before = true;
            tokens[idx].fmt_blank_lines = blank_pend;
            pending_nl = false;
            blank_pend = 0;
            at_bol = true;
        }
    };
    auto matching_open_paren = [&](size_t close_idx) -> size_t {
        int depth = 0;
        for (size_t n = close_idx + 1; n > 0; --n) {
            size_t idx = n - 1;
            if (tok_whitespace(tokens[idx]) || tok_comment(tokens[idx]) || tok_directive(tokens[idx]))
                continue;
            if (tok_is(tokens[idx], ")", TokenKind::CloseParenthesis))
                ++depth;
            else if (tok_is(tokens[idx], "(", TokenKind::OpenParenthesis) && --depth == 0)
                return idx;
        }
        return SIZE_MAX;
    };
    auto instance_name_before_port_open = [&](size_t open_idx) -> size_t {
        size_t prev_sig = prev_code_sig(tokens, 0, open_idx);
        if (prev_sig == SIZE_MAX)
            return SIZE_MAX;
        if (is_identifier(tokens[prev_sig]))
            return prev_sig;
        if (!tok_is(tokens[prev_sig], "]", TokenKind::CloseBracket))
            return SIZE_MAX;

        int depth = 0;
        for (size_t n = prev_sig + 1; n > 0; --n) {
            size_t idx = n - 1;
            if (tok_whitespace(tokens[idx]) || tok_comment(tokens[idx]) || tok_directive(tokens[idx]))
                continue;
            if (tok_is(tokens[idx], "]", TokenKind::CloseBracket))
                ++depth;
            else if (tok_is(tokens[idx], "[", TokenKind::OpenBracket) && --depth == 0)
                return prev_code_sig(tokens, 0, idx);
        }
        return SIZE_MAX;
    };
    auto paren_list_has_top_level_named_connections = [&](size_t open_idx, size_t close_idx) -> bool {
        bool saw_named_connection = false;
        bool expect_item_start = true;
        int paren = 0, bracket = 0, brace = 0;
        bool skip_pp_cond_line = false;
        for (size_t idx = open_idx + 1; idx < close_idx && idx < tokens.size(); ++idx) {
            if (skip_pp_cond_line) {
                if (tok_whitespace(tokens[idx]) &&
                    tok_text(tokens[idx]).find('\n') != std::string::npos)
                    skip_pp_cond_line = false;
                continue;
            }
            if (is_line_directive(tokens[idx]) &&
                is_pp_conditional(tok_directive_kind(tokens[idx]))) {
                // Slang lexes conditionals like "`ifdef A" as a directive token
                // followed by ordinary identifier tokens for the condition.  For
                // list classification, the whole directive line is structural
                // trivia; otherwise the condition name ("A") is mistaken for a
                // non-.port item and the enclosing instance port list stops
                // wrapping after the conditional.
                skip_pp_cond_line = tok_text(tokens[idx]).find('\n') == std::string::npos;
                continue;
            }
            if (tok_whitespace(tokens[idx]) || tok_comment(tokens[idx]) || tok_directive(tokens[idx]))
                continue;
            if (tok_is(tokens[idx], "(", TokenKind::OpenParenthesis)) {
                ++paren;
                continue;
            }
            if (tok_is(tokens[idx], ")", TokenKind::CloseParenthesis) && paren > 0) {
                --paren;
                continue;
            }
            if (tok_is(tokens[idx], "[", TokenKind::OpenBracket)) {
                ++bracket;
                continue;
            }
            if (tok_is(tokens[idx], "]", TokenKind::CloseBracket) && bracket > 0) {
                --bracket;
                continue;
            }
            if (tok_is(tokens[idx], "{", TokenKind::OpenBrace)) {
                ++brace;
                continue;
            }
            if (tok_is(tokens[idx], "}", TokenKind::CloseBrace) && brace > 0) {
                --brace;
                continue;
            }
            if (paren == 0 && bracket == 0 && brace == 0 && tok_is(tokens[idx], ",", TokenKind::Comma)) {
                expect_item_start = true;
                continue;
            }
            if (paren == 0 && bracket == 0 && brace == 0 && expect_item_start) {
                if (!tok_is(tokens[idx], ".", TokenKind::Dot))
                    return false;
                saw_named_connection = true;
                expect_item_start = false;
            }
        }
        return saw_named_connection;
    };
    auto is_instantiation_port_list_open = [&](size_t open_idx) -> bool {
        size_t close_idx = matching_close_paren(tokens, open_idx, tokens.size());
        if (close_idx == SIZE_MAX)
            return false;
        if (!paren_list_has_top_level_named_connections(open_idx, close_idx))
            return false;
        size_t semi = next_code_sig(tokens, close_idx + 1, tokens.size());
        if (semi == SIZE_MAX || !tok_is(tokens[semi], ";", TokenKind::Semicolon))
            return false;

        size_t inst_name = instance_name_before_port_open(open_idx);
        if (inst_name == SIZE_MAX || !is_identifier(tokens[inst_name]) || is_keyword(tokens[inst_name]))
            return false;

        size_t before_inst = prev_code_sig(tokens, 0, inst_name);
        if (before_inst == SIZE_MAX)
            return false;
        if (is_identifier(tokens[before_inst]) && !is_keyword(tokens[before_inst]))
            return true;
        if (!tok_is(tokens[before_inst], ")", TokenKind::CloseParenthesis))
            return false;

        size_t param_open = matching_open_paren(before_inst);
        if (param_open == SIZE_MAX)
            return false;
        size_t hash = prev_code_sig(tokens, 0, param_open);
        if (hash == SIZE_MAX || !tok_is(tokens[hash], "#", TokenKind::Hash))
            return false;
        size_t mod = prev_code_sig(tokens, 0, hash);
        return mod != SIZE_MAX && is_identifier(tokens[mod]) && !is_keyword(tokens[mod]);
    };
    auto is_instantiation_param_list_open = [&](size_t open_idx) -> bool {
        size_t hash = prev_code_sig(tokens, 0, open_idx);
        if (hash == SIZE_MAX || !tok_is(tokens[hash], "#", TokenKind::Hash))
            return false;
        size_t mod = prev_code_sig(tokens, 0, hash);
        if (mod == SIZE_MAX || !is_identifier(tokens[mod]) || is_keyword(tokens[mod]))
            return false;

        size_t param_close = matching_close_paren(tokens, open_idx, tokens.size());
        if (param_close == SIZE_MAX)
            return false;
        if (!paren_list_has_top_level_named_connections(open_idx, param_close))
            return false;
        size_t inst_name = next_code_sig(tokens, param_close + 1, tokens.size());
        if (inst_name == SIZE_MAX || !is_identifier(tokens[inst_name]))
            return false;
        size_t port_open = next_code_sig(tokens, inst_name + 1, tokens.size());
        while (port_open != SIZE_MAX && tok_is(tokens[port_open], "[", TokenKind::OpenBracket)) {
            int depth = 1;
            size_t j = port_open;
            while (++j < tokens.size() && depth > 0) {
                if (tok_is(tokens[j], "[", TokenKind::OpenBracket))
                    ++depth;
                else if (tok_is(tokens[j], "]", TokenKind::CloseBracket))
                    --depth;
            }
            port_open = next_code_sig(tokens, j + 1, tokens.size());
        }
        return port_open != SIZE_MAX && tok_is(tokens[port_open], "(", TokenKind::OpenParenthesis) &&
               is_instantiation_port_list_open(port_open);
    };
    auto classify_paren_at = [&](size_t open_idx, bool procedural_at_context) -> ParenKind {
        size_t prev_sig = prev_code_sig(tokens, 0, open_idx);
        if (prev_sig == SIZE_MAX)
            return ParenKind::Ordinary;
        const Tok& left = tokens[prev_sig];

        if (tok_is(left, "#", TokenKind::Hash) && is_instantiation_param_list_open(open_idx))
            return ParenKind::InstantiationParamList;
        if (is_instantiation_port_list_open(open_idx))
            return ParenKind::InstantiationPortList;
        if (is_identifier(left) && !paren_stack.empty()) {
            size_t before_name = prev_code_sig(tokens, 0, prev_sig);
            if (before_name != SIZE_MAX && tok_text(tokens[before_name]) == ".") {
                if (paren_stack.back() == ParenKind::InstantiationParamList)
                    return ParenKind::InstantiationParamConnection;
                if (paren_stack.back() == ParenKind::InstantiationPortList)
                    return ParenKind::InstantiationPortConnection;
            }
        }
        return classify_paren(left, procedural_at_context);
    };
    auto is_user_type_packed_dimension_open = [&](size_t open_idx) -> bool {
        size_t type_idx = prev_code_sig(tokens, 0, open_idx);
        if (type_idx == SIZE_MAX || !is_identifier(tokens[type_idx]) || is_keyword(tokens[type_idx]))
            return false;

        int bracket_depth = 0;
        size_t close_idx = SIZE_MAX;
        for (size_t j = open_idx; j < tokens.size(); ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_is(tokens[j], "[", TokenKind::OpenBracket)) {
                ++bracket_depth;
            } else if (tok_is(tokens[j], "]", TokenKind::CloseBracket)) {
                if (bracket_depth > 0)
                    --bracket_depth;
                if (bracket_depth == 0) {
                    close_idx = j;
                    break;
                }
            } else if (tok_is(tokens[j], ";", TokenKind::Semicolon)) {
                return false;
            }
        }
        if (close_idx == SIZE_MAX)
            return false;

        size_t after_dim = next_code_sig(tokens, close_idx + 1, tokens.size());
        return after_dim != SIZE_MAX && is_identifier(tokens[after_dim]) &&
               !is_keyword(tokens[after_dim]);
    };

    // -----------------------------------------------------------------------
    // Main token loop
    // -----------------------------------------------------------------------
    for (size_t i = 0; i < tokens.size(); ++i) {
        Tok& tok = tokens[i];
        // Classify macro tokens before normal spacing decisions.  The macro
        // configuration determines whether a macro acts like an expression, a
        // statement, a declaration, a control construct, or a synthetic block
        // delimiter.  Function-like macros are identified by looking for an
        // immediate significant "(" after the macro token.
        bool tok_is_macro = is_macro_usage(tok);
        MacroClassification tok_macro_class;
        bool tok_macro_has_args = false;
        if (tok_is_macro) {
            if (const Tok* next = next_significant(i))
                tok_macro_has_args = tok_is(*next, "(", TokenKind::OpenParenthesis);
            tok_macro_class = classify_macro(tok, tok_macro_has_args, macro_classifier);
        }

        // Detect preprocessor conditionals in both lexer representations:
        // most directives arrive as a Directive token with directive_kind set,
        // but some preserved/passthrough text can contain the directive spelling
        // inside tok_text().  Normalize both cases to tok_pp_cond_kind so the
        // conditional-handling path below can treat them identically.
        syntax::SyntaxKind tok_pp_cond_kind = syntax::SyntaxKind::Unknown;
        if (is_line_directive(tok) && is_pp_conditional(tok_directive_kind(tok)))
            tok_pp_cond_kind = tok_directive_kind(tok);

        bool tok_is_pp_conditional = is_pp_conditional(tok_pp_cond_kind);

        if (whitespace_macro_passthrough) {
            // Some macros are explicitly marked whitespace-sensitive (for
            // example, tool-specific DSL macros).  Once such a macro starts, all
            // tokens through its balanced argument list are rendered verbatim.
            // We still count parentheses so we know where the protected region
            // ends, then restore normal macro/newline state.
            tok.fmt_passthrough = true;
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            if (!tok_whitespace(tok) && !tok_comment(tok)) {
                if (tok_is(tok, "(", TokenKind::OpenParenthesis)) {
                    ++whitespace_macro_paren_depth;
                    whitespace_macro_seen_open = true;
                } else if (tok_is(tok, ")", TokenKind::CloseParenthesis) &&
                           whitespace_macro_seen_open && whitespace_macro_paren_depth > 0) {
                    --whitespace_macro_paren_depth;
                    if (whitespace_macro_paren_depth == 0) {
                        whitespace_macro_passthrough = false;
                        whitespace_macro_seen_open = false;
                        if (macro_newline_after(whitespace_macro_class.role))
                            pending_nl = true;
                        prev_macro_role_valid = true;
                        prev_macro_role = whitespace_macro_class.role;
                        prev = &whitespace_macro_prev;
                        original_newline_before_token = false;
                    }
                }
            }
            continue;
        }

        if (tok_is_macro && tok_macro_has_args && tok_macro_class.whitespace_sensitive) {
            // Start a whitespace-sensitive function-like macro.  Its own token
            // receives normal indentation/newline metadata; following tokens are
            // marked passthrough until the matching close parenthesis.
            apply_newline(i);
            tok.fmt_indent = indent_level;
            at_bol = false;
            whitespace_macro_passthrough = true;
            whitespace_macro_seen_open = false;
            whitespace_macro_paren_depth = 0;
            whitespace_macro_class = tok_macro_class;
            whitespace_macro_prev = tok;
            prev_macro_role_valid = false;
            original_newline_before_token = false;
            continue;
        }

        if (tok_define_block(tok)) {
            if (tok_whitespace(tok)) {
                int cols = 0;
                bool saw_newline = false;
                for (char ch : tok_text(tok)) {
                    if (ch == '\n') {
                        saw_newline = true;
                        cols = 0;
                    } else if (ch == ' ' || ch == '\t') {
                        ++cols;
                    }
                }
                if (tok_text(tok).find('\n') != std::string::npos) {
                    pending_nl = true;
                    at_bol = true;
                }
                if (saw_newline)
                    define_spaces_pending = cols;
                else
                    define_spaces_pending += cols;
                original_newline_before_token = tok_text(tok).find('\n') != std::string::npos;
                continue;
            }
            apply_newline(i);
            tok.fmt_spaces_before = define_spaces_pending;
            define_spaces_pending = 0;
            tok.fmt_indent = 0;
            at_bol = false;
            prev = &tok;
            original_newline_before_token = false;
            continue;
        }

        // --- A. Disabled region ---
        if (in_disabled(tok_pos(tok), disabled)) {
            // verilog_format:off regions are preserved.  Tokens that already
            // carry source newlines (whitespace, preprocessor conditionals,
            // multi-token defines) are passed through exactly; otherwise a token
            // at the beginning of a disabled line can still receive current
            // indentation so the transition into the disabled block is stable.
            tok.fmt_disabled = true;
            apply_newline(i);
            if (is_line_directive(tok) && tok_directive_kind(tok) == syntax::SyntaxKind::DefineDirective)
                in_define_disabled = true;
            if (tok_is_pp_conditional || in_define_disabled || !at_bol || tok_whitespace(tok)) {
                tok.fmt_passthrough = true;
            } else {
                tok.fmt_indent = indent_level;
                at_bol = false;
            }
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            after_dis = !at_bol;
            continue;
        }
        in_define_disabled = false;

        // --- B. Whitespace tokens ---
        if (tok_whitespace(tok)) {
            // Whitespace tokens are not emitted directly unless passthrough is
            // active.  Convert their newline count into deferred layout state.
            // Original newlines inside parentheses are ignored here; pass0 and
            // later structural passes decide which paren contexts should wrap
            // (for example function calls and instantiation port lists).
            int nl = (int)std::count(tok_text(tok).begin(), tok_text(tok).end(), '\n');
            original_newline_before_token = nl > 0;
            if (after_dis && nl >= 1)
                pending_nl = true;
            after_dis = false;
            if (nl > 0 && paren_depth > 0)
                continue;
            if (nl > 1) {
                int extra = std::min(nl - 1, opts.blank_lines_between_items);
                blank_pend = std::max(blank_pend, extra);
            }
            continue;
        }

        if (tok_is_pp_conditional) {
            // Preprocessor conditionals are line-oriented and should remain
            // syntactically untouched.  The directive token itself is emitted as
            // passthrough; for split forms such as "`ifdef" followed by a
            // separate condition token, in_pp_cond preserves the next token too.
            apply_newline(i);
            if (!at_bol) {
                tok.fmt_newline_before = true;
                at_bol = true;
            }
            tok.fmt_passthrough = true;
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            pending_nl = false;
            blank_pend = 0;

            bool has_inline_arg = false;
            if (is_pp_cond_with(tok_pp_cond_kind)) {
                auto directive = directive_at_offset(tok_text(tok), 0);
                size_t arg = directive.end;
                while (arg < tok_text(tok).size() && (tok_text(tok)[arg] == ' ' || tok_text(tok)[arg] == '\t'))
                    ++arg;
                has_inline_arg = arg < tok_text(tok).size() && tok_text(tok)[arg] != '\n';
            }
            if (is_pp_cond_with(tok_pp_cond_kind) && !has_inline_arg) {
                in_pp_cond = true;
            } else {
                in_pp_cond = false;
                pending_nl = true;
            }
            prev_macro_role_valid = false;
            prev = &tok;
            original_newline_before_token = false;
            continue;
        }

        if (in_pp_cond) {
            // Preserve the argument/expression part of a split preprocessor
            // conditional and then force following SystemVerilog code onto a new
            // line.  A single separating space is added only if the directive and
            // condition share the rendered line.
            if (!at_bol) tok.fmt_spaces_before = 1;
            tok.fmt_passthrough = true;
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            in_pp_cond = false;
            pending_nl = true;
            prev_macro_role_valid = false;
            prev = &tok;
            original_newline_before_token = false;
            continue;
        }

        // --- C. Decide spacing and line-break ---
        // Compute local context for the pair (prev, tok), then delegate the
        // ordinary spacing/wrapping policy to spaces_req() and break_dec().
        // ParenKind is the important extra context here: the same raw tokens
        // "(" / ")" can mean ordinary expression grouping, a function call,
        // event control, instantiation parameter overrides, instantiation port
        // lists, or individual named connections inside those lists.  Special
        // constructs below can override the generic baseline decision.
        bool in_dim = dim_depth > 0;
        bool in_for_header =
            std::any_of(for_header_stack.begin(), for_header_stack.end(), [](bool v) { return v; });
        bool procedural_at = prev && ((tok_text(tok) == "@" && is_procedural_event_keyword(*prev)) ||
                                      (tok_text(*prev) == "@" && prev_at_procedural));
        ParenKind paren_kind = ParenKind::Ordinary;
        if (prev && (tok_text(*prev) == "(" || tok_is(tok, ")", TokenKind::CloseParenthesis))) {
            // For the first token after "(", and for the matching ")", use the
            // currently-open paren context.  This is how spacing/wrapping knows
            // whether it is inside an instantiation port list versus a normal
            // function call.
            if (!paren_stack.empty())
                paren_kind = paren_stack.back();
        } else if (prev && tok_is(tok, "(", TokenKind::OpenParenthesis))
            // For an opening "(", classify the paren before it is pushed below
            // so the pairwise spacing decision for "prev (" can use the new
            // context immediately.
            paren_kind = classify_paren_at(i, procedural_at);
        int spaces = 0;
        SD dec = SD::Undecided;
        if (prev) {
            spaces = spaces_req(*prev, tok, opts, in_dim, in_for_header, !paren_stack.empty(),
                                paren_kind, procedural_at);
            dec = break_dec(*prev, tok, opts, in_dim);
        }
        if (paren_kind == ParenKind::InstantiationPortList && prev &&
            tok_is(*prev, "(", TokenKind::OpenParenthesis) && !tok_comment(tok)) {
            // Start the first real port entry on a new line after the instance
            // port-list "(".  Inline comments immediately after "(" are
            // intentionally excluded so headers like "u_mem ( // comment" are
            // preserved on one line.
            //
            //   memory u_mem (        ->  memory u_mem (
            //       .a(a)                         .a(a)
            //
            // but:
            //
            //   memory u_mem ( // keep this comment on the header line
            dec = SD::MustWrap;
        }
        if (paren_kind == ParenKind::InstantiationPortList &&
            tok_is(tok, ")", TokenKind::CloseParenthesis)) {
            // Do not insert padding before the closing ")" of an instantiation
            // port list; the close line should render as ")" followed directly
            // by the semicolon spacing handled by the next token.
            //
            //   memory u_mem (
            //       .a(a)
            //   );
            //
            // not:
            //
            //   memory u_mem (
            //       .a(a)
            //    );
            dec = SD::MustWrap;
            spaces = 0;
        }
        if (prev && tok_is(tok, "[", TokenKind::OpenBracket) &&
            is_user_type_packed_dimension_open(i)) {
            spaces = 1;
        }
        if (prev_macro_role_valid && tok.kind == TokenKind::ElseKeyword &&
            prev_macro_role == MacroRole::BlockEndLike)
            dec = opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        if (tok_is_macro && macro_force_own_line(tok_macro_class.role)) {
            if (tok_macro_class.role == MacroRole::BlockBeginLike && prev &&
                prev->kind == TokenKind::CloseParenthesis) {
                dec = opts.statement.begin_newline ? SD::MustWrap : SD::MustAppend;
            } else if (!at_bol) {
                dec = SD::MustWrap;
            }
        }

        // --- D. Inline-comment suppression ---
        // If a line comment was originally inline, do not let a pending newline
        // from the previous token move it to its own line.  Standalone comments
        // are detected from the original input and forced to start a line.
        bool inline_comment = tok_comment(tok) && prev && !original_newline_before_token;

        do_while_tail = false;
        if (prev && prev->kind == TokenKind::EndKeyword && tok.kind == TokenKind::WhileKeyword) {
            if (do_depth > 0) {
                dec = SD::MustAppend;
                --do_depth;
                do_while_tail = true;
            } else {
                dec = SD::Undecided;
            }
        }
        if (macro_wrap_pending && !tok_whitespace(tok) && !tok_comment(tok)) {
            dec = SD::MustWrap;
            macro_wrap_pending = false;
        }
        if (function_macro_newline_candidate) {
            if (original_newline_before_token && function_macro_newline_depth == 0 &&
                paren_depth == 0 && !continues_after_function_like_macro(tok))
                dec = SD::MustWrap;
            function_macro_newline_candidate = false;
            function_macro_newline_depth = -1;
        }
        bool disable_target = prev && prev->kind == TokenKind::DisableKeyword;
        bool wait_fork_target = prev && prev->kind == TokenKind::WaitKeyword &&
                                tok.kind == TokenKind::ForkKeyword;

        // --- Block label ---
        if (block_label_state == 1 && !tok_whitespace(tok) && !tok_comment(tok)) {
            if (tok_text(tok) == ":") {
                dec = SD::MustAppend;
                spaces = 0;
                block_label_state = 2;
            } else {
                block_label_state = 0;
            }
        } else if (block_label_state == 2 && !tok_whitespace(tok) && !tok_comment(tok)) {
            dec = SD::MustAppend;
        }
        if (case_label_pending_nl && tok.kind == TokenKind::BeginKeyword)
            case_label_pending_nl = false;

        if (inline_comment && pending_nl) {
            if (!case_label_pending_nl)
                pending_nl = false;
        } else if (tok_comment(tok) && tok_text(tok).rfind("//", 0) == 0) {
            size_t line_start = input.rfind('\n', (size_t)tok_pos(tok));
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            bool standalone_comment = true;
            for (size_t p = line_start; p < (size_t)tok_pos(tok); ++p) {
                if (input[p] != ' ' && input[p] != '\t') {
                    standalone_comment = false;
                    break;
                }
            }
            if (standalone_comment)
                dec = SD::MustWrap;
        }

        // --- E. Emit newline / spacing based on break decision ---
        // dec chooses between three behaviors:
        //   MustWrap   attach an explicit newline to this token
        //   MustAppend consume any pending newline and keep this token inline
        //   Undecided  honor pending newlines, otherwise use normal spacing
        if (dec == SD::MustWrap) {
            pending_nl = false;
            tok.fmt_newline_before = true;
            tok.fmt_blank_lines = blank_pend;
            blank_pend = 0;
            at_bol = true;
        } else if (dec == SD::MustAppend) {
            if (pending_nl) {
                pending_nl = false;
                blank_pend = 0;
            }
            if (!at_bol && spaces > 0)
                tok.fmt_spaces_before = spaces;
        } else {
            apply_newline(i);
            if (!at_bol && spaces > 0)
                tok.fmt_spaces_before = spaces;
        }

        // --- F. Single-statement indent ---
        // Apply the one-token-late indent for control statements without begin.
        // If the next token is begin, the normal block opener handles indentation
        // instead.  Constraint braces are also allowed to become real scopes.
        if (single_stmt_pending && at_bol) {
            if (constraint_depth > 0 && tok_is(tok, "{", TokenKind::OpenBrace)) {
                // constraint body brace — let brace handler below create the scope
            } else if (tok.kind == TokenKind::BeginKeyword) {
                single_stmt_pending = false;
            } else {
                ++indent_level;
                single_stmt_pending = false;
                single_stmt_active = true;
            }
        }

        // --- G. Indent-close ---
        // Closing constructs reduce indent before the token's fmt_indent is set,
        // so "end", "endcase", block-end macros, and closing struct/constraint
        // braces align with their corresponding opener.
        if (tok_is_macro && tok_macro_class.role == MacroRole::BlockEndLike) {
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        } else if (is_indent_close(tok.kind)) {
            if (tok.kind == TokenKind::EndCaseKeyword) {
                case_depth = std::max(0, case_depth - 1);
                case_conditional_depth = 0;
            }
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        } else if (is_close_group(tok) && tok_is(tok, "}", TokenKind::CloseBrace) && !brace_stk.empty() &&
                   (brace_stk.back() == "struct" || brace_stk.back() == "constraint" || brace_stk.back() == "enum")) {
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        }

        // --- H. Set indent for the token ---
        // At this point all pre-token indentation adjustments are complete.
        // render_tokens() will multiply fmt_indent by opts.indent_size if this
        // token begins a line.
        tok.fmt_indent = indent_level;
        if (tok.fmt_newline_before && !paren_stack.empty() &&
            paren_stack.back() == ParenKind::InstantiationPortList &&
            !tok_is(tok, ")", TokenKind::CloseParenthesis)) {
            // Indent any token that starts a new rendered line inside an
            // instantiation port list as a port entry.  This covers both:
            //
            //   memory u_mem(
            //       .a(a)
            //
            // and:
            //
            //   memory u_mem( // header comment
            //       .a(a)
            tok.fmt_indent = indent_level + opts.instance.port_indent_level;
        }
        if (at_bol)
            at_bol = false;

        // --- J. Update bracket/paren/dim depth counters ---
        // Update grouping state after annotating the current token.  This makes
        // an opening token use the outer indentation/context, while following
        // tokens see the inner context.  paren_stack stores the classified
        // ParenKind for each open "(", including instantiation-specific kinds
        // needed by later comma and close-paren handling.
        if (tok_is(tok, "[", TokenKind::OpenBracket))
            ++dim_depth;
        else if (tok_is(tok, "]", TokenKind::CloseBracket) && dim_depth > 0)
            --dim_depth;
        else if (tok_is(tok, "(", TokenKind::OpenParenthesis)) {
            ++paren_depth;
            paren_stack.push_back(classify_paren_at(i, prev_at_procedural));
            for_header_stack.push_back(prev && (prev->kind == TokenKind::ForKeyword ||
                                                prev->kind == TokenKind::ForeachKeyword));
            if (!active_macros.empty() && active_macros.back().wait_open && prev &&
                is_macro_usage(*prev)) {
                active_macros.back().paren_depth = paren_depth;
                active_macros.back().wait_open = false;
            }
            if (case_expr_pending) {
                case_expr_depth = paren_depth;
                case_expr_pending = false;
            }
            if (control_expr_pending) {
                control_expr_depth = paren_depth;
                control_expr_pending = false;
            }
        } else if (tok_is(tok, ")", TokenKind::CloseParenthesis) && paren_depth > 0) {
            size_t closing_macro_index = active_macros.size();
            for (size_t mi = active_macros.size(); mi > 0; --mi) {
                const auto& macro = active_macros[mi - 1];
                if (!macro.wait_open && macro.paren_depth == paren_depth) {
                    closing_macro_index = mi - 1;
                    break;
                }
            }
            if (case_expr_depth == paren_depth) {
                pending_nl = true;
                case_expr_depth = -1;
            }
            if (control_expr_depth == paren_depth) {
                single_stmt_pending = true;
                pending_nl = true;
                control_expr_depth = -1;
            }
            --paren_depth;
            if (!paren_stack.empty())
                paren_stack.pop_back();
            if (!for_header_stack.empty())
                for_header_stack.pop_back();
            if (closing_macro_index < active_macros.size()) {
                MacroClassification classification =
                    active_macros[closing_macro_index].classification;
                active_macros.erase(active_macros.begin() + (ptrdiff_t)closing_macro_index);
                finish_macro_invocation(classification);
                if (classification.role == MacroRole::FunctionLikeExpr) {
                    function_macro_newline_candidate = true;
                    function_macro_newline_depth = paren_depth;
                }
            }
        } else if (tok_is(tok, ";", TokenKind::Semicolon))
            dim_depth = 0;

        // --- K. Post-emit housekeeping ---
        // Update structural state that affects subsequent tokens: macro
        // completion, keyword-opened scopes, statement termination, comments,
        // preprocessor continuations, and case-label bookkeeping.
        if (tok_is_macro) {
            if (tok_macro_has_args) {
                active_macros.push_back({tok_macro_class, true, -1});
            } else {
                finish_macro_invocation(tok_macro_class);
            }
        } else if (is_keyword(tok)) {
            if (tok.kind == TokenKind::DoKeyword)
                ++do_depth;

            if (tok.kind == TokenKind::ImportKeyword || tok.kind == TokenKind::ExportKeyword)
                in_import_export_decl = true;
            if (tok.kind == TokenKind::ExternKeyword)
                in_extern_decl = true;

            if (tok.kind == TokenKind::CaseKeyword || tok.kind == TokenKind::CaseXKeyword ||
                tok.kind == TokenKind::CaseZKeyword) {
                case_expr_pending = true;
                ++case_depth;
                case_conditional_depth = 0;
            }

            if (tok.kind == TokenKind::IfKeyword || tok.kind == TokenKind::ForKeyword ||
                tok.kind == TokenKind::ForeachKeyword ||
                (tok.kind == TokenKind::WhileKeyword && !do_while_tail) ||
                tok.kind == TokenKind::RepeatKeyword)
                control_expr_pending = true;

            if (tok.kind == TokenKind::ElseKeyword) {
                single_stmt_pending = true;
                pending_nl = true;
            }

            if (tok.kind == TokenKind::BeginKeyword)
                single_stmt_pending = false;

            if (is_constraint_keyword(tok))
                constraint_pend = true;

            bool import_export_function_or_task = in_import_export_decl &&
                                                  (tok.kind == TokenKind::FunctionKeyword ||
                                                   tok.kind == TokenKind::TaskKeyword);
            bool extern_function_or_task = in_extern_decl &&
                                           (tok.kind == TokenKind::FunctionKeyword ||
                                            tok.kind == TokenKind::TaskKeyword);
            bool typedef_class_forward_decl = tok.kind == TokenKind::ClassKeyword && prev &&
                                              prev->kind == TokenKind::TypedefKeyword;
            if (is_indent_open(tok.kind) && !disable_target && !wait_fork_target &&
                !import_export_function_or_task && !extern_function_or_task &&
                !typedef_class_forward_decl) {
                int delta = (tok.kind == TokenKind::ModuleKeyword ||
                             tok.kind == TokenKind::MacromoduleKeyword ||
                             tok.kind == TokenKind::InterfaceKeyword ||
                             tok.kind == TokenKind::PackageKeyword)
                                ? opts.default_indent_level_inside_outmost_block
                                : 1;
                indent_level += delta;
                indent_stack.push_back(delta);
                if (is_block_open(tok.kind) && tok.kind != TokenKind::CaseKeyword &&
                    tok.kind != TokenKind::CaseXKeyword && tok.kind != TokenKind::CaseZKeyword)
                    pending_nl = true;
            } else if (is_indent_close(tok.kind)) {
                pending_nl = true;
            } else if (tok.kind == TokenKind::StructKeyword || tok.kind == TokenKind::UnionKeyword) {
                struct_pend = true;
            } else if (tok.kind == TokenKind::EnumKeyword) {
                enum_pend = true;
            }
        } else if (is_open_group(tok) && tok_is(tok, "{", TokenKind::OpenBrace)) {
            if (struct_pend || enum_pend || constraint_pend || (constraint_depth > 0 && single_stmt_pending)) {
                bool is_constraint_brace = constraint_pend || (constraint_depth > 0 && single_stmt_pending);
                bool is_enum_brace = enum_pend && !is_constraint_brace;
                brace_stk.push_back(is_constraint_brace ? "constraint" : (is_enum_brace ? "enum" : "struct"));
                pending_nl = true;
                indent_level += 1;
                indent_stack.push_back(1);
                if (is_constraint_brace) {
                    ++constraint_depth;
                    constraint_pend = false;
                    single_stmt_pending = false;
                }
            } else {
                brace_stk.push_back("other");
            }
            struct_pend = false;
            enum_pend = false;
        } else if (is_close_group(tok) && tok_is(tok, "}", TokenKind::CloseBrace)) {
            if (!brace_stk.empty()) {
                if (brace_stk.back() == "constraint") {
                    constraint_depth = std::max(0, constraint_depth - 1);
                    pending_nl = true;
                } else if (brace_stk.back() == "enum") {
                    tok.fmt_newline_before = true;
                    tok.fmt_blank_lines = 0;
                }
                brace_stk.pop_back();
            }
        } else if (tok_is(tok, ";", TokenKind::Semicolon)) {
            in_import_export_decl = false;
            in_extern_decl = false;
            bool in_for_header_now =
                std::any_of(for_header_stack.begin(), for_header_stack.end(),
                            [](bool v) { return v; });
            if (paren_depth == 0 || (case_depth > 0 && !in_for_header_now))
                pending_nl = true;
            if (single_stmt_active) {
                indent_level = std::max(0, indent_level - 1);
                single_stmt_active = false;
            }
        } else if (tok_is(tok, ",", TokenKind::Comma) && prev &&
                   tok_is(*prev, ")", TokenKind::CloseParenthesis) &&
                   !paren_stack.empty() &&
                   paren_stack.back() == ParenKind::InstantiationPortList) {
            // A comma after a named instantiation port connection terminates the
            // current port item.  Schedule a newline for the following token.
            // If that following token is an inline comment, the inline-comment
            // suppression above keeps it attached to the comma line; otherwise
            // the next .port entry starts on a new line.
            //
            //   .a(a), .b(b)          ->  .a(a),
            //                              .b(b)
            //
            //   .a(a), // comment     ->  .a(a), // comment
            //   .b(b)                     .b(b)
            //
            //   .a(a),                ->  .a(a),
            //   // comment                // comment
            //   .b(b)                     .b(b)
            pending_nl = true;
        } else if (tok_is(tok, ",", TokenKind::Comma) && !brace_stk.empty() && brace_stk.back() == "enum") {
            pending_nl = true;
        } else if (is_line_directive(tok)) {
            pending_nl = true;
            if (is_pp_cond_bare(tok_directive_kind(tok)) || is_pp_cond_with(tok_directive_kind(tok)))
                in_pp_cond = false;
        } else if (tok_comment(tok)) {
            if (i + 1 < tokens.size() && tok_whitespace(tokens[i + 1]) &&
                tok_text(tokens[i + 1]).find('\n') != std::string::npos)
                pending_nl = true;
        } else if (tok.kind == TokenKind::Directive) {
            if (is_pp_cond_bare(tok_directive_kind(tok))) {
                pending_nl = true;
                in_pp_cond = false;
            } else if (is_pp_cond_with(tok_directive_kind(tok))) {
                in_pp_cond = true;
            }
        } else if (is_identifier(tok)) {
            if (in_pp_cond) {
                pending_nl = true;
                in_pp_cond = false;
            }
            if (is_macro_usage(tok) && tok_text(tok).find(';') != std::string::npos) {
                pending_nl = true;
                if (single_stmt_active) {
                    indent_level = std::max(0, indent_level - 1);
                    single_stmt_active = false;
                }
            }
        } else if (in_pp_cond) {
            pending_nl = true;
            in_pp_cond = false;
        }

        // Within case items, ':' can mean either a case-label separator or the
        // false branch of a ternary expression.  Track '?' nesting so only label
        // colons suppress the pending newline before the item body.
        if (tok_is(tok, "?", TokenKind::Question) && case_depth > 0 && dim_depth == 0)
            ++case_conditional_depth;
        else if (tok_is(tok, ":", TokenKind::Colon) && case_depth > 0 && dim_depth == 0) {
            if (case_conditional_depth > 0) {
                --case_conditional_depth;
            } else if (block_label_state == 0) {
                pending_nl = false;
                case_label_pending_nl = true;
            }
        } else if (tok_is(tok, ";", TokenKind::Semicolon) || tok.kind == TokenKind::EndCaseKeyword) {
            case_label_pending_nl = false;
            case_conditional_depth = 0;
        } else if (!tok_comment(tok) && !tok_whitespace(tok)) {
            case_label_pending_nl = false;
        }

        // --- Block label post-emit ---
        // After seeing begin/fork/end-style tokens, arm the label detector for
        // an optional ": name" sequence.  Once a label name is consumed, schedule
        // the next token on a new line.
        if (block_label_state == 2 && !tok_whitespace(tok) && !tok_comment(tok) && tok_text(tok) != ":") {
            pending_nl = true;
            block_label_state = 0;
        }
        if (!tok_whitespace(tok) && !tok_comment(tok) && is_keyword(tok) &&
            (tok.kind == TokenKind::BeginKeyword || tok.kind == TokenKind::ForkKeyword ||
             is_indent_close(tok.kind)) &&
            !disable_target && !wait_fork_target) {
            block_label_state = 1;
        }

        // Carry pairwise context into the next iteration.  Whitespace tokens do
        // not normally reach this point, so prev is the last significant token
        // seen by spacing and wrapping decisions.
        prev_macro_role_valid = tok_is_macro;
        if (tok_is_macro)
            prev_macro_role = tok_macro_class.role;
        prev_at_procedural = tok_text(tok) == "@" && prev && is_procedural_event_keyword(*prev);
        prev = &tok;
        original_newline_before_token = false;
    } // end pass0 main token loop
}


static void align_assign_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    align_assign_pass_v2(tokens, opts);
}

static void align_var_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    align_var_pass_v2(tokens, opts);
}

static void align_port_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    struct PortNameSlot {
        size_t name{SIZE_MAX};
        size_t trail_start{SIZE_MAX};
        size_t trail_end{SIZE_MAX};
        size_t terminator{SIZE_MAX};
        std::string name_text;
        std::string trail_text;
    };
    struct PortLineParsedTok {
        bool valid{false};
        size_t line_start{SIZE_MAX};
        size_t line_end{SIZE_MAX};
        size_t direction{SIZE_MAX};
        size_t dtype_start{SIZE_MAX};
        size_t dtype_end{SIZE_MAX};
        size_t qualifier{SIZE_MAX};
        size_t dim_start{SIZE_MAX};
        size_t dim_end{SIZE_MAX};
        std::string direction_text;
        std::string dtype_text;
        std::string qualifier_text;
        std::string dim_text;
        std::vector<PortNameSlot> names;
    };

    auto line_first_sig = [&](size_t s, size_t e) -> size_t {
        return next_code_sig(tokens, s, e);
    };
    auto line_has_kind = [&](size_t s, size_t e, TokenKind kind) {
        for (size_t k = s; k < e && k < tokens.size(); ++k)
            if (!tok_whitespace(tokens[k]) && !tok_comment(tokens[k]) && !tok_directive(tokens[k]) &&
                tokens[k].kind == kind)
                return true;
        return false;
    };
    auto line_has_header_end = [&](size_t s, size_t e) {
        bool saw_close = false;
        for (size_t k = s; k < e && k < tokens.size(); ++k) {
            if (tok_whitespace(tokens[k]) || tok_comment(tokens[k]) || tok_directive(tokens[k]))
                continue;
            if (tok_is(tokens[k], ")", TokenKind::CloseParenthesis))
                saw_close = true;
            else if (saw_close && tok_is(tokens[k], ";", TokenKind::Semicolon))
                return true;
        }
        return false;
    };
    auto is_header_start_line = [&](size_t s, size_t e) {
        size_t first = line_first_sig(s, e);
        if (first == SIZE_MAX)
            return false;
        bool starts_header = tokens[first].kind == TokenKind::ModuleKeyword ||
                             tokens[first].kind == TokenKind::MacromoduleKeyword ||
                             tokens[first].kind == TokenKind::InterfaceKeyword ||
                             tokens[first].kind == TokenKind::ProgramKeyword ||
                             tokens[first].kind == TokenKind::TaskKeyword ||
                             tokens[first].kind == TokenKind::FunctionKeyword;
        if (!starts_header || line_has_header_end(s, e))
            return false;
        return line_has_kind(s, e, TokenKind::OpenParenthesis) ||
               !line_has_kind(s, e, TokenKind::Semicolon) ||
               line_has_kind(s, e, TokenKind::ImportKeyword);
    };
    auto is_comment_or_blank_line = [&](size_t s, size_t e) {
        bool saw_comment = false;
        for (size_t k = s; k < e && k < tokens.size(); ++k) {
            if (tok_whitespace(tokens[k]))
                continue;
            if (tok_comment(tokens[k])) {
                saw_comment = true;
                continue;
            }
            return false;
        }
        return saw_comment || line_first_sig(s, e) == SIZE_MAX;
    };
    auto is_semi_only_line = [&](size_t s, size_t e) {
        size_t first = line_first_sig(s, e);
        if (first == SIZE_MAX || !tok_is(tokens[first], ";", TokenKind::Semicolon))
            return false;
        return next_code_sig(tokens, first + 1, e) == SIZE_MAX;
    };
    auto is_port_decl_line = [&](size_t s, size_t e) {
        size_t first = line_first_sig(s, e);
        return first != SIZE_MAX && is_port_direction_token(tokens[first]);
    };
    auto pure_type_id = [&](size_t idx) {
        return is_identifier(tokens[idx]) && !is_keyword(tokens[idx]);
    };
    auto parse_line = [&](size_t s, size_t e, size_t extra_semi) {
        PortLineParsedTok r;
        r.line_start = s;
        r.line_end = e;
        std::vector<size_t> sigs;
        for (size_t k = s; k < e && k < tokens.size(); ++k) {
            if (tok_whitespace(tokens[k]) || tok_comment(tokens[k]) || tok_directive(tokens[k]))
                continue;
            sigs.push_back(k);
        }
        if (extra_semi != SIZE_MAX)
            sigs.push_back(extra_semi);
        if (sigs.empty() || !is_port_direction_token(tokens[sigs[0]]))
            return r;
        r.direction = sigs[0];
        r.direction_text = tok_text(tokens[r.direction]);

        size_t term = SIZE_MAX;
        if (!sigs.empty() && (tok_is(tokens[sigs.back()], ";", TokenKind::Semicolon) ||
                              tok_is(tokens[sigs.back()], ",", TokenKind::Comma))) {
            term = sigs.back();
            sigs.pop_back();
        }
        if (sigs.size() < 2)
            return r;

        size_t si = 1;
        if (si < sigs.size()) {
            bool builtin = is_var_builtin_type_token(tokens[sigs[si]]) ||
                           is_port_net_type_text(tok_text(tokens[sigs[si]]));
            bool usertype = pure_type_id(sigs[si]) && si + 1 < sigs.size() &&
                            !tok_is(tokens[sigs[si + 1]], ",", TokenKind::Comma);
            if ((builtin || usertype) && !is_sign_qualifier_token(tokens[sigs[si]]) &&
                !tok_is(tokens[sigs[si]], "[", TokenKind::OpenBracket)) {
                r.dtype_start = sigs[si++];
                if (is_port_net_type_text(tok_text(tokens[r.dtype_start])) && si < sigs.size() &&
                    is_port_data_type_text(tok_text(tokens[sigs[si]])))
                    ++si;
                while (si + 1 < sigs.size() && tok_text(tokens[sigs[si]]) == "::")
                    si += 2;
                r.dtype_end = si < sigs.size() ? sigs[si] : sigs.back() + 1;
                r.dtype_text = token_join_compact(tokens, r.dtype_start, r.dtype_end);
            }
        }
        if (si < sigs.size() && is_sign_qualifier_token(tokens[sigs[si]])) {
            r.qualifier = sigs[si++];
            r.qualifier_text = tok_text(tokens[r.qualifier]);
        }
        if (si < sigs.size() && tok_is(tokens[sigs[si]], "[", TokenKind::OpenBracket)) {
            r.dim_start = sigs[si];
            int depth = 0;
            while (si < sigs.size()) {
                if (tok_is(tokens[sigs[si]], "[", TokenKind::OpenBracket))
                    ++depth;
                else if (tok_is(tokens[sigs[si]], "]", TokenKind::CloseBracket))
                    --depth;
                ++si;
                if (depth <= 0)
                    break;
            }
            r.dim_end = si < sigs.size() ? sigs[si] : sigs.back() + 1;
            r.dim_text = normalize_bracket_spacing(token_join_compact(tokens, r.dim_start, r.dim_end), opts);
        }
        if (si >= sigs.size())
            return r;

        size_t names_first = sigs[si];
        size_t names_last = sigs.back() + 1;
        std::vector<std::pair<size_t, size_t>> ranges;
        size_t range_start = names_first;
        int paren = 0, bracket = 0, brace = 0;
        for (size_t k = names_first; k < names_last && k < tokens.size(); ++k) {
            if (tok_whitespace(tokens[k]) || tok_comment(tokens[k]) || tok_directive(tokens[k]))
                continue;
            if (tok_is(tokens[k], "(", TokenKind::OpenParenthesis)) ++paren;
            else if (tok_is(tokens[k], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
            else if (tok_is(tokens[k], "[", TokenKind::OpenBracket)) ++bracket;
            else if (tok_is(tokens[k], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
            else if (tok_is(tokens[k], "{", TokenKind::OpenBrace) || tokens[k].kind == TokenKind::ApostropheOpenBrace) ++brace;
            else if (tok_is(tokens[k], "}", TokenKind::CloseBrace) && brace > 0) --brace;
            else if (tok_is(tokens[k], ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
                size_t end = prev_code_sig(tokens, range_start, k);
                if (end != SIZE_MAX && end >= range_start)
                    ranges.push_back({range_start, end + 1});
                range_start = next_code_sig(tokens, k + 1, names_last);
                if (range_start == SIZE_MAX)
                    break;
            }
        }
        if (range_start != SIZE_MAX) {
            size_t end = prev_code_sig(tokens, range_start, names_last);
            if (end != SIZE_MAX && end >= range_start)
                ranges.push_back({range_start, end + 1});
        }
        if (ranges.empty())
            return r;
        for (size_t ri = 0; ri < ranges.size(); ++ri) {
            auto [first, end] = ranges[ri];
            PortNameSlot slot;
            slot.name = first;
            slot.name_text = tok_text(tokens[first]);
            size_t trail = next_code_sig(tokens, first + 1, end);
            if (trail != SIZE_MAX) {
                slot.trail_start = trail;
                slot.trail_end = end;
                slot.trail_text = normalize_bracket_spacing(token_join_compact(tokens, trail, end), opts);
            }
            if (ri + 1 < ranges.size()) {
                for (size_t k = end; k < ranges[ri + 1].first && k < tokens.size(); ++k) {
                    if (tok_whitespace(tokens[k]) || tok_comment(tokens[k]) || tok_directive(tokens[k]))
                        continue;
                    if (tok_is(tokens[k], ",", TokenKind::Comma)) {
                        slot.terminator = k;
                        break;
                    }
                }
            } else {
                slot.terminator = term;
            }
            r.names.push_back(std::move(slot));
        }
        r.valid = !r.names.empty();
        return r;
    };

    auto starts = tok_line_starts(tokens);
    size_t li = 0;
    bool module_header_region = false;
    while (li < starts.size()) {
        size_t s = starts[li], e = (li + 1 < starts.size()) ? starts[li + 1] : tokens.size();
        if (token_range_has_pp_conditional(tokens, s, e) || token_range_disabled_or_passthrough(tokens, s, e)) {
            ++li;
            continue;
        }
        if (module_header_region) {
            if (line_has_header_end(s, e))
                module_header_region = false;
            ++li;
            continue;
        }
        if (is_header_start_line(s, e)) {
            module_header_region = true;
            ++li;
            continue;
        }
        if (!is_port_decl_line(s, e)) {
            ++li;
            continue;
        }

        std::vector<PortLineParsedTok> blk;
        size_t j = li;
        while (j < starts.size()) {
            size_t bs = starts[j], be = (j + 1 < starts.size()) ? starts[j + 1] : tokens.size();
            if (token_range_has_pp_conditional(tokens, bs, be) || token_range_disabled_or_passthrough(tokens, bs, be))
                break;
            if (!is_port_decl_line(bs, be)) {
                if (is_comment_or_blank_line(bs, be)) {
                    ++j;
                    continue;
                }
                break;
            }
            size_t extra_semi = SIZE_MAX;
            if (j + 1 < starts.size()) {
                size_t ns = starts[j + 1], ne = (j + 2 < starts.size()) ? starts[j + 2] : tokens.size();
                if (is_semi_only_line(ns, ne)) {
                    extra_semi = line_first_sig(ns, ne);
                    ++j;
                }
            }
            auto parsed = parse_line(bs, be, extra_semi);
            if (parsed.valid)
                blk.push_back(std::move(parsed));
            ++j;
        }

        int md = 0, ms2_content = 0, mdim = 0;
        bool has_qualified_type = false;
        size_t max_slots = 0;
        for (const auto& p : blk) {
            md = std::max(md, (int)p.direction_text.size());
            std::string s2 = p.dtype_text + (p.qualifier_text.empty() ? "" : " " + p.qualifier_text);
            ms2_content = std::max(ms2_content, (int)s2.size());
            has_qualified_type = has_qualified_type || s2.find("::") != std::string::npos;
            mdim = std::max(mdim, (int)p.dim_text.size());
            max_slots = std::max(max_slots, p.names.size());
        }
        if (blk.empty()) {
            li = std::max(li + 1, j);
            continue;
        }

        const auto& pd = opts.port_declaration;
        int pd_s1_min = tab_aligned_width(pd.section1_min_width, opts);
        int pd_s2_min = tab_aligned_width(pd.section2_min_width, opts);
        int pd_s3_min = tab_aligned_width(pd.section3_min_width, opts);
        int pd_s4_min = tab_aligned_width(pd.section4_min_width, opts);
        int pd_s5_min = tab_aligned_width(pd.section5_min_width, opts);
        int s1w = tab_aligned_width(std::max(pd_s1_min, md + 1), opts);
        int s2w = ms2_content > 0 ? tab_aligned_width(std::max(pd_s2_min, ms2_content + (has_qualified_type ? 3 : 1)), opts) : 0;
        int s3w = mdim > 0 ? tab_aligned_width(std::max(pd_s3_min, mdim + 1), opts) : 0;
        std::vector<int> id_widths(max_slots, 0), trail_widths(max_slots, 0);
        for (size_t slot = 0; slot < max_slots; ++slot) {
            int max_id = 0, max_tr = 0;
            for (const auto& p : blk) {
                if (slot < p.names.size()) {
                    max_id = std::max(max_id, (int)p.names[slot].name_text.size());
                    max_tr = std::max(max_tr, (int)p.names[slot].trail_text.size());
                }
            }
            id_widths[slot] = tab_aligned_width(std::max(pd_s4_min, max_id + 1), opts);
            trail_widths[slot] = tab_aligned_width(std::max(pd_s5_min, max_tr), opts);
        }

        std::vector<std::vector<int>> adaptive_id_widths;
        if (pd.align_adaptive) {
            adaptive_id_widths.resize(blk.size());
            std::vector<int> cur_start(blk.size(), 0);
            for (size_t bi = 0; bi < blk.size(); ++bi) {
                const auto& p = blk[bi];
                std::string tp = p.dtype_text + (p.qualifier_text.empty() ? "" : " " + p.qualifier_text);
                int t1 = pd_s1_min;
                int t2 = t1 + (s2w > 0 ? pd_s2_min : 0);
                int t3 = t2 + (s3w > 0 ? pd_s3_min : 0);
                int e1 = tab_aligned_width(std::max(t1, (int)p.direction_text.size() + 1), opts);
                int e2 = e1;
                if (s2w > 0) {
                    int c2 = tp.empty() ? 0 : (int)tp.size() + 1;
                    e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                }
                int e3 = e2;
                if (s3w > 0) {
                    int c3 = p.dim_text.empty() ? 0 : (int)p.dim_text.size() + 1;
                    e3 = tab_aligned_width(std::max(t3, e2 + c3), opts);
                }
                cur_start[bi] = e3;
            }
            for (size_t slot = 0; slot < max_slots; ++slot) {
                int global_end = 0;
                for (size_t bi = 0; bi < blk.size(); ++bi) {
                    if (slot >= blk[bi].names.size())
                        continue;
                    int nw = tab_aligned_width(std::max(pd_s4_min, (int)blk[bi].names[slot].name_text.size() + 1), opts);
                    global_end = std::max(global_end, cur_start[bi] + nw);
                }
                for (size_t bi = 0; bi < blk.size(); ++bi) {
                    if (slot >= blk[bi].names.size()) {
                        adaptive_id_widths[bi].push_back(0);
                        continue;
                    }
                    int nw = tab_aligned_width(std::max(pd_s4_min, (int)blk[bi].names[slot].name_text.size() + 1), opts);
                    int aw = global_end > cur_start[bi] ? std::max(nw, global_end - cur_start[bi]) : nw;
                    adaptive_id_widths[bi].push_back(aw);
                    cur_start[bi] += aw + trail_widths[slot] + 2;
                }
            }
        }

        for (size_t bi = 0; bi < blk.size(); ++bi) {
            auto& p = blk[bi];
            int line_s1 = s1w, line_s2 = s2w, line_s3 = s3w;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;
            std::string type_part = p.dtype_text + (p.qualifier_text.empty() ? "" : " " + p.qualifier_text);
            if (pd.align_adaptive) {
                int t1 = pd_s1_min;
                int t2 = t1 + (s2w > 0 ? pd_s2_min : 0);
                int t3 = t2 + (s3w > 0 ? pd_s3_min : 0);
                int e1 = tab_aligned_width(std::max(t1, (int)p.direction_text.size() + 1), opts);
                line_s1 = e1;
                int e2 = e1;
                if (s2w > 0) {
                    int c2 = type_part.empty() ? 0 : (int)type_part.size() + 1;
                    e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                    line_s2 = e2 - e1;
                }
                if (s3w > 0) {
                    int c3 = p.dim_text.empty() ? 0 : (int)p.dim_text.size() + 1;
                    int e3 = tab_aligned_width(std::max(t3, e2 + c3), opts);
                    line_s3 = e3 - e2;
                }
                if (bi < adaptive_id_widths.size())
                    line_id_widths = adaptive_id_widths[bi];
            }

            if (p.dtype_start != SIZE_MAX || p.qualifier != SIZE_MAX) {
                size_t type_first = p.dtype_start != SIZE_MAX ? p.dtype_start : p.qualifier;
                tokens[type_first].fmt_spaces_before = std::max(1, line_s1 - (int)p.direction_text.size());
                if (p.dim_start != SIZE_MAX) {
                    tokens[p.dim_start].fmt_spaces_before = std::max(1, line_s2 - (int)type_part.size());
                    tokens[p.names[0].name].fmt_spaces_before = std::max(1, line_s3 - (int)p.dim_text.size());
                } else {
                    tokens[p.names[0].name].fmt_spaces_before = std::max(1, line_s2 - (int)type_part.size()) + (line_s3 > 0 ? line_s3 : 0);
                }
            } else if (p.dim_start != SIZE_MAX) {
                tokens[p.dim_start].fmt_spaces_before = std::max(1, line_s1 - (int)p.direction_text.size()) + (line_s2 > 0 ? line_s2 : 0);
                tokens[p.names[0].name].fmt_spaces_before = std::max(1, line_s3 - (int)p.dim_text.size());
            } else {
                tokens[p.names[0].name].fmt_spaces_before = std::max(1, line_s1 - (int)p.direction_text.size()) +
                                                            (line_s2 > 0 ? line_s2 : 0) + (line_s3 > 0 ? line_s3 : 0);
            }

            for (size_t slot = 0; slot < p.names.size(); ++slot) {
                const auto& nm = p.names[slot];
                if (slot > 0)
                    tokens[nm.name].fmt_spaces_before = 1;
                if (nm.trail_start != SIZE_MAX) {
                    int idw = slot < line_id_widths.size() ? line_id_widths[slot] : (int)nm.name_text.size() + 1;
                    tokens[nm.trail_start].fmt_spaces_before = std::max(0, idw - (int)nm.name_text.size());
                }
                if (nm.terminator != SIZE_MAX) {
                    int idw = slot < line_id_widths.size() ? line_id_widths[slot] : (int)nm.name_text.size() + 1;
                    int trailw = slot < line_trail_widths.size() ? line_trail_widths[slot] : 0;
                    int pad = nm.trail_text.empty()
                                  ? std::max(0, idw - (int)nm.name_text.size() + trailw)
                                  : std::max(0, trailw - (int)nm.trail_text.size());
                    tokens[nm.terminator].fmt_newline_before = false;
                    tokens[nm.terminator].fmt_spaces_before = pad;
                }
            }
        }
        li = j;
    }
}

static size_t matching_close_token(const std::vector<Tok>& tokens, size_t open_idx,
                                   size_t end_limit, const std::string& open_text,
                                   const std::string& close_text, TokenKind open_kind,
                                   TokenKind close_kind) {
    int depth = 0;
    for (size_t i = open_idx; i < end_limit && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], open_text, open_kind))
            ++depth;
        else if (tok_is(tokens[i], close_text, close_kind)) {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return SIZE_MAX;
}

static std::vector<std::pair<size_t, size_t>> top_level_ranges_between(
    const std::vector<Tok>& tokens, size_t first, size_t last, size_t* last_comma = nullptr) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t start = next_code_sig(tokens, first, last);
    int paren = 0, bracket = 0, brace = 0;
    for (size_t i = first; i < last && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis)) ++paren;
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
        else if (tok_is(tokens[i], "[", TokenKind::OpenBracket)) ++bracket;
        else if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) || tokens[i].kind == TokenKind::ApostropheOpenBrace) ++brace;
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && brace > 0) --brace;
        else if (tok_is(tokens[i], ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
            size_t end = prev_code_sig(tokens, start, i);
            if (start != SIZE_MAX && end != SIZE_MAX && end < i)
                ranges.push_back({start, i});
            if (last_comma)
                *last_comma = i;
            start = next_code_sig(tokens, i + 1, last);
        }
    }
    if (start != SIZE_MAX) {
        size_t end = prev_code_sig(tokens, start, last);
        if (end != SIZE_MAX && end >= start)
            ranges.push_back({start, end + 1});
    }
    return ranges;
}

static size_t find_top_level_token(const std::vector<Tok>& tokens, size_t first, size_t last,
                                   const std::string& text, TokenKind kind) {
    int paren = 0, bracket = 0, brace = 0;
    for (size_t i = first; i < last && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], text, kind) && paren == 0 && bracket == 0 && brace == 0)
            return i;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis)) ++paren;
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
        else if (tok_is(tokens[i], "[", TokenKind::OpenBracket)) ++bracket;
        else if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) || tokens[i].kind == TokenKind::ApostropheOpenBrace) ++brace;
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && brace > 0) --brace;
    }
    return SIZE_MAX;
}

static int token_range_width_compact(const std::vector<Tok>& tokens, size_t first, size_t end) {
    return (int)token_join_compact(tokens, first, end).size();
}

static size_t statement_end_semicolon(const std::vector<Tok>& tokens, size_t start) {
    int paren = 0, bracket = 0, brace = 0;
    for (size_t i = start; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis)) ++paren;
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
        else if (tok_is(tokens[i], "[", TokenKind::OpenBracket)) ++bracket;
        else if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) || tokens[i].kind == TokenKind::ApostropheOpenBrace) ++brace;
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && brace > 0) --brace;
        else if (tok_is(tokens[i], ";", TokenKind::Semicolon) && paren == 0 && bracket == 0 && brace == 0)
            return i;
    }
    return SIZE_MAX;
}

static void format_enum_declaration_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    const auto& eo = opts.enum_declaration;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].fmt_disabled || tokens[i].fmt_passthrough || tokens[i].kind != TokenKind::TypedefKeyword)
            continue;
        size_t semi = statement_end_semicolon(tokens, i);
        if (semi == SIZE_MAX || token_range_has_pp_conditional(tokens, i, semi + 1) ||
            token_range_disabled_or_passthrough(tokens, i, semi + 1))
            continue;
        size_t enum_i = SIZE_MAX, open = SIZE_MAX;
        for (size_t j = i; j <= semi; ++j) {
            if (tokens[j].kind == TokenKind::EnumKeyword)
                enum_i = j;
            if (enum_i != SIZE_MAX && tok_is(tokens[j], "{", TokenKind::OpenBrace)) { open = j; break; }
        }
        if (enum_i == SIZE_MAX || open == SIZE_MAX)
            continue;
        size_t close = matching_close_token(tokens, open, semi + 1, "{", "}", TokenKind::OpenBrace, TokenKind::CloseBrace);
        if (close == SIZE_MAX || close >= semi)
            continue;
        auto items = top_level_ranges_between(tokens, open + 1, close);
        if (items.empty())
            continue;
        int base = tokens[i].fmt_indent;
        int item_base_col = (base + 1) * std::max(0, opts.indent_size);
        int name_width = tab_aligned_width(eo.enum_name_min_width, opts);
        int value_width = tab_aligned_width(eo.enum_value_min_width, opts);
        std::vector<size_t> eqs(items.size(), SIZE_MAX);
        bool sp_before = binary_space_before(opts.spacing.assignment_operator_spacing);
        bool sp_after = binary_space_after(opts.spacing.assignment_operator_spacing);
        int eq_gap = (sp_before ? 1 : 0) + 1 + (sp_after ? 1 : 0); // spaces_before_eq + '=' + spaces_after_val
        if (eo.align && !eo.align_adaptive) {
            for (size_t k = 0; k < items.size(); ++k) {
                eqs[k] = find_top_level_token(tokens, items[k].first, items[k].second, "=", TokenKind::Unknown);
                size_t name_end = eqs[k] == SIZE_MAX ? items[k].second : eqs[k];
                name_width = std::max(name_width, token_range_width_compact(tokens, items[k].first, name_end) + (sp_before ? 1 : 0));
                if (eqs[k] != SIZE_MAX)
                    value_width = std::max(value_width, token_range_width_compact(tokens, eqs[k] + 1, items[k].second));
            }
            if (opts.tab_align) {
                name_width = snap_to_indent_grid(item_base_col + name_width, opts.indent_size) - item_base_col;
                value_width = snap_to_indent_grid(item_base_col + name_width + eq_gap + value_width, opts.indent_size) - item_base_col - name_width - eq_gap;
            }
        }
        tokens[open].fmt_newline_before = false;
        tokens[open].fmt_spaces_before = 1;
        for (size_t k = 0; k < items.size(); ++k) {
            size_t first = items[k].first;
            size_t eq = eqs[k] == SIZE_MAX ? find_top_level_token(tokens, items[k].first, items[k].second, "=", TokenKind::Unknown) : eqs[k];
            tokens[first].fmt_newline_before = true;
            tokens[first].fmt_blank_lines = 0;
            tokens[first].fmt_indent = base + 1;
            tokens[first].fmt_spaces_before = 0;
            if (eo.align && eq != SIZE_MAX) {
                int nw = token_range_width_compact(tokens, first, eq);
                int cur_name_width = name_width;
                if (eo.align_adaptive) {
                    cur_name_width = std::max(tab_aligned_width(eo.enum_name_min_width, opts), nw) + (sp_before ? 1 : 0);
                    if (opts.tab_align)
                        cur_name_width = snap_to_indent_grid(item_base_col + cur_name_width, opts.indent_size) - item_base_col;
                }
                tokens[eq].fmt_spaces_before = std::max(sp_before ? 1 : 0, cur_name_width - nw);
                size_t val = next_code_sig(tokens, eq + 1, items[k].second);
                if (val != SIZE_MAX)
                    tokens[val].fmt_spaces_before = sp_after ? 1 : 0;
            } else if (eq != SIZE_MAX) {
                tokens[eq].fmt_spaces_before = sp_before ? 1 : 0;
                size_t val = next_code_sig(tokens, eq + 1, items[k].second);
                if (val != SIZE_MAX)
                    tokens[val].fmt_spaces_before = sp_after ? 1 : 0;
            }
            size_t comma = find_top_level_token(tokens, items[k].second, close, ",", TokenKind::Comma);
            if (comma != SIZE_MAX) {
                if (eo.align && eo.align_adaptive) {
                    if (eq != SIZE_MAX) {
                        size_t val = next_code_sig(tokens, eq + 1, items[k].second);
                        int vw = val != SIZE_MAX ? token_range_width_compact(tokens, val, items[k].second) : 0;
                        int cvw = std::max(tab_aligned_width(eo.enum_value_min_width, opts), vw);
                        tokens[comma].fmt_spaces_before = std::max(0, cvw - vw);
                    } else {
                        int nw_full = token_range_width_compact(tokens, first, items[k].second);
                        int cnw = std::max(tab_aligned_width(eo.enum_name_min_width, opts), nw_full) + (sp_before ? 1 : 0);
                        if (opts.tab_align)
                            cnw = snap_to_indent_grid(item_base_col + cnw, opts.indent_size) - item_base_col;
                        int cvw = tab_aligned_width(eo.enum_value_min_width, opts);
                        tokens[comma].fmt_spaces_before = std::max(0, cnw - nw_full + 1 + (sp_after ? 1 : 0) + cvw);
                    }
                } else if (eo.align) {
                    if (eq != SIZE_MAX) {
                        size_t val = next_code_sig(tokens, eq + 1, items[k].second);
                        int vw = val != SIZE_MAX ? token_range_width_compact(tokens, val, items[k].second) : 0;
                        tokens[comma].fmt_spaces_before = std::max(0, value_width - vw);
                    } else {
                        int nw_full = token_range_width_compact(tokens, first, items[k].second);
                        tokens[comma].fmt_spaces_before = std::max(0, name_width - nw_full +
                            (value_width > 0 ? 1 + (sp_after ? 1 : 0) + value_width : 0));
                    }
                } else {
                    tokens[comma].fmt_spaces_before = 0;
                }
            }
        }
        tokens[close].fmt_newline_before = true;
        tokens[close].fmt_blank_lines = 0;
        tokens[close].fmt_indent = base;
        tokens[close].fmt_spaces_before = 0;
        size_t suffix = next_code_sig(tokens, close + 1, semi);
        if (suffix != SIZE_MAX)
            tokens[suffix].fmt_spaces_before = 1;
        tokens[semi].fmt_spaces_before = 0;
        i = semi;
    }
}

static bool is_modport_dir_kind(TokenKind kind) {
    return kind == TokenKind::InputKeyword || kind == TokenKind::OutputKeyword ||
           kind == TokenKind::InOutKeyword || kind == TokenKind::RefKeyword ||
           kind == TokenKind::ImportKeyword || kind == TokenKind::ExportKeyword;
}

static void format_modport_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    const auto& mo = opts.modport;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].fmt_disabled || tokens[i].fmt_passthrough || tokens[i].kind != TokenKind::ModPortKeyword)
            continue;
        size_t semi = statement_end_semicolon(tokens, i);
        if (semi == SIZE_MAX || token_range_has_pp_conditional(tokens, i, semi + 1) ||
            token_range_disabled_or_passthrough(tokens, i, semi + 1))
            continue;
        auto modports = top_level_ranges_between(tokens, next_code_sig(tokens, i + 1, semi), semi);
        int base = tokens[i].fmt_indent;
        for (auto mp : modports) {
            size_t name = mp.first;
            size_t open = SIZE_MAX;
            for (size_t j = name + 1; j < mp.second; ++j)
                if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) { open = j; break; }
            if (open == SIZE_MAX)
                continue;
            size_t close = matching_close_paren(tokens, open, mp.second);
            if (close == SIZE_MAX)
                continue;
            auto items = top_level_ranges_between(tokens, open + 1, close);
            if (items.empty())
                continue;
            int dir_width = tab_aligned_width(mo.direction_min_width, opts);
            int sig_width = tab_aligned_width(mo.signal_min_width, opts);
            for (auto item : items) {
                if (!is_modport_dir_kind(tokens[item.first].kind))
                    continue;
                dir_width = std::max(dir_width, (int)tok_text(tokens[item.first]).size() + 1);
                if (mo.align && !mo.align_adaptive)
                    sig_width = std::max(sig_width, token_range_width_compact(tokens, item.first + 1, item.second));
            }
            if (opts.tab_align) {
                int col = (base + 1) * std::max(0, opts.indent_size);
                dir_width = snap_to_indent_grid(col + dir_width, opts.indent_size) - col;
            }
            tokens[name].fmt_spaces_before = (i == mp.first ? 1 : tokens[name].fmt_spaces_before);
            tokens[open].fmt_spaces_before = 1;
            for (size_t k = 0; k < items.size(); ++k) {
                auto item = items[k];
                tokens[item.first].fmt_newline_before = true;
                tokens[item.first].fmt_blank_lines = 0;
                tokens[item.first].fmt_indent = base + 1;
                tokens[item.first].fmt_spaces_before = 0;
                size_t rem = next_code_sig(tokens, item.first + 1, item.second);
                if (rem != SIZE_MAX && mo.align)
                    tokens[rem].fmt_spaces_before = std::max(1, dir_width - (int)tok_text(tokens[item.first]).size());
                else if (rem != SIZE_MAX)
                    tokens[rem].fmt_spaces_before = 1;
                size_t comma = find_top_level_token(tokens, item.second, close, ",", TokenKind::Comma);
                if (comma != SIZE_MAX) {
                    if (mo.align) {
                        int sw = rem != SIZE_MAX ? token_range_width_compact(tokens, rem, item.second) : 0;
                        int cur_sig_width = mo.align_adaptive
                            ? std::max(tab_aligned_width(mo.signal_min_width, opts), sw)
                            : sig_width;
                        tokens[comma].fmt_spaces_before = std::max(0, cur_sig_width - sw);
                    } else {
                        tokens[comma].fmt_spaces_before = 0;
                    }
                }
            }
            tokens[close].fmt_newline_before = true;
            tokens[close].fmt_blank_lines = 0;
            tokens[close].fmt_indent = base;
            tokens[close].fmt_spaces_before = 0;
        }
        tokens[semi].fmt_spaces_before = 0;
        i = semi;
    }
}

static void expand_instances_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].fmt_disabled || tokens[i].fmt_passthrough || !is_identifier(tokens[i]))
            continue;
        size_t mod = i;
        if (is_keyword(tokens[mod]))
            continue;
        size_t j = next_code_sig(tokens, mod + 1, tokens.size());
        if (j != SIZE_MAX && tok_is(tokens[j], "#", TokenKind::Hash)) {
            size_t po = next_code_sig(tokens, j + 1, tokens.size());
            if (po == SIZE_MAX || !tok_is(tokens[po], "(", TokenKind::OpenParenthesis))
                continue;
            j = next_code_sig(tokens, matching_close_paren(tokens, po, tokens.size()) + 1, tokens.size());
        }
        if (j == SIZE_MAX || !is_identifier(tokens[j]))
            continue;
        size_t inst_name = j;
        size_t open = SIZE_MAX;
        int bracket = 0;
        for (j = next_code_sig(tokens, inst_name + 1, tokens.size()); j != SIZE_MAX; j = next_code_sig(tokens, j + 1, tokens.size())) {
            if (tok_is(tokens[j], "[", TokenKind::OpenBracket)) ++bracket;
            else if (tok_is(tokens[j], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
            else if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis) && bracket == 0) { open = j; break; }
            else if (bracket == 0) break;
        }
        if (open == SIZE_MAX)
            continue;
        size_t close = matching_close_paren(tokens, open, tokens.size());
        if (close == SIZE_MAX)
            continue;
        size_t semi = next_code_sig(tokens, close + 1, tokens.size());
        if (semi == SIZE_MAX || !tok_is(tokens[semi], ";", TokenKind::Semicolon) ||
            token_range_has_pp_conditional(tokens, i, semi + 1) || token_range_disabled_or_passthrough(tokens, i, semi + 1))
            continue;
        auto ports = top_level_ranges_between(tokens, open + 1, close);
        if (ports.empty())
            continue;
        bool named = true;
        int max_port = 0, max_sig = 0;
        struct P { size_t dot, name, op, cl, comma; int sigw; };
        std::vector<P> ps;
        for (auto r : ports) {
            size_t dot = r.first;
            if (tok_text(tokens[dot]) != ".") { named = false; break; }
            size_t name = next_code_sig(tokens, dot + 1, r.second);
            size_t op = next_code_sig(tokens, name == SIZE_MAX ? r.second : name + 1, r.second);
            if (name == SIZE_MAX || op == SIZE_MAX || !tok_is(tokens[op], "(", TokenKind::OpenParenthesis)) { named = false; break; }
            size_t cl = matching_close_paren(tokens, op, r.second);
            if (cl == SIZE_MAX) { named = false; break; }
            size_t comma = find_top_level_token(tokens, r.second, close, ",", TokenKind::Comma);
            int sigw = token_range_width_compact(tokens, op + 1, cl);
            max_port = std::max(max_port, (int)tok_text(tokens[name]).size());
            max_sig = std::max(max_sig, sigw);
            ps.push_back({dot, name, op, cl, comma, sigw});
        }
        if (!named)
            continue;
        int base = tokens[mod].fmt_indent;
        int port_indent = opts.instance.port_indent_level;
        tokens[open].fmt_spaces_before = 1;
        int eff_before = std::max(1, opts.instance.instance_port_name_width - max_port - 1);
        int eff_inside = std::max(0, opts.instance.instance_port_between_paren_width - max_sig);
        for (const auto& p : ps) {
            tokens[p.dot].fmt_newline_before = true;
            tokens[p.dot].fmt_blank_lines = 0;
            tokens[p.dot].fmt_indent = base + port_indent;
            tokens[p.dot].fmt_spaces_before = 0;
            tokens[p.name].fmt_spaces_before = 0;
            int before = opts.instance.align_adaptive ? std::max(1, opts.instance.instance_port_name_width - (int)tok_text(tokens[p.name]).size() - 1) : eff_before + (max_port - (int)tok_text(tokens[p.name]).size());
            tokens[p.op].fmt_spaces_before = before;
            int inside = opts.instance.align_adaptive ? std::max(0, opts.instance.instance_port_between_paren_width - p.sigw) : eff_inside + (max_sig - p.sigw);
            tokens[p.cl].fmt_spaces_before = inside;
            if (p.comma != SIZE_MAX)
                tokens[p.comma].fmt_spaces_before = 0;
        }
        tokens[close].fmt_newline_before = true;
        tokens[close].fmt_blank_lines = 0;
        tokens[close].fmt_indent = base;
        tokens[close].fmt_spaces_before = 0;
        tokens[semi].fmt_spaces_before = 0;
        i = semi;
    }
}

static PPContext build_pp_context(const std::vector<Tok>& tokens, const FormatOptions&) {
    PPContext ctx;
    auto starts = tok_line_starts(tokens);
    ctx.lines.resize(starts.size());
    int depth = 0;
    for (size_t li = 0; li < starts.size(); ++li) {
        size_t s = starts[li], e = (li + 1 < starts.size()) ? starts[li + 1] : tokens.size();
        ctx.lines[li].depth_before = depth;
        bool is_if = false, is_endif = false, is_cond = false;
        size_t first = SIZE_MAX;
        for (size_t k = s; k < e && k < tokens.size(); ++k) {
            if (!tok_whitespace(tokens[k]) && !tok_comment(tokens[k])) {
                first = k;
                break;
            }
        }
        if (first != SIZE_MAX && is_line_directive(tokens[first]) &&
            is_pp_conditional(tok_directive_kind(tokens[first]))) {
            is_cond = true;
            auto kind = tok_directive_kind(tokens[first]);
            is_if = kind == syntax::SyntaxKind::IfDefDirective || kind == syntax::SyntaxKind::IfNDefDirective;
            is_endif = kind == syntax::SyntaxKind::EndIfDirective;
        }
        ctx.lines[li].conditional = is_cond;
        if (is_endif && depth > 0) --depth;
        if (is_if) ++depth;
        ctx.lines[li].depth_after = depth;
    }
    return ctx;
}

static void format_arg_list_metadata(std::vector<Tok>& tokens, size_t open, size_t close,
                                     int base_indent, int hanging_indent, bool hanging) {
    auto args = top_level_ranges_between(tokens, open + 1, close);
    if (args.empty())
        return;
    if (hanging) {
        tokens[args[0].first].fmt_newline_before = false;
        tokens[args[0].first].fmt_spaces_before = 0;
        for (size_t k = 1; k < args.size(); ++k) {
            tokens[args[k].first].fmt_newline_before = true;
            tokens[args[k].first].fmt_blank_lines = 0;
            tokens[args[k].first].fmt_indent = hanging_indent;
            tokens[args[k].first].fmt_spaces_before = 0;
        }
        tokens[close].fmt_newline_before = false;
        tokens[close].fmt_spaces_before = 0;
    } else {
        for (auto r : args) {
            tokens[r.first].fmt_newline_before = true;
            tokens[r.first].fmt_blank_lines = 0;
            tokens[r.first].fmt_indent = base_indent + 1;
            tokens[r.first].fmt_spaces_before = 0;
        }
        tokens[close].fmt_newline_before = true;
        tokens[close].fmt_blank_lines = 0;
        tokens[close].fmt_indent = base_indent;
        tokens[close].fmt_spaces_before = 0;
    }
    for (auto r : args) {
        size_t comma = find_top_level_token(tokens, r.second, close, ",", TokenKind::Comma);
        if (comma != SIZE_MAX)
            tokens[comma].fmt_spaces_before = 0;
    }
}

static size_t find_simple_call_tokens(const std::vector<Tok>& tokens, size_t first, size_t end,
                                      size_t& name, size_t& open, size_t& close) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        bool callable = is_identifier(tokens[i]) || starts_with_chars(tok_text(tokens[i]), '`');
        if (!callable || is_function_call_skip_token(tokens[i]))
            continue;
        size_t op = next_code_sig(tokens, i + 1, end);
        if (op == SIZE_MAX || !tok_is(tokens[op], "(", TokenKind::OpenParenthesis))
            continue;
        size_t cl = matching_close_paren(tokens, op, end);
        if (cl == SIZE_MAX)
            continue;
        name = i; open = op; close = cl; return i;
    }
    return SIZE_MAX;
}

static void format_function_calls_pass(std::vector<Tok>& tokens, const FormatOptions& opts);

static void format_pp_conditional_function_calls_pass(std::vector<Tok>& tokens,
                                                      const FormatOptions& opts,
                                                      const PPContext&) {
    format_function_calls_pass(tokens, opts);
}

static void format_multiline_macro_arg_calls_pass(std::vector<Tok>& tokens,
                                                  const FormatOptions& opts) {
    const auto& fo = opts.function;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].fmt_disabled || tokens[i].fmt_passthrough || tok_whitespace(tokens[i]) || tok_comment(tokens[i]))
            continue;
        size_t semi = statement_end_semicolon(tokens, i);
        if (semi == SIZE_MAX || token_range_has_pp_conditional(tokens, i, semi + 1))
            continue;
        size_t name, open, close;
        if (find_simple_call_tokens(tokens, i, semi + 1, name, open, close) == SIZE_MAX || tok_text(tokens[name])[0] == '`')
            continue;
        bool has_macro = false;
        for (size_t k = open + 1; k < close; ++k)
            has_macro = has_macro || tok_text(tokens[k]).find('`') != std::string::npos;
        auto args = top_level_ranges_between(tokens, open + 1, close);
        if (!has_macro || args.size() <= 1)
            continue;
        tokens[open].fmt_spaces_before = fo.space_before_paren ? 1 : 0;
        int hang = tokens[name].fmt_indent;
        format_arg_list_metadata(tokens, open, close, tokens[name].fmt_indent, hang, true);
        i = semi;
    }
}

static void format_function_calls_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    const auto& fo = opts.function;
    auto starts = tok_line_starts(tokens);
    for (size_t li = 0; li < starts.size(); ++li) {
        size_t s = starts[li], e = (li + 1 < starts.size()) ? starts[li + 1] : tokens.size();
        if (token_range_disabled_or_passthrough(tokens, s, e) || token_range_has_pp_conditional(tokens, s, e))
            continue;
        size_t name, open, close;
        if (find_simple_call_tokens(tokens, s, e, name, open, close) == SIZE_MAX || tok_text(tokens[name])[0] == '`')
            continue;
        bool decl_prefix = false;
        for (size_t k = s; k < name; ++k)
            decl_prefix = decl_prefix || tokens[k].kind == TokenKind::FunctionKeyword || tokens[k].kind == TokenKind::TaskKeyword || tokens[k].kind == TokenKind::ModuleKeyword || tokens[k].kind == TokenKind::ClassKeyword;
        if (decl_prefix)
            continue;
        auto args = top_level_ranges_between(tokens, open + 1, close);
        bool has_macro_arg = false;
        for (auto r : args)
            for (size_t k = r.first; k < r.second; ++k)
                has_macro_arg = has_macro_arg || tok_text(tokens[k]).find('`') != std::string::npos;
        bool do_break = false;
        if (has_macro_arg && args.size() > 1) do_break = true;
        else if (fo.break_policy == "always") do_break = !args.empty();
        else if (fo.break_policy == "auto") do_break = fo.arg_count >= 0 && (int)args.size() >= fo.arg_count;
        tokens[open].fmt_spaces_before = fo.space_before_paren ? 1 : 0;
        if (!do_break || fo.break_policy == "never") {
            if (!args.empty() && fo.space_inside_paren)
                tokens[args.front().first].fmt_spaces_before = 1;
            tokens[close].fmt_spaces_before = (!args.empty() && fo.space_inside_paren) ? 1 : 0;
            continue;
        }
        format_arg_list_metadata(tokens, open, close, tokens[name].fmt_indent, tokens[name].fmt_indent, fo.layout == "hanging");
    }
}

static void format_covergroup_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    int cover_depth = 0;
    int brace_depth = 0;
    int cover_base = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tokens[i].kind == TokenKind::CoverGroupKeyword) {
            cover_depth++;
            cover_base = tokens[i].fmt_indent;
        } else if (tokens[i].kind == TokenKind::EndGroupKeyword) {
            tokens[i].fmt_indent = cover_base;
            cover_depth = std::max(0, cover_depth - 1);
            brace_depth = 0;
        } else if (cover_depth > 0 && !tokens[i].fmt_disabled && !tokens[i].fmt_passthrough) {
            bool close_brace = tok_is(tokens[i], "}", TokenKind::CloseBrace);
            if (tokens[i].fmt_newline_before) {
                int d = brace_depth - (close_brace ? 1 : 0);
                tokens[i].fmt_indent = cover_base + 1 + std::max(0, d);
                tokens[i].fmt_spaces_before = 0;
            }
            if (tok_is(tokens[i], "{", TokenKind::OpenBrace)) ++brace_depth;
            else if (close_brace && brace_depth > 0) --brace_depth;
        }
    }
    (void)opts;
}

static void format_constraint_dist_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind != TokenKind::DistKeyword || tokens[i].fmt_disabled || tokens[i].fmt_passthrough)
            continue;
        size_t open = next_code_sig(tokens, i + 1, tokens.size());
        if (open == SIZE_MAX || !tok_is(tokens[open], "{", TokenKind::OpenBrace))
            continue;
        size_t close = matching_close_token(tokens, open, tokens.size(), "{", "}", TokenKind::OpenBrace, TokenKind::CloseBrace);
        if (close == SIZE_MAX || token_range_has_pp_conditional(tokens, i, close + 1))
            continue;
        auto items = top_level_ranges_between(tokens, open + 1, close);
        if (items.size() <= 1)
            continue;
        int base = tokens[i].fmt_indent;
        if (opts.statement.begin_newline) {
            tokens[open].fmt_newline_before = true;
            tokens[open].fmt_indent = base;
            tokens[open].fmt_spaces_before = 0;
        } else {
            tokens[open].fmt_spaces_before = 1;
        }
        for (auto r : items) {
            tokens[r.first].fmt_newline_before = true;
            tokens[r.first].fmt_blank_lines = 0;
            tokens[r.first].fmt_indent = base + 1;
            tokens[r.first].fmt_spaces_before = 0;
            size_t comma = find_top_level_token(tokens, r.second, close, ",", TokenKind::Comma);
            if (comma != SIZE_MAX) tokens[comma].fmt_spaces_before = 0;
        }
        tokens[close].fmt_newline_before = true;
        tokens[close].fmt_blank_lines = 0;
        tokens[close].fmt_indent = base;
        tokens[close].fmt_spaces_before = 0;
        i = close;
    }
}

static void format_function_declaration_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    const auto& fd = opts.function_declaration;
    auto starts = tok_line_starts(tokens);
    for (size_t li = 0; li < starts.size(); ++li) {
        size_t s = starts[li], e = (li + 1 < starts.size()) ? starts[li + 1] : tokens.size();
        size_t first = next_code_sig(tokens, s, e);
        if (first == SIZE_MAX || (tokens[first].kind != TokenKind::FunctionKeyword && tokens[first].kind != TokenKind::TaskKeyword) ||
            token_range_disabled_or_passthrough(tokens, s, e) || token_range_has_pp_conditional(tokens, s, e))
            continue;
        size_t open = SIZE_MAX;
        for (size_t j = first + 1; j < e; ++j)
            if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) { open = j; break; }
        if (open == SIZE_MAX)
            continue;
        size_t close = matching_close_paren(tokens, open, e);
        if (close == SIZE_MAX)
            continue;
        int approx_len = 0;
        for (size_t j = s; j < e; ++j)
            if (!tok_whitespace(tokens[j])) approx_len += (int)tok_text(tokens[j]).size() + std::max(0, tokens[j].fmt_spaces_before);
        if (approx_len <= fd.line_length)
            continue;
        tokens[open].fmt_spaces_before = 0;
        format_arg_list_metadata(tokens, open, close, tokens[first].fmt_indent, tokens[first].fmt_indent, fd.layout == "hanging");
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// format_source — main entry point
// ---------------------------------------------------------------------------

std::string format_source(const std::string& source, const FormatOptions& opts) {
    ScopedTokenCache token_cache;

    const std::string& input = source;
    write_log(opts, "format_source_input.sv", input);

    auto tokens = collect_lexer_tokens(input);
    write_log(opts, "00_input.sv", input);
    basic_formatting(tokens, input, opts);
    write_log(opts, "01_basic_formatting.sv", render_tokens(tokens, opts));
    align_define_continuation_pass_v2(tokens, opts);
    write_log(opts, "03_align_define_continuation_pass.sv", render_tokens(tokens, opts));


    // Module-header reflow mutates token metadata directly.
    format_class_extends_parameter_pass(tokens, opts);
    write_log(opts, "04_format_class_extends_parameter_pass.sv", render_tokens(tokens, opts));
    format_portlist_pass(tokens, opts);
    write_log(opts, "05_format_portlist_pass.sv", render_tokens(tokens, opts));

    if (opts.statement.align) {
        align_assign_pass(tokens, opts);
        write_log(opts, "06_align_assign_pass.sv", render_tokens(tokens, opts));
    }
    if (opts.var_declaration.align) {
        align_var_pass(tokens, opts);
        write_log(opts, "07_align_var_pass.sv", render_tokens(tokens, opts));
    }
    if (opts.port_declaration.align) {
        align_port_pass(tokens, opts);
        write_log(opts, "08_align_port_pass.sv", render_tokens(tokens, opts));
    }

    format_enum_declaration_pass(tokens, opts);
    write_log(opts, "09_format_enum_declaration_pass.sv", render_tokens(tokens, opts));
    format_modport_pass(tokens, opts);
    write_log(opts, "10_format_modport_pass.sv", render_tokens(tokens, opts));
    if (opts.instance.align) {
        expand_instances_pass(tokens, opts);
        write_log(opts, "11_expand_instances_pass.sv", render_tokens(tokens, opts));
    }
    PPContext pp = build_pp_context(tokens, opts);
    format_pp_conditional_function_calls_pass(tokens, opts, pp);
    write_log(opts, "12_format_pp_conditional_function_calls_pass.sv", render_tokens(tokens, opts));
    format_multiline_macro_arg_calls_pass(tokens, opts);
    write_log(opts, "13_format_multiline_macro_arg_calls_pass.sv", render_tokens(tokens, opts));
    format_function_calls_pass(tokens, opts);
    write_log(opts, "14_format_function_calls_pass.sv", render_tokens(tokens, opts));
    format_covergroup_pass(tokens, opts);
    write_log(opts, "15_format_covergroup_pass.sv", render_tokens(tokens, opts));
    format_constraint_dist_pass(tokens, opts);
    write_log(opts, "16_format_constraint_dist_pass.sv", render_tokens(tokens, opts));
    format_function_declaration_pass(tokens, opts);
    write_log(opts, "17_format_function_declaration_pass.sv", render_tokens(tokens, opts));

    std::string out = render_tokens(tokens, opts);

    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    out += '\n';

    verify_safe_mode_unchanged(source, out, opts);

    return out;
}
