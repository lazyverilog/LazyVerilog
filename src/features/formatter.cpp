#include "formatter.hpp"
#include <algorithm>
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
    if (!opts.debug_main_token_loop || opts.log_path.empty())
        return;
    std::filesystem::path dir(opts.log_path);
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / filename);
    out << text;
}

static std::vector<std::string> text_to_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream ss(text);
    std::string l;
    while (std::getline(ss, l))
        lines.push_back(std::move(l));
    return lines;
}

using slang::parsing::TokenKind;

struct Tok {
    TokenKind kind{TokenKind::Unknown};
    std::string text;
    std::string lo;
    int pos{0};
    bool whitespace{false};
    bool comment{false};
    bool directive{false};
    syntax::SyntaxKind directive_kind{syntax::SyntaxKind::Unknown};
};

static std::vector<Tok> collect_lexer_tokens(const std::string& source);

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
            size_t start = (size_t)tok.pos;
            by_start.emplace(start, DirectiveMatch{tok.directive_kind, start,
                                                   start + tok.text.size()});
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
        if (tok.whitespace || tok.comment)
            continue;
        if (tok.kind != TokenKind::Directive)
            return {};
        return {tok.directive_kind, offset + (size_t)tok.pos,
                offset + (size_t)tok.pos + tok.text.size()};
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
           (t.kind == TokenKind::Directive && t.directive_kind == syntax::SyntaxKind::MacroUsage);
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
    std::string text = tok.text;
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
    return t.directive && !is_macro_usage(t);
}

static bool is_keyword(const Tok& t) {
    return !t.directive && !t.comment && !t.whitespace && LexerFacts::isKeyword(t.kind);
}

static bool is_constraint_keyword(const Tok& t) {
    return !t.directive && !t.comment && !t.whitespace &&
           t.kind == TokenKind::ConstraintKeyword;
}

static bool is_begin_keyword(const Tok& t) {
    return !t.directive && !t.comment && !t.whitespace && t.kind == TokenKind::BeginKeyword;
}

static bool is_else_keyword(const Tok& t) {
    return !t.directive && !t.comment && !t.whitespace && t.kind == TokenKind::ElseKeyword;
}

static bool is_identifier(const Tok& t) {
    return !t.directive && !t.comment && !t.whitespace &&
           (t.kind == TokenKind::Identifier || t.kind == TokenKind::SystemIdentifier ||
            (!t.text.empty() &&
             (std::isalpha((unsigned char)t.text[0]) || t.text[0] == '_' || t.text[0] == '$' ||
              t.text[0] == '`') &&
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

static bool is_open_group(const Tok& t) { return t.text == "(" || t.text == "[" || t.text == "{"; }

static bool is_close_group(const Tok& t) { return t.text == ")" || t.text == "]" || t.text == "}"; }

static bool is_hierarchy(const Tok& t) { return t.text == "." || t.text == "::"; }

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
    if (tok.text == ";" || tok.text == "," || is_close_group(tok) || is_binary_op(tok) ||
        is_assignment_op(tok) || is_unary_op(tok))
        return true;
    return tok.text == "." || tok.text == "::" || tok.text == "[" || tok.text == "with";
}

static bool binary_space_before(const std::string& mode) {
    return mode == "before" || mode == "both";
}

static bool binary_space_after(const std::string& mode) {
    return mode == "after" || mode == "both";
}

enum class ParenKind { Ordinary, FunctionCall, EventControl };

static ParenKind classify_paren(const Tok& L, bool procedural_at = false) {
    if (L.text == "@")
        return procedural_at ? ParenKind::EventControl : ParenKind::FunctionCall;
    if (is_identifier(L) || L.text == "]")
        return ParenKind::FunctionCall;
    return ParenKind::Ordinary;
}

static bool looks_unary_context(const Tok& L, const Tok& R) {
    if (R.text != "+" && R.text != "-")
        return false;
    return L.text == "(" || L.text == "[" || L.text == "{" || L.text == "," || L.text == ";" ||
           L.text == ":" || L.text == "?" || L.text == "=" || is_binary_op(L) || is_keyword(L);
}

static bool is_indexed_part_select_op_pair(const Tok& A, const Tok& B) {
    return (A.text == "+" || A.text == "-") && B.text == ":";
}

static bool is_indexed_part_select_op(const Tok& t) { return t.text == "+:" || t.text == "-:"; }

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
    const auto& lx = L.text;
    const auto& ll = L.lo;
    const auto& rx = R.text;
    const auto& sp = opts.spacing;
    bool L_assign = is_assignment_op(L, in_parens);
    bool R_assign = is_assignment_op(R, in_parens);

    if (is_line_directive(L) || is_line_directive(R))
        return 0;
    if (R.comment)
        return 1;
    if (lx == "(") {
        if (paren_kind == ParenKind::EventControl)
            return sp.space_inside_event_control_parens ? 1 : 0;
        if (paren_kind == ParenKind::Ordinary)
            return sp.space_inside_parens ? 1 : 0;
        return 0;
    }
    if (rx == ")") {
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
        return (is_keyword(L) || is_identifier(L) || L.kind == TokenKind::CloseParenthesis) ? 1 : 0;
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
    const auto& lx = L.text;
    const auto& ll = L.lo;
    const auto& rx = R.text;
    const auto& rl = R.lo;

    if (in_dim && lx != ":" && lx != "[" && lx != "]" && rx != ":" && rx != "[" && rx != "]")
        return SD::Preserve;
    if (is_line_directive(R) || is_line_directive(L))
        return SD::MustWrap;
    if (L.comment && L.text.find('\n') != std::string::npos)
        return SD::MustWrap;
    if (L.comment && L.text.rfind("//", 0) == 0)
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
                     syntax::SyntaxKind directive_kind = syntax::SyntaxKind::Unknown) {
    Tok t;
    t.kind = kind;
    t.lo = lower(text);
    t.text = std::move(text);
    t.pos = pos;
    t.whitespace = whitespace;
    t.comment = comment;
    t.directive = directive;
    t.directive_kind = directive_kind;
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

static void append_trivia_text(std::vector<Tok>& toks, const std::string& source, size_t start,
                               size_t end) {
    size_t i = start;
    while (i < end) {
        if (source[i] == '/' && i + 1 < end && source[i + 1] == '/') {
            size_t j = i + 2;
            while (j < end && source[j] != '\n')
                ++j;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, true, false);
            i = j;
        } else if (source[i] == '/' && i + 1 < end && source[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < end && !(source[j] == '*' && source[j + 1] == '/'))
                ++j;
            if (j + 1 < end)
                j += 2;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, true, false);
            i = j;
        } else if (source[i] == '`') {
            size_t line_start = i;
            while (line_start > start && source[line_start - 1] != '\n')
                --line_start;
            size_t first = line_start;
            while (first < i && (source[first] == ' ' || source[first] == '\t'))
                ++first;
            size_t name_end = i + 1;
            while (name_end < end && (std::isalnum((unsigned char)source[name_end]) ||
                                      source[name_end] == '_' || source[name_end] == '$'))
                ++name_end;
            std::string name = source.substr(i, name_end - i);
            if (first == i && is_compiler_directive_name(name)) {
                size_t j = i;
                do {
                    while (j < end && source[j] != '\n')
                        ++j;
                    if (j == 0 || source[j - 1] != '\\')
                        break;
                    if (j < end)
                        ++j;
                } while (j < end);
                push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, false,
                         true);
                i = j;
            } else {
                push_tok(toks, TokenKind::Identifier, std::move(name), (int)i);
                i = name_end;
            }
        } else if (source[i] == '"') {
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
            push_tok(toks, TokenKind::StringLiteral, source.substr(i, j - i), (int)i);
            i = j;
        } else if (std::isspace((unsigned char)source[i])) {
            size_t j = i + 1;
            while (j < end && std::isspace((unsigned char)source[j]))
                ++j;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, true, false, false);
            i = j;
        } else if (std::isalpha((unsigned char)source[i]) || source[i] == '_' || source[i] == '$') {
            size_t j = i + 1;
            while (j < end &&
                   (std::isalnum((unsigned char)source[j]) || source[j] == '_' || source[j] == '$'))
                ++j;
            push_tok(toks, TokenKind::Identifier, source.substr(i, j - i), (int)i);
            i = j;
        } else if (std::isdigit((unsigned char)source[i])) {
            size_t j = i + 1;
            while (j < end && (std::isalnum((unsigned char)source[j]) || source[j] == '_' ||
                               source[j] == '\''))
                ++j;
            push_tok(toks, TokenKind::IntegerLiteral, source.substr(i, j - i), (int)i);
            i = j;
        } else if (std::ispunct((unsigned char)source[i])) {
            static const std::vector<std::string> OPS = {
                "<<=", ">>=", "===", "!==", "<<<", ">>>", "->", "<->", "&&", "||", "**", "##",
                "|->", "+=",  "-=",  "*=",  "/=",  "%=",  "&=", "|=",  "^=", "<=", ">=", "==",
                "!=",  "::",  "+:",  "-:",  "~&",  "~|",  "~^", "^~",  "++", "--", "<<", ">>"};
            bool matched = false;
            for (const auto& op : OPS) {
                if (i + op.size() <= end && source.compare(i, op.size(), op) == 0) {
                    push_tok(toks, TokenKind::Unknown, op, (int)i);
                    i += op.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                push_tok(toks, single_punct_token_kind(source[i]), source.substr(i, 1), (int)i);
                ++i;
            }
        } else {
            push_tok(toks, TokenKind::Unknown, source.substr(i, 1), (int)i);
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
        if (tok.whitespace)
            continue;
        if (prev) {
            int spaces =
                spaces_req(*prev, tok, opts, in_dim, false, false, ParenKind::Ordinary, false);
            if (spaces > 0)
                out += std::string(spaces, ' ');
        }
        out += tok.text;
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

    std::vector<Tok> toks;
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
        // Fill gap between cursor and this token with trivia text
        // (handles comments, directives, strings in gaps)
        if (cursor < off)
            append_trivia_text(toks, source, cursor, off);
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
                size_t end = off;
                do {
                    while (end < source.size() && source[end] != '\n')
                        ++end;
                    if (end == 0 || source[end - 1] != '\\')
                        break;
                    if (end < source.size())
                        ++end;
                } while (end < source.size());
                push_tok(toks, token.kind, source.substr(off, end - off), (int)off, false, false,
                         true, token.directiveKind());
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
                push_tok(toks, token.kind, source.substr(off, end - off), (int)off);
                cursor = std::max(cursor, end);
                continue;
            }
        }
        if (is_combined_operator_token(token.kind) &&
            (off + raw.size() > source.size() || source.compare(off, raw.size(), raw) != 0)) {
            push_tok(toks, TokenKind::Unknown, raw.substr(0, 1), (int)off);
            cursor = std::max(cursor, off + 1);
            continue;
        }
        if (!raw.empty()) {
            syntax::SyntaxKind directive_kind = syntax::SyntaxKind::Unknown;
            if (token.kind == TokenKind::Directive)
                directive_kind = token.directiveKind();
            push_tok(toks, token.kind, raw, (int)off, false, false, false, directive_kind);
        }
        cursor = std::max(cursor, off + raw.size());
    }
    if (cursor < source.size())
        append_trivia_text(toks, source, cursor, source.size());

    if (active_token_cache) {
        auto [it, _] = active_token_cache->emplace(source, std::move(toks));
        return it->second;
    }
    return toks;
}

static bool line_has_pp_conditional(const std::string& line) {
    for (const auto& tok : collect_lexer_tokens(line)) {
        if (tok.whitespace || tok.comment)
            continue;
        if ((is_line_directive(tok) && is_pp_conditional(tok.directive_kind)) ||
            is_pp_conditional_text(tok.text))
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
            if (tok.whitespace || tok.comment)
                continue;
            if ((is_line_directive(tok) && is_pp_conditional(tok.directive_kind)) ||
                is_pp_conditional_text(tok.text)) {
                is_cond = true;
                syntax::SyntaxKind kind = tok.kind == TokenKind::Directive
                                              ? tok.directive_kind
                                              : directive_at_offset(tok.text, 0).kind;
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
        if (tok.comment) {
            return {trim_right_copy(line.substr(0, (size_t)tok.pos)), " " + line.substr(tok.pos)};
        }
    }
    return {trim_right_copy(line), ""};
}

static std::vector<std::string> code_tokens_for_alignment(const std::string& code) {
    std::vector<std::string> out;
    for (const auto& tok : collect_lexer_tokens(code)) {
        if (tok.whitespace || tok.comment || tok.directive)
            continue;
        out.push_back(tok.text);
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
        if (!tok.whitespace && !tok.comment && !tok.directive)
            out.push_back(std::move(tok));
    }
    return out;
}

static std::vector<Tok> significant_tokens_from(const std::vector<Tok>& toks) {
    std::vector<Tok> out;
    for (const auto& tok : toks) {
        if (!tok.whitespace && !tok.comment && !tok.directive)
            out.push_back(tok);
    }
    return out;
}

static bool tok_is(const Tok& tok, const std::string& text, TokenKind kind) {
    return tok.text == text || tok.kind == kind;
}

static bool tok_contains(const Tok& tok, char ch) {
    if (!tok.text.empty() && tok.text[0] == '"')
        return false;
    if (tok.text.size() > 2)
        return false;
    return tok.text.find(ch) != std::string::npos;
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
        if (tok.whitespace || tok.comment || tok.directive)
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
            parts.push_back({text.substr(start, (size_t)tok.pos - start), true});
            start = (size_t)tok.pos + tok.text.size();
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
        if (tok.whitespace || tok.comment || tok.directive)
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
            parts.push_back(text.substr(start, (size_t)tok.pos - start));
            start = (size_t)tok.pos + tok.text.size();
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

static std::vector<std::string> align_port_pass(std::vector<std::string> lines,
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
            if (tok.whitespace)
                continue;
            return tok.comment;
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

static std::vector<std::string> align_assign_pass(std::vector<std::string> lines,
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
            if (tok.comment)
                break;
            if (tok.whitespace || tok.directive)
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
                std::find(OPS.begin(), OPS.end(), tok.text) != OPS.end())
                return {tok.pos, tok.text};
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
            if (tok.whitespace || tok.comment || tok.directive)
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

static std::vector<std::string> align_var_pass(std::vector<std::string> lines,
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
                if (tok.whitespace)
                    continue;
                comment_or_blank = tok.comment;
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
        if (toks[i].whitespace || toks[i].comment || toks[i].directive || toks[i].text != ".")
            continue;
        size_t name_i = i + 1;
        while (name_i < toks.size() && toks[name_i].whitespace)
            ++name_i;
        if (name_i >= toks.size() || !is_identifier(toks[name_i]))
            continue;
        size_t open_i = name_i + 1;
        while (open_i < toks.size() && toks[open_i].whitespace)
            ++open_i;
        if (open_i < toks.size() && tok_is(toks[open_i], "(", TokenKind::OpenParenthesis))
            last = toks[name_i].text;
    }
    return last;
}

static std::string last_named_port_before_comment(const std::string& code) {
    return last_named_port_before_comment(collect_lexer_tokens(code));
}

static std::string first_named_port_in_code(const std::vector<Tok>& toks) {
    for (size_t i = 0; i + 2 < toks.size(); ++i) {
        if (toks[i].whitespace || toks[i].comment || toks[i].directive || toks[i].text != ".")
            continue;
        size_t name_i = i + 1;
        while (name_i < toks.size() && toks[name_i].whitespace)
            ++name_i;
        if (name_i >= toks.size() || !is_identifier(toks[name_i]))
            continue;
        size_t open_i = name_i + 1;
        while (open_i < toks.size() && toks[open_i].whitespace)
            ++open_i;
        if (open_i < toks.size() && tok_is(toks[open_i], "(", TokenKind::OpenParenthesis))
            return toks[name_i].text;
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
    std::vector<std::string> pending_standalone_comments;
    int depth = 0;
    int param_depth = 0;
    size_t j = start;
    while (j < lines.size()) {
        std::string stripped = lines[j];
        size_t sp = 0;
        while (sp < stripped.size() && (stripped[sp] == ' ' || stripped[sp] == '\t'))
            ++sp;
        stripped = stripped.substr(sp);
        if (line_has_pp_conditional(stripped))
            comments.preserve_original = true;

        std::string code = stripped;
        size_t line_comment = std::string::npos;
        auto stripped_toks = collect_lexer_tokens(stripped);
        for (const auto& tok : stripped_toks) {
            if (tok.comment && starts_with_chars(tok.text, '/', '/')) {
                line_comment = (size_t)tok.pos;
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
                if (tok.whitespace || tok.comment || tok.directive)
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
                        if (tok.whitespace || tok.comment || tok.directive)
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
                        pending_standalone_comments.push_back(comment);
                    else if (comments.header.empty())
                        comments.header = comment;
                }
            }
        }

        param_depth = next_param_depth;

        std::string next_port = first_named_port_in_code(code_toks);
        if (!next_port.empty() && !pending_standalone_comments.empty()) {
            for (const auto& comment : pending_standalone_comments)
                comments.leading_port_comments.push_back({next_port, comment});
            pending_standalone_comments.clear();
        }

        parts.push_back(code);
        for (const auto& tok : code_toks) {
            if (tok.whitespace || tok.comment || tok.directive)
                continue;
            if (tok_is(tok, "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(tok, ")", TokenKind::CloseParenthesis))
                --depth;
            else if (tok_is(tok, ";", TokenKind::Semicolon) && depth == 0) {
                end_i = j + 1;
                comments.footer_comments = std::move(pending_standalone_comments);
                flat = "";
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k)
                        flat += ' ';
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
    size_t start = (size_t)toks[open_i].pos + toks[open_i].text.size();
    size_t end = (size_t)toks[close_i].pos;
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
        size_t tok_start = (size_t)tok.pos;
        size_t tok_end = tok_start + tok.text.size();
        if (tok_start > cursor)
            code += port_list.substr(cursor, tok_start - cursor);

        if (tok.comment && starts_with_chars(tok.text, '/', '*') && paren_depth == 0) {
            size_t next = ti + 1;
            while (next < toks.size() && toks[next].whitespace)
                ++next;
            if (next < toks.size() && tok_is(toks[next], "(", TokenKind::OpenParenthesis)) {
                code += tok.text;
                cursor = tok_end;
                continue;
            }
            std::string port = last_named_port_before_comment(code);
            if (!port.empty()) {
                if (!comments.port_comments.empty() && comments.port_comments.back().first == port)
                    append_comment(comments.port_comments.back().second, tok.text);
                else
                    comments.port_comments.push_back({port, tok.text});
            } else {
                append_comment(comments.header, tok.text);
            }
            cursor = tok_end;
            continue;
        }

        code += tok.text;
        cursor = tok_end;
        if (tok.whitespace || tok.comment || tok.directive)
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

static bool parse_named_ports(const std::string& port_list,
                              std::vector<std::pair<std::string, std::string>>& ports) {
    auto toks = collect_lexer_tokens(port_list);
    size_t i = 0;
    auto skip_ws_commas = [&]() {
        while (i < toks.size() &&
               (toks[i].whitespace || tok_is(toks[i], ",", TokenKind::Comma)))
            ++i;
    };

    while (true) {
        skip_ws_commas();
        if (i >= toks.size())
            break;
        if (toks[i].comment || toks[i].directive)
            return false;
        if (toks[i].text != ".")
            return false; // positional
        ++i;
        while (i < toks.size() && toks[i].whitespace)
            ++i;
        if (i >= toks.size() || !is_identifier(toks[i]))
            return false;
        std::string port_name = toks[i].text;
        ++i;
        while (i < toks.size() && toks[i].whitespace)
            ++i;
        while (i < toks.size() && toks[i].comment && starts_with_chars(toks[i].text, '/', '*')) {
            port_name += " " + toks[i].text;
            ++i;
            while (i < toks.size() && toks[i].whitespace)
                ++i;
        }
        if (i >= toks.size() || !tok_is(toks[i], "(", TokenKind::OpenParenthesis))
            return false;
        size_t sig_start = (size_t)toks[i].pos + toks[i].text.size();
        ++i;
        int depth = 1;
        size_t close_pos = std::string::npos;
        while (i < toks.size() && depth > 0) {
            if (toks[i].whitespace || toks[i].comment || toks[i].directive) {
                ++i;
                continue;
            }
            if (tok_is(toks[i], "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(toks[i], ")", TokenKind::CloseParenthesis) && --depth == 0) {
                close_pos = (size_t)toks[i].pos;
                ++i;
                break;
            }
            ++i;
        }
        if (close_pos == std::string::npos || close_pos < sig_start)
            return false;
        std::string sig = trim_copy(port_list.substr(sig_start, close_pos - sig_start));
        ports.push_back({port_name, sig});
    }
    return !ports.empty();
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
    module_type = toks[i++].text;

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
        size_t start = (size_t)toks[hash_i].pos;
        size_t end = (size_t)toks[close_i].pos + toks[close_i].text.size();
        param_block = flat.substr(start, end - start);
        while (!param_block.empty() && (param_block.back() == ' ' || param_block.back() == '\t'))
            param_block.pop_back();
        i = close_i + 1;
    }

    if (i >= toks.size() || !is_identifier(toks[i]))
        return false;
    size_t inst_i = i++;
    inst_name = toks[inst_i].text;

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
    while (after_close < toks.size() && toks[after_close].comment)
        ++after_close;
    if (after_close >= toks.size() || !tok_is(toks[after_close], ";", TokenKind::Semicolon))
        return false;
    if (after_close + 1 != toks.size())
        return false;

    size_t suffix_start = (size_t)toks[inst_i].pos + toks[inst_i].text.size();
    size_t suffix_end = (size_t)toks[open_i].pos;
    inst_suffix = flat.substr(suffix_start, suffix_end - suffix_start);
    size_t a = 0;
    while (a < inst_suffix.size() && (inst_suffix[a] == ' ' || inst_suffix[a] == '\t'))
        ++a;
    size_t b = inst_suffix.size();
    while (b > a && (inst_suffix[b - 1] == ' ' || inst_suffix[b - 1] == '\t'))
        --b;
    inst_suffix = inst_suffix.substr(a, b - a);
    port_open = (size_t)toks[open_i].pos;
    port_close = (size_t)toks[close_i].pos;
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

static std::vector<std::string> expand_instances_pass(std::vector<std::string> lines,
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
    while (i < lines.size()) {
        const std::string& line = lines[i];

        if (in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i])) {
            out.push_back(lines[i]);
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
        std::vector<std::pair<std::string, std::string>> ports;
        if (!parse_named_ports(port_list, ports)) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }
        int max_port = 0, max_sig = 0;
        for (auto& [p, s] : ports) {
            max_port = std::max(max_port, (int)p.size());
            max_sig = std::max(max_sig, (int)s.size());
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
        for (size_t k = 0; k < ports.size(); ++k) {
            auto& [port, sig] = ports[k];
            while (leading_comment_index < comments.leading_port_comments.size() &&
                   comments.leading_port_comments[leading_comment_index].first == port) {
                out.push_back(indent + port_indent +
                              comments.leading_port_comments[leading_comment_index++].second);
            }
            std::string comma = (k + 1 == ports.size()) ? "" : ",";
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
            if (tok.whitespace || tok.comment || tok.directive)
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
        if (tok.whitespace || tok.comment || tok.directive)
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
            return (size_t)tok.pos;
    }
    return std::string::npos;
}

static std::string pad_right(std::string s, int width) {
    if ((int)s.size() < width)
        s.resize(width, ' ');
    return s;
}

static std::vector<std::string> format_enum_declaration_pass(std::vector<std::string> lines,
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

        std::string leading = line.substr(0, (size_t)first_line_toks[0].pos);
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
        size_t open = (size_t)toks[open_i].pos;
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
        size_t close = (size_t)toks[close_i].pos;
        size_t semi_i = close_i + 1;
        while (semi_i < toks.size() && !tok_is(toks[semi_i], ";", TokenKind::Semicolon))
            ++semi_i;
        if (semi_i >= toks.size()) {
            out.push_back(lines[i]);
            continue;
        }
        size_t semi = (size_t)toks[semi_i].pos;

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
    size_t rest_start = (size_t)toks[0].pos + toks[0].text.size();
    size_t semi = (size_t)toks.back().pos;
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
        mp.name = entry_toks[0].text;
        size_t open = (size_t)entry_toks[open_i].pos;
        size_t close = (size_t)entry_toks[close_i].pos;
        auto items = split_top_level(entry.substr(open + 1, close - open - 1));
        for (auto& item_raw : items) {
            std::string item = trim_copy(item_raw);
            if (item.empty())
                continue;
            auto item_toks = significant_tokens(item);
            if (item_toks.empty())
                return false;
            std::string dir = item_toks[0].text;
            size_t remainder_start = (size_t)item_toks[0].pos + item_toks[0].text.size();
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

static std::vector<std::string> format_modport_pass(std::vector<std::string> lines,
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

        std::string leading = line.substr(0, (size_t)line_toks[0].pos);
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
        if (tok.whitespace || tok.comment)
            continue;
        bool callable = is_identifier(tok) || starts_with_chars(tok.text, '`');
        if (!callable)
            continue;
        size_t prev = i;
        while (prev > 0 && toks[prev - 1].whitespace)
            --prev;
        size_t member_start = i;
        if (prev > 0 && toks[prev - 1].text == ".") {
            size_t base = prev - 1;
            while (base > 0 && toks[base - 1].whitespace)
                --base;
            if (base == 0 || !is_identifier(toks[base - 1]))
                continue;
            member_start = base - 1;
        }
        size_t j = i + 1;
        while (j < toks.size() && toks[j].whitespace)
            ++j;
        if (j >= toks.size() || !tok_is(toks[j], "(", TokenKind::OpenParenthesis))
            continue;
        if (is_function_call_skip_token(tok))
            continue;
        int depth = 1;
        size_t k = j;
        while (++k < toks.size()) {
            if (toks[k].whitespace || toks[k].comment || toks[k].directive)
                continue;
            if (tok_contains(toks[k], '('))
                ++depth;
            else if (tok_contains(toks[k], ')') && --depth == 0)
                break;
        }
        if (depth != 0 || k >= toks.size())
            return false;
        name_start = (size_t)toks[member_start].pos;
        name_end = (size_t)tok.pos + tok.text.size();
        open = (size_t)toks[j].pos;
        close = (size_t)toks[k].pos;
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

static std::vector<std::string> format_function_calls_pass(std::vector<std::string> lines,
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
            if (tok.whitespace || tok.comment || tok.directive)
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

static std::vector<std::string> format_pp_conditional_function_calls_pass(
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
        std::string flat;
        if (!collect_statement_lines_pp_aware(lines, pp, i, end_i, flat) || end_i == i ||
            !pp_range_has_conditional(pp, i, end_i)) {
            out.push_back(lines[i]);
            continue;
        }

        size_t ns = 0, ne = 0, op = 0, cl = 0;
        if (!find_simple_call(flat, ns, ne, op, cl) || ns >= flat.size() || flat[ns] == '`') {
            out.push_back(lines[i]);
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
            out.push_back(lines[i]);
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
            out.push_back(lines[i]);
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

static std::vector<std::string> format_multiline_macro_arg_calls_pass(
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
        std::string flat;
        if (!collect_statement_lines(lines, i, end_i, flat) || end_i == i ||
            range_has_pp_conditional(lines, i, end_i)) {
            out.push_back(lines[i]);
            continue;
        }
        bool disabled_range = false;
        for (size_t k = i; k <= end_i; ++k)
            disabled_range = disabled_range || in_disabled(line_starts[k], disabled);
        if (disabled_range) {
            out.push_back(lines[i]);
            continue;
        }

        size_t ns = 0, ne = 0, op = 0, cl = 0;
        if (!find_simple_call(flat, ns, ne, op, cl)) {
            out.push_back(lines[i]);
            continue;
        }
        std::string args_text = flat.substr(op + 1, cl - op - 1);
        if (args_text.find('`') == std::string::npos) {
            out.push_back(lines[i]);
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
            out.push_back(lines[i]);
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
            out.push_back(lines[i]);
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
    size_t open_paren = (size_t)toks[i].pos;

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
    size_t close_paren = (size_t)toks[close_index].pos;

    size_t semi_index = close_index + 1;
    if (semi_index >= toks.size() || !tok_is(toks[semi_index], ";", TokenKind::Semicolon))
        return false;
    if (semi_index + 1 != toks.size())
        return false;
    size_t semi_pos = (size_t)toks[semi_index].pos;

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
        if (tok.whitespace || tok.comment || tok.directive)
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
            parts.push_back(text.substr(start, (size_t)tok.pos - start));
            start = (size_t)tok.pos + tok.text.size();
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
            size_t end = (size_t)tok.pos;
            size_t next = (size_t)tok.pos + tok.text.size();
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
        if (tok.whitespace)
            continue;
        return tok.comment;
    }
    return false;
}

static bool is_line_comment_token(const Tok& tok) {
    return tok.comment && starts_with_chars(tok.text, '/', '/');
}

static size_t first_line_comment_start_tokenized(const std::string& text) {
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (is_line_comment_token(tok))
            return (size_t)tok.pos;
    }
    return std::string::npos;
}

static bool token_starts_physical_line(const std::string& text, const Tok& tok) {
    size_t p = (size_t)tok.pos;
    while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t'))
        --p;
    return p == 0 || text[p - 1] == '\n';
}

static size_t first_standalone_line_comment_tokenized(const std::string& text) {
    for (const auto& tok : collect_lexer_tokens(text)) {
        if (is_line_comment_token(tok) && token_starts_physical_line(text, tok))
            return (size_t)tok.pos;
    }
    return std::string::npos;
}

static size_t token_end(const Tok& tok) {
    return (size_t)tok.pos + tok.text.size();
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
        if (comment != std::string::npos && (size_t)toks[i + 1].pos < comment) {
            line.erase((size_t)toks[i + 1].pos, token_end(toks[i + 1]) - (size_t)toks[i + 1].pos);
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
        size_t comma = (size_t)tok.pos;
        if (comment == std::string::npos || comma < comment)
            remove_space_before_comma_token(line, comma);
    }
}

static bool remove_last_code_comma(std::string& line) {
    size_t comment = first_line_comment_start_tokenized(line);
    auto toks = significant_tokens(line);
    for (size_t i = toks.size(); i > 0; --i) {
        const auto& tok = toks[i - 1];
        if (comment != std::string::npos && (size_t)tok.pos >= comment)
            continue;
        if (!tok_is(tok, ",", TokenKind::Comma))
            return false;
        line.erase((size_t)tok.pos, token_end(tok) - (size_t)tok.pos);
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
        if (tok.whitespace)
            continue;
        if (tok.comment)
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
        if (tok.whitespace)
            continue;
        if (!tok.comment || !starts_with_chars(tok.text, '/', '*'))
            break;
        comments += ports_str.substr(erase_end, (size_t)tok.pos + tok.text.size() - erase_end);
        erase_end = (size_t)tok.pos + tok.text.size();
    }
    if (erase_end > 0)
        ports_str.erase(0, erase_end);
    return comments;
}

static std::string leading_horizontal_whitespace_tokenized(const std::string& line) {
    for (const auto& tok : collect_lexer_tokens(line)) {
        if (tok.whitespace)
            continue;
        return line.substr(0, (size_t)tok.pos);
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
                hash_pos = (size_t)toks[i].pos;
                param_open = (size_t)toks[j].pos;
                param_close = (size_t)toks[k].pos;
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

static std::vector<std::string> format_class_extends_parameter_pass(
    std::vector<std::string> lines, const FormatOptions& opts) {
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);
    auto line_starts = line_start_offsets(lines);

    std::vector<std::string> out;
    for (size_t li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        if (in_disabled(line_starts[li], disabled) || line_has_pp_conditional(line)) {
            out.push_back(line);
            continue;
        }

        auto toks = significant_tokens(line);
        if (toks.empty() || toks[0].kind != TokenKind::ClassKeyword ||
            !line_has_token_kind(line, TokenKind::ExtendsKeyword)) {
            out.push_back(line);
            continue;
        }

        size_t hash_i = toks.size();
        for (size_t i = 0; i + 1 < toks.size(); ++i) {
            if (tok_is(toks[i], "#", TokenKind::Hash) &&
                tok_is(toks[i + 1], "(", TokenKind::OpenParenthesis)) {
                hash_i = i;
                break;
            }
        }
        if (hash_i == toks.size()) {
            out.push_back(line);
            continue;
        }

        int depth = 1;
        size_t close_i = hash_i + 1;
        while (++close_i < toks.size()) {
            if (tok_is(toks[close_i], "(", TokenKind::OpenParenthesis))
                ++depth;
            else if (tok_is(toks[close_i], ")", TokenKind::CloseParenthesis) && --depth == 0)
                break;
        }
        if (depth != 0 || close_i >= toks.size()) {
            out.push_back(line);
            continue;
        }

        size_t hash_pos = (size_t)toks[hash_i].pos;
        size_t open_pos = (size_t)toks[hash_i + 1].pos;
        size_t close_pos = (size_t)toks[close_i].pos;
        std::string before_hash = trim_right_copy(line.substr(0, hash_pos));
        std::string params_str = line.substr(open_pos + 1, close_pos - open_pos - 1);
        std::string suffix = trim_copy(line.substr(close_pos + 1));

        auto raw_params = split_top_level(params_str);
        std::vector<std::string> params;
        for (auto& p : raw_params) {
            std::string trimmed = trim_copy(p);
            if (!trimmed.empty())
                params.push_back(trimmed);
        }
        if (params.size() <= 1) {
            out.push_back(line);
            continue;
        }

        std::string leading_ws = leading_horizontal_whitespace_tokenized(line);
        if (opts.module.parameter_layout == "block") {
            std::string param_indent = leading_ws + std::string(opts.indent_size, ' ');
            std::string r = before_hash + " #(\n";
            for (size_t i = 0; i < params.size(); ++i) {
                r += param_indent + params[i];
                if (i + 1 < params.size())
                    r += ",";
                r += "\n";
            }
            r += leading_ws + ")" + suffix;
            out.push_back(r);
            continue;
        }

        std::string open = before_hash + " #(";
        std::string hang(open.size(), ' ');
        std::string r = open + params[0];
        for (size_t i = 1; i < params.size(); ++i)
            r += ",\n" + hang + params[i];
        r += ")" + suffix;
        out.push_back(r);
    }
    return out;
}

static std::vector<std::string> format_portlist_pass(std::vector<std::string> lines,
                                                       const FormatOptions& opts) {
    const std::string indent_unit(opts.indent_size, ' ');

    std::vector<std::string> out;
    auto push_original_lines = [&](size_t first, size_t last) {
        for (size_t k = first; k <= last; ++k)
            out.push_back(lines[k]);
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];

        if (line_has_pp_conditional(line) || !could_start_module_header(line)) {
            out.push_back(lines[i]);
            continue;
        }

        bool module_start = is_module_header_start_tokenized(line);
        if (!module_start) {
            out.push_back(lines[i]);
            continue;
        }

        ModuleHeaderScan header_scan;
        bool header_complete = scan_module_header_line(header_scan, line);

        size_t consumed_end = i;
        if (!header_complete) {
            std::string flat = line;
            size_t j = i + 1;
            for (; j < lines.size(); ++j) {
                std::string trimmed = trim_copy(lines[j]);
                // Preserve line boundaries so // comments in ANSI headers don't
                // comment out following port declarations in the flattened text.
                flat += "\n" + trimmed;
                if (scan_module_header_line(header_scan, lines[j]))
                    break;
            }
            if (j < lines.size()) {
                line = flat;
                consumed_end = j;
            }
        }
        if (range_has_pp_conditional(lines, i, consumed_end)) {
            push_original_lines(i, consumed_end);
            i = consumed_end;
            continue;
        }

        // Try to extract single-line module header: module foo [#(...)] (ports);
        std::string prefix, ports_str, suffix_str;
        if (!extract_single_line_module_header(line, prefix, ports_str, suffix_str)) {
            push_original_lines(i, consumed_end);
            i = consumed_end;
            continue;
        }
        std::string leading_ws = leading_horizontal_whitespace_tokenized(line);
        prefix = format_module_parameter_prefix(prefix, leading_ws, opts);
        prefix += take_leading_portlist_block_comments_tokenized(ports_str);

        auto ports = split_module_ports_tokenized(ports_str);
        std::vector<std::string> trimmed_ports;
        for (auto& p : ports) {
            std::string rest = trim_all_copy(p);
            while (!rest.empty()) {
                bool consumed_directive = false;
                for (const auto& tok : collect_lexer_tokens(rest)) {
                    if (tok.whitespace)
                        continue;
                    if (is_line_directive(tok) && tok.pos == 0) {
                        trimmed_ports.push_back(trim_all_copy(tok.text));
                        rest = trim_all_copy(rest.substr(tok.text.size()));
                        consumed_directive = true;
                    }
                    break;
                }
                if (consumed_directive)
                    continue;

                auto rest_toks = collect_lexer_tokens(rest);
                const Tok* first_tok = nullptr;
                for (const auto& tok : rest_toks) {
                    if (!tok.whitespace) {
                        first_tok = &tok;
                        break;
                    }
                }

                if (first_tok && is_line_comment_token(*first_tok)) {
                    size_t line_end = line_end_after_token(rest, *first_tok);
                    if (line_end >= rest.size()) {
                        trimmed_ports.push_back(rest);
                        rest.clear();
                    } else {
                        trimmed_ports.push_back(trim_all_copy(rest.substr(0, line_end)));
                        rest = trim_all_copy(rest.substr(line_end + 1));
                    }
                    continue;
                }

                size_t standalone_comment = first_standalone_line_comment_tokenized(rest);
                if (standalone_comment == std::string::npos) {
                    trimmed_ports.push_back(std::move(rest));
                    break;
                }

                std::string code = trim_all_copy(rest.substr(0, standalone_comment));
                if (!code.empty())
                    trimmed_ports.push_back(std::move(code));
                rest = trim_all_copy(rest.substr(standalone_comment));
            }
        }
        if (trimmed_ports.empty()) {
            push_original_lines(i, consumed_end);
            i = consumed_end;
            continue;
        }

        // Leading whitespace
        std::string port_indent = leading_ws + indent_unit;

        // ANSI vs non-ANSI detection
        std::vector<PortListEntryKind> port_kinds;
        port_kinds.reserve(trimmed_ports.size());
        bool is_ansi = false;
        for (auto& p : trimmed_ports) {
            PortListEntryKind kind = classify_portlist_entry(p);
            port_kinds.push_back(kind);
            if (kind == PortListEntryKind::Port) {
                is_ansi = true;
            }
        }

        std::string new_lines_str;
        if (is_ansi) {
            auto has_later_port = [&](size_t index) {
                for (size_t pi = index + 1; pi < trimmed_ports.size(); ++pi) {
                    if (is_comma_eligible_portlist_entry(port_kinds[pi]))
                        return true;
                }
                return false;
            };
            std::string port_lines;
            for (size_t k = 0; k < trimmed_ports.size(); ++k) {
                std::string port = trimmed_ports[k];
                std::string comma =
                    (is_comma_eligible_portlist_entry(port_kinds[k]) &&
                     (has_later_port(k) || opts.port_declaration.align))
                        ? ","
                        : "";
                if (!comma.empty()) {
                    size_t comment = find_line_comment_start(port);
                    if (comment != std::string::npos)
                        port = trim_right_copy(port.substr(0, comment)) + comma + " " +
                               trim_left_copy(port.substr(comment));
                    else
                        port += comma;
                }
                port_lines += port_indent + port + "\n";
            }
            if (!port_lines.empty() && port_lines.back() == '\n')
                port_lines.pop_back();
            if (opts.port_declaration.align) {
                auto port_flines = align_port_pass(text_to_lines(port_lines), opts);
                port_lines = render_lines(port_flines);
                std::vector<std::string> aligned_lines;
                {
                    std::istringstream ss(port_lines);
                    std::string l;
                    while (std::getline(ss, l)) {
                        while (true) {
                            std::string before = l;
                            erase_extra_comma_before_line_comment(l);
                            bool changed = l != before;
                            if (!changed)
                                break;
                        }
                        normalize_trailing_comma_spacing(l);
                        aligned_lines.push_back(l);
                    }
                }
                for (size_t ai = aligned_lines.size(); ai > 0; --ai) {
                    std::string& l = aligned_lines[ai - 1];
                    if (is_standalone_comment_line(l))
                        continue;
                    remove_last_code_comma(l);
                    break;
                }
                port_lines.clear();
                for (size_t ai = 0; ai < aligned_lines.size(); ++ai) {
                    if (ai)
                        port_lines += '\n';
                    port_lines += aligned_lines[ai];
                }
                auto port_toks = significant_tokens(port_lines);
                for (size_t ti = port_toks.size(); ti > 0; --ti) {
                    const auto& tok = port_toks[ti - 1];
                    if (tok_is(tok, ",", TokenKind::Comma)) {
                        port_lines.erase((size_t)tok.pos, tok.text.size());
                        break;
                    }
                    if (!tok.comment)
                        break;
                }
                while (!port_lines.empty() &&
                       (port_lines.back() == ' ' || port_lines.back() == '\t'))
                    port_lines.pop_back();
            }
            new_lines_str = prefix + "\n" + port_lines + "\n" + leading_ws + suffix_str;
        } else {
            // Check all are simple identifiers.
            bool all_simple = true;
            for (auto& p : trimmed_ports) {
                if (!is_simple_identifier_tokenized(p)) {
                    all_simple = false;
                    break;
                }
            }
            if (!all_simple) {
                push_original_lines(i, consumed_end);
                i = consumed_end;
                continue;
            }

            std::string port_block;
            if (opts.module.non_ansi_port_per_line_enabled && opts.module.non_ansi_port_per_line > 0) {
                int n = opts.module.non_ansi_port_per_line;
                for (size_t gi = 0; gi < trimmed_ports.size(); gi += (size_t)n) {
                    size_t end_g = std::min(gi + (size_t)n, trimmed_ports.size());
                    std::string comma = (end_g < trimmed_ports.size()) ? "," : "";
                    std::string grp_line = port_indent;
                    for (size_t k = gi; k < end_g; ++k) {
                        if (k > gi)
                            grp_line += ", ";
                        grp_line += trimmed_ports[k];
                    }
                    grp_line += comma;
                    port_block += grp_line + "\n";
                }
            } else if (opts.module.non_ansi_port_max_line_length_enabled &&
                       opts.module.non_ansi_port_max_line_length > 0) {
                int max_len = opts.module.non_ansi_port_max_line_length;
                std::vector<std::string> current;
                for (size_t pi = 0; pi < trimmed_ports.size(); ++pi) {
                    std::string candidate = port_indent;
                    for (size_t k = 0; k < current.size(); ++k) {
                        if (k)
                            candidate += ", ";
                        candidate += current[k];
                    }
                    candidate += ", " + trimmed_ports[pi];
                    if (!current.empty() && (int)candidate.size() > max_len) {
                        std::string row = port_indent;
                        for (size_t k = 0; k < current.size(); ++k) {
                            if (k)
                                row += ", ";
                            row += current[k];
                        }
                        row += ",";
                        port_block += row + "\n";
                        current = {trimmed_ports[pi]};
                    } else {
                        current.push_back(trimmed_ports[pi]);
                    }
                }
                if (!current.empty()) {
                    std::string row = port_indent;
                    for (size_t k = 0; k < current.size(); ++k) {
                        if (k)
                            row += ", ";
                        row += current[k];
                    }
                    port_block += row + "\n";
                }
            } else {
                for (size_t k = 0; k < trimmed_ports.size(); ++k) {
                    std::string comma = (k + 1 < trimmed_ports.size()) ? "," : "";
                    port_block += port_indent + trimmed_ports[k] + comma + "\n";
                }
            }
            if (!port_block.empty() && port_block.back() == '\n')
                port_block.pop_back();
            new_lines_str = prefix + "\n" + port_block + "\n" + leading_ws + suffix_str;
        }

        // Split new_lines_str by \n and push each line
        auto new_flines = text_to_lines(new_lines_str);
        for (auto& fl : new_flines)
            out.push_back(std::move(fl));
        i = consumed_end;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Formatter pipeline phases
// ---------------------------------------------------------------------------
//
// The formatter is organized as:
//   Phase 1: token-level spacing, line breaks, and indentation   (token loop)
//   Phase 2: structural layout / reflow of multiline constructs  (post-token)
//   Phase 3: line-group alignment                                (post-token)
//
// Post-token passes operate on plain line vectors.  Passes that need syntactic
// roles classify the relevant lines locally from lexer tokens.

static std::vector<std::string> run_module_header_layout_phase(std::vector<std::string> lines,
                                                                 const FormatOptions& opts) {
    lines = format_class_extends_parameter_pass(std::move(lines), opts);
    return format_portlist_pass(std::move(lines), opts);
}

static std::vector<std::string> run_line_group_alignment_phase(std::vector<std::string> lines,
                                                                 const FormatOptions& opts) {
    if (opts.statement.align)
        lines = align_assign_pass(std::move(lines), opts);
    if (opts.var_declaration.align)
        lines = align_var_pass(std::move(lines), opts);
    if (opts.port_declaration.align)
        lines = align_port_pass(std::move(lines), opts);
    return lines;
}

// ---------------------------------------------------------------------------
// Function/task declaration formatting pass
// ---------------------------------------------------------------------------
static std::vector<std::string> format_function_declaration_pass(
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
        size_t indent_end = (size_t)toks[0].pos;
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
        size_t open = (size_t)toks[open_i].pos;
        size_t close = (size_t)toks[close_i].pos;
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
            if (tok.comment)
                break;
            if (tok.whitespace || tok.directive)
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
                size_t rest = (size_t)tok.pos + tok.text.size();
                while (rest < line.size() && (line[rest] == ' ' || line[rest] == '\t'))
                    ++rest;
                if (rest < line.size() && line.compare(rest, 2, "//") != 0 &&
                    line.compare(rest, 2, "/*") != 0)
                    split_points.push_back((size_t)tok.pos + tok.text.size());
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

static std::vector<std::string> format_covergroup_pass(std::vector<std::string> lines,
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
                    open_pos = (size_t)toks[ti].pos;
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
                if (!tok.whitespace && !tok.comment)
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

static std::vector<std::string> format_constraint_dist_pass(std::vector<std::string> lines,
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
                if (tok.whitespace || tok.comment || tok.directive)
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

        size_t open_pos = (size_t)toks[open_i].pos;
        size_t close_pos = (size_t)toks[close_i].pos;
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

static std::vector<std::string> run_structural_layout_phase(
    std::vector<std::string> lines, const FormatOptions& opts) {
    PPContext pp = build_pp_context(lines);
    lines = format_enum_declaration_pass(std::move(lines), opts);
    lines = format_modport_pass(std::move(lines), opts);
    if (opts.instance.align)
        lines = expand_instances_pass(std::move(lines), opts);
        for (int i = 0  ;  i <lines.size() ; ++i)
    pp = build_pp_context(lines);
    lines = format_pp_conditional_function_calls_pass(std::move(lines), opts, pp);
    lines = format_multiline_macro_arg_calls_pass(std::move(lines), opts);
    lines = format_function_calls_pass(std::move(lines), opts);
    lines = text_to_lines(render_lines(lines));
    lines = format_covergroup_pass(std::move(lines), opts);
    lines = format_constraint_dist_pass(std::move(lines), opts);
    lines = split_semicolon_statements_pass(std::move(lines));
    return lines;
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
            if (tok.whitespace)
                continue;
            std::string text = tok.text;
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.pop_back();
            return !text.empty() && text.back() == '\\';
        }
        return false;
    };
    auto content_width = [](const std::string& ln) -> size_t {
        size_t e = 0;
        for (const auto& tok : collect_lexer_tokens(ln)) {
            if (!tok.whitespace)
                e = (size_t)tok.pos + tok.text.size();
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
        if (in_disabled(line_starts[i], disabled) || line_has_pp_conditional(lines[i]) ||
            !ends_with_bs(lines[i])) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        size_t start = i;
        while (i < lines.size() && !in_disabled(line_starts[i], disabled) &&
               !line_has_pp_conditional(lines[i]) && ends_with_bs(lines[i]))
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

static std::vector<std::string> run_post_token_pipeline(std::vector<std::string> lines,
                                                          const FormatOptions& opts) {
    auto debug_dump = [&](const char* pass_name, const std::vector<std::string>& pass_lines) {
        write_log(opts, std::string(pass_name) + ".sv", render_lines(pass_lines) + "\n");
    };

    // Post-token pipeline order:
    // 1. Module header layout can reshape module declarations before broad
    //    alignment sees those lines.
    // 2. Line-group alignment handles local declaration/assignment groups.
    // 3. Structural layout expands instances, calls, covergroups, constraints,
    //    and semicolon-separated statements.
    // 4. Function/task declaration layout runs after structural call/layout
    //    changes so declarations get their final multi-line shape.
    // 5. `define continuation alignment runs last because macro bodies should
    //    not be reflowed by earlier structural passes.
    lines = run_module_header_layout_phase(std::move(lines), opts);
    debug_dump("01_run_module_header_layout_phase", lines);
    lines = run_line_group_alignment_phase(std::move(lines), opts);
    debug_dump("02_run_line_group_alignment_phase", lines);
    lines = run_structural_layout_phase(std::move(lines), opts);
    debug_dump("03_run_structural_layout_phase", lines);
    lines = format_function_declaration_pass(std::move(lines), opts);
    debug_dump("04_format_function_declaration_pass", lines);
    lines = align_define_continuation_pass(std::move(lines), opts);
    debug_dump("05_align_define_continuation_pass", lines);
    return lines;
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// format_source — main entry point
// ---------------------------------------------------------------------------

std::string format_source(const std::string& source, const FormatOptions& opts) {
    ScopedTokenCache token_cache;

    // -----------------------------------------------------------------------
    // STEP 1: Tokenize using slang::Lexer (no SyntaxTree/CST needed)
    // -----------------------------------------------------------------------
    const std::string& input = source;
    write_log(opts, "main_token_loop_input.sv", input);

    // -----------------------------------------------------------------------
    // STEP 3: Build helper data structures
    //
    // indent_unit  — the string for one indent level, e.g. "    " (4 spaces).
    //
    // disabled     — list of [start, end) byte ranges that are inside
    //                1. `// verilog_format: off` … `// verilog_format: on`
    //                regions.  Tokens inside these ranges are copied verbatim.
    //                2. multi-line ``define` macros
    //
    // tokens       — ordered list of Tok structs extracted from the Lexer.
    //                Each Tok has: text, pos (byte offset), and bool flags
    //                (whitespace, comment, directive, keyword, etc.).
    // -----------------------------------------------------------------------
    const std::string indent_unit(opts.indent_size, ' ');
    auto disabled = find_disabled(input);
    auto tokens = collect_lexer_tokens(input);

    const MacroClassifier macro_classifier(opts.macros);

    // -----------------------------------------------------------------------
    // STEP 6: State variables for the token-walking loop
    //
    // The main loop below walks every token exactly once and builds `out` —
    // the formatted output string.  All the variables below track the
    // "current state of the world" as we advance through the token stream.
    // -----------------------------------------------------------------------

    std::string out;
    out.reserve(input.size() + input.size() / 4); // pre-allocate ~25% extra

    // indent_level — current nesting depth (in units of indent_unit).
    // indent_stack — stack of how much each indent-opening keyword added, so
    //               the matching indent-closing keyword can pop the exact same amount.
    //               (outmost module/interface/package blocks add
    //                opts.default_indent_level_inside_outmost_block, everything else adds 1.)
    int indent_level = 0;
    std::vector<int> indent_stack;

    // at_bol    — true when the next character we emit starts a fresh line,
    //             so we must emit the indentation prefix first.
    bool at_bol = true;

    // dim_depth — how many unmatched '[' we have seen since the last ';'.
    //             Nonzero means we are inside an array dimension like [7:0].
    //             Spacing rules differ inside dimensions.
    int dim_depth = 0;

    // paren_depth — how many unmatched '(' we have seen.
    //               Used to distinguish top-level ';' (statement end) from
    //               ';' inside a for(;;) header.
    int paren_depth = 0;
    std::vector<ParenKind> paren_stack;
    std::vector<bool> for_header_stack;

    // do_depth  — counts nested `do` keywords so we know when `end while`
    //             belongs to a do…while (MustAppend) vs. a plain `while`.
    int do_depth = 0;

    // pending_nl — true means we want a newline BEFORE the next token, but
    //              haven't emitted it yet.  Deferred so that inline comments
    //              can suppress it if they live on the same source line.
    bool pending_nl = false;
    bool original_newline_before_token = false;

    // blank_pend — number of extra blank lines to insert before the next
    //              token (capped at opts.blank_lines_between_items).
    int blank_pend = 0;

    // in_pp_cond — true while we are inside a preprocessor conditional that
    //              takes an argument on the same line, e.g. `ifdef MACRO_NAME.
    //              We need a newline after the argument.
    bool in_pp_cond = false;

    // after_dis  — true immediately after we exit a disabled region.
    //              If the next whitespace token contains a newline, we must
    //              emit pending_nl so the re-enabled code starts on its own line.
    bool after_dis = false;
    bool in_define_disabled = false;

    // block_label_state — tracks `begin: label` / `fork: label` sequences.
    //   0 = idle, 1 = just emitted begin/fork (expecting ':'), 2 = saw ':'
    //   (expecting label name).  Used to keep `begin: label` on one line.
    int block_label_state = 0;

    // struct_pend — true after we see `struct` or `union` keyword; the next
    //               `{` should be treated as a struct-brace (indented block)
    //               rather than a concatenation brace.
    bool struct_pend = false;
    bool constraint_pend = false;
    int constraint_depth = 0;

    // case_expr_pending / case_expr_depth
    //   After `case`/`casex`/`casez` we expect a `(expr)`.
    //   pending goes true at the keyword; depth records the paren nesting level
    //   of the opening `(`.  When we see the matching `)`, we schedule a newline
    //   (the case items start on the next line).
    bool case_expr_pending = false;
    int case_expr_depth = -1;
    int case_depth = 0;
    int case_conditional_depth = 0;
    bool case_label_pending_nl = false;

    // control_expr_pending / control_expr_depth
    //   Same idea for `if`/`for`/`foreach`/`while`/`repeat` — after the closing
    //   `)` of the condition, we set single_stmt_pending so the body gets
    //   an extra indent level if it has no `begin`.
    bool control_expr_pending = false;
    int control_expr_depth = -1;

    // single_stmt_pending — set after the `)` of an if/for/while condition.
    //                       If the very next token at BOL is NOT `begin`, we
    //                       bump indent_level by 1 (single_stmt_active = true).
    // single_stmt_active  — true while we are inside a bodyless single-statement
    //                       block; reset (and indent_level decremented) at `;`.
    bool single_stmt_pending = false;
    bool single_stmt_active = false;

    // do_while_tail — true for the `while` token that closes a `do…while`.
    //                 Prevents that `while` from being treated as a loop header
    //                 (which would set control_expr_pending again).
    bool do_while_tail = false;

    // Import/export declarations can contain `function` / `task` keywords, but
    // those keywords are part of the declaration and do not open SV blocks.
    bool in_import_export_decl = false;
    bool in_extern_decl = false;
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

    // brace_stk — stack that records whether each open `{` is a struct/union
    //             brace (value "struct") or something else ("other").
    //             Only struct braces trigger indentation.
    std::vector<std::string> brace_stk;

    // prev — pointer to the previous non-whitespace Tok, used by spaces_req()
    //        and break_dec() to decide spacing/line-break between token pairs.
    const Tok* prev = nullptr;
    bool prev_at_procedural = false;

    // -----------------------------------------------------------------------
    // Helper lambdas
    //
    // flush_nl() — actually writes the deferred newlines to `out`.
    //              Called just before emitting a token (unless MustAppend).
    //
    // emit(text) — writes `text` to `out`, prepending indentation if we are
    //              at the beginning of a line (at_bol == true).
    // -----------------------------------------------------------------------
    auto flush_nl = [&]() {
        if (pending_nl) {
            out += '\n';
            at_bol = true;
            pending_nl = false;
        }
        if (blank_pend > 0) {
            if (!at_bol) {
                out += '\n';
                at_bol = true;
                }
            for (int k = 0; k < blank_pend; ++k) {
                out += '\n';
                at_bol = true;
                }
            blank_pend = 0;
        }
    };
    auto emit = [&](const std::string& text) {
        if (at_bol) {
            for (int k = 0; k < indent_level; ++k)
                out += indent_unit;
            at_bol = false;
        }
        out += text;
    };
    auto next_significant = [&](size_t idx) -> const Tok* {
        for (size_t j = idx + 1; j < tokens.size(); ++j) {
            if (!tokens[j].whitespace && !tokens[j].comment)
                return &tokens[j];
        }
        return nullptr;
    };
    auto macro_force_own_line = [](MacroRole role) {
        return role == MacroRole::DeclarationLike || role == MacroRole::ControlFlowLike ||
               role == MacroRole::BlockBeginLike || role == MacroRole::BlockEndLike;
    };
    auto macro_newline_after = [](MacroRole role) {
        return role == MacroRole::StatementLike || role == MacroRole::DeclarationLike ||
               role == MacroRole::ControlFlowLike || role == MacroRole::BlockBeginLike ||
               role == MacroRole::BlockEndLike;
    };
    auto finish_macro_invocation = [&](const MacroClassification& classification) {
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

    // -----------------------------------------------------------------------
    // STEP 7: Main token loop — single pass over all tokens
    //
    // For each token we:
    //   A. Skip / passthrough disabled-region tokens verbatim.
    //   B. Skip whitespace tokens (we reconstruct spacing from scratch).
    //   C. Decide spacing (spaces_req) and line-break (break_dec) from the
    //      previous token.
    //   D. Handle inline comments (don't let pending_nl split them off).
    //   E. Flush pending newlines or append-space depending on break decision.
    //   F. Handle single-statement indent (no `begin` after if/for/while).
    //   G. Decrement indent BEFORE emitting indent-closing keywords (end, endmodule, …)
    //      so the closing keyword aligns with the matching open keyword.
    //   H. Emit the token text.
    //   I. Record orig→fmt line mapping.
    //   J. Update bracket/paren/dim depth counters.
    //   K. Post-emit: open new indent level, schedule newlines, track pp-conds.
    // -----------------------------------------------------------------------
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Tok& tok = tokens[i];
        bool tok_is_macro = is_macro_usage(tok);
        MacroClassification tok_macro_class;
        bool tok_macro_has_args = false;
        if (tok_is_macro) {
            if (const Tok* next = next_significant(i))
                tok_macro_has_args = tok_is(*next, "(", TokenKind::OpenParenthesis);
            tok_macro_class = classify_macro(tok, tok_macro_has_args, macro_classifier);
        }
        syntax::SyntaxKind tok_pp_cond_kind = syntax::SyntaxKind::Unknown;
        if (is_line_directive(tok) && is_pp_conditional(tok.directive_kind))
            tok_pp_cond_kind = tok.directive_kind;
        else if (is_pp_conditional_text(tok.text))
            tok_pp_cond_kind = directive_at_offset(tok.text, 0).kind;
        bool tok_is_pp_conditional = is_pp_conditional(tok_pp_cond_kind);

        if (whitespace_macro_passthrough) {
            out += tok.text;
            at_bol = !tok.text.empty() && tok.text.back() == '\n';
            if (!tok.whitespace && !tok.comment) {
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
            flush_nl();
            emit(tok.text);
            whitespace_macro_passthrough = true;
            whitespace_macro_seen_open = false;
            whitespace_macro_paren_depth = 0;
            whitespace_macro_class = tok_macro_class;
            whitespace_macro_prev = tok;
            prev_macro_role_valid = false;
            original_newline_before_token = false;
            continue;
        }

        // --- A. Disabled region — pass through verbatim ---
        // Tokens between `// verilog_format: off` and `// verilog_format: on`
        // are copied to `out` exactly as-is, with no reformatting at all.
        // We still flush any pending newline first so the disabled block starts
        // on a fresh line.  Define bodies and preprocessor conditionals are
        // appended raw instead of using emit(), so they normalize to column 0
        // rather than inheriting formatter indentation.  `prev` is NOT updated
        // so the token before the disabled region doesn't influence spacing
        // after it.
        if (in_disabled(tok.pos, disabled)) {
            flush_nl();
            if (is_line_directive(tok) && tok.directive_kind == syntax::SyntaxKind::DefineDirective)
                in_define_disabled = true;
            if (tok_is_pp_conditional || in_define_disabled || !at_bol || tok.whitespace) {
                out += tok.text;
            } else {
                emit(tok.text);
            }
            at_bol = !tok.text.empty() && tok.text.back() == '\n';
            after_dis = !at_bol; // remember we just left a disabled region mid-line
            continue;            // don't update prev
        }
        in_define_disabled = false;

        // --- B. Whitespace tokens — discard, but extract blank-line info ---
        // We never copy original whitespace; spacing is completely rewritten.
        // However, we do respect blank lines between items up to the configured
        // maximum (opts.blank_lines_between_items).
        if (tok.whitespace) {
            int nl = (int)std::count(tok.text.begin(), tok.text.end(), '\n');
            original_newline_before_token = nl > 0;
            // If a disabled region ended mid-line and now we see a newline in
            // the following whitespace, emit the pending newline.
            if (after_dis && nl >= 1)
                pending_nl = true;
            after_dis = false;
            if (nl > 0 && paren_depth > 0)
                continue;
            if (nl > 1) {
                // More than one newline = at least one blank line between items.
                // extra = number of blank lines (nl-1 because first \n ends the
                // current line, subsequent \n's are the blank lines themselves).
                int extra = std::min(nl - 1, opts.blank_lines_between_items);
                blank_pend = std::max(blank_pend, extra);
            }
            continue;
        }

        if (tok_is_pp_conditional) {
            flush_nl();
            if (!at_bol) {
                out += '\n';
                at_bol = true;
            }
            out += tok.text;
            at_bol = !tok.text.empty() && tok.text.back() == '\n';
            pending_nl = false;
            blank_pend = 0;

            bool has_inline_arg = false;
            if (is_pp_cond_with(tok_pp_cond_kind)) {
                auto directive = directive_at_offset(tok.text, 0);
                size_t arg = directive.end;
                while (arg < tok.text.size() && (tok.text[arg] == ' ' || tok.text[arg] == '\t'))
                    ++arg;
                has_inline_arg = arg < tok.text.size() && tok.text[arg] != '\n';
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
            if (!at_bol)
                out += ' ';
            out += tok.text;
            at_bol = !tok.text.empty() && tok.text.back() == '\n';
            in_pp_cond = false;
            pending_nl = true;
            prev_macro_role_valid = false;
            prev = &tok;
            original_newline_before_token = false;
            continue;
        }

        // --- C. Decide spacing and line-break between prev and tok ---
        // in_dim: inside array dimension brackets [hi:lo], spacing is tighter.
        // spaces: how many spaces to insert between prev and tok (0 or 1).
        // dec:    SD::MustWrap   → force a newline before tok
        //         SD::MustAppend → force tok onto the same line as prev
        //         SD::Undecided  → use pending_nl / blank_pend to decide
        bool in_dim = dim_depth > 0;
        bool in_for_header =
            std::any_of(for_header_stack.begin(), for_header_stack.end(), [](bool v) { return v; });
        bool procedural_at = prev && ((tok.text == "@" && is_procedural_event_keyword(*prev)) ||
                                      (prev->text == "@" && prev_at_procedural));
        ParenKind paren_kind = ParenKind::Ordinary;
        if (prev && (prev->text == "(" || tok_is(tok, ")", TokenKind::CloseParenthesis))) {
            if (!paren_stack.empty())
                paren_kind = paren_stack.back();
        } else if (prev && tok_is(tok, "(", TokenKind::OpenParenthesis))
            paren_kind = classify_paren(*prev, procedural_at);
        int spaces = 0;
        SD dec = SD::Undecided;
        if (prev) {
            spaces = spaces_req(*prev, tok, opts, in_dim, in_for_header, !paren_stack.empty(),
                                paren_kind, procedural_at);
            dec = break_dec(*prev, tok, opts, in_dim);
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
        // A `;` sets pending_nl=true (statement ended → new line).
        // But if the very next token is a comment that lives on the same
        // original source line (e.g. `foo; // comment`), we must NOT emit
        // the newline yet — the comment must stay on the same line as `foo;`.
        bool inline_comment = tok.comment && prev && !original_newline_before_token;

        // --- Special: `end while` in a do…while statement ---
        // In `do begin … end while (cond);` the `while` after `end` is NOT
        // the start of a new while-loop — it is the tail of the do…while.
        // We must append it to the same line as `end` (MustAppend) and must
        // NOT treat it as a new control-expression header.
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
        if (macro_wrap_pending && !tok.whitespace && !tok.comment) {
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
        // In `disable <block_name>;`, the token after `disable` names the
        // target block. It must not be treated as a structural keyword even
        // when the target is `fork`.
        bool disable_target = prev && prev->kind == TokenKind::DisableKeyword;
        bool wait_fork_target = prev && prev->kind == TokenKind::WaitKeyword &&
                                tok.kind == TokenKind::ForkKeyword;

        // --- Block label: keep `begin: label` on one line ---
        if (block_label_state == 1 && !tok.whitespace && !tok.comment) {
            if (tok.text == ":") {
                dec = SD::MustAppend;
                spaces = 0;
                block_label_state = 2;
            } else {
                block_label_state = 0;
            }
        } else if (block_label_state == 2 && !tok.whitespace && !tok.comment) {
            dec = SD::MustAppend;
        }
        if (case_label_pending_nl && tok.kind == TokenKind::BeginKeyword)
            case_label_pending_nl = false;

        // Suppress the pending newline if this comment is on the same source line.
        if (inline_comment && pending_nl) {
            if (!case_label_pending_nl)
                pending_nl = false;
        } else if (tok.comment && tok.text.rfind("//", 0) == 0) {
            size_t line_start = input.rfind('\n', (size_t)tok.pos);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            bool standalone_comment = true;
            for (size_t p = line_start; p < (size_t)tok.pos; ++p) {
                if (input[p] != ' ' && input[p] != '\t') {
                    standalone_comment = false;
                    break;
                }
            }
            if (standalone_comment)
                dec = SD::MustWrap;
        }

        // --- E. Emit newline / spacing based on break decision ---
        if (dec == SD::MustWrap) {
            // Force newline before this token regardless of pending_nl.
            pending_nl = false;
            if (!at_bol) {
                out += '\n';
                at_bol = true;
                }
            for (int k = 0; k < blank_pend; ++k) {
                out += '\n';
                }
            blank_pend = 0;
        } else if (dec == SD::MustAppend) {
            // Force this token onto the same line — cancel any pending newline.
            if (pending_nl) {
                pending_nl = false;
                blank_pend = 0;
            }
            if (!at_bol && spaces > 0)
                out += std::string(spaces, ' ');
        } else {
            // Normal: flush deferred newlines first, then add spaces.
            flush_nl();
            if (!at_bol && spaces > 0)
                out += std::string(spaces, ' ');
        }

        // --- F. Single-statement indent (bodyless if/for/while body) ---
        // single_stmt_pending is set after the `)` closing an if/for/while
        // condition.  We wait until we are truly at the start of a new line
        // (at_bol) to decide:
        //   • If the next token is `begin` → no extra indent needed (begin
        //     will open its own indented block as an indent-opening keyword).
        //   • Otherwise → bump indent_level by 1 for this single statement.
        if (single_stmt_pending && at_bol) {
            if (constraint_depth > 0 && tok_is(tok, "{", TokenKind::OpenBrace)) {
                // Constraint if/else bodies use braces instead of begin/end; let the
                // brace handler below create the indentation scope.
            } else if (tok.kind == TokenKind::BeginKeyword) {
                single_stmt_pending = false; // begin handles its own indent
            } else {
                ++indent_level;
                single_stmt_pending = false;
                single_stmt_active = true; // remember to undo at the next `;`
            }
        }

        // --- G. Indent-close: decrement indent BEFORE emitting the token ---
        // Keywords like `end`, `endmodule`, `endfunction`, `join`, etc. are
        // indent-closing keywords.  They should appear at the SAME indentation level
        // as the matching open keyword, so we reduce indent_level first.
        //
        // Closing `}` of a struct/union block works the same way — the `}`
        // should align with the `struct {` line.
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
                   (brace_stk.back() == "struct" || brace_stk.back() == "constraint")) {
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        }

        // --- H. Emit the token ---
        // Tokens are emitted verbatim. emit() prepends the indentation string if at_bol is true.
        emit(tok.text);

        // --- J. Update bracket/paren/dim depth counters ---
        // dim_depth tracks `[…]` nesting for array dimensions.
        // paren_depth tracks `(…)` nesting.
        // When we see `(` after case/if/for/while, record the depth so we
        // know which `)` closes the expression.
        // `;` resets dim_depth because dimensions never span statements.
        if (tok_is(tok, "[", TokenKind::OpenBracket))
            ++dim_depth;
        else if (tok_is(tok, "]", TokenKind::CloseBracket) && dim_depth > 0)
            --dim_depth;
        else if (tok_is(tok, "(", TokenKind::OpenParenthesis)) {
            ++paren_depth;
            paren_stack.push_back(prev ? classify_paren(*prev, prev_at_procedural)
                                       : ParenKind::Ordinary);
            for_header_stack.push_back(prev && (prev->kind == TokenKind::ForKeyword ||
                                                prev->kind == TokenKind::ForeachKeyword));
            if (!active_macros.empty() && active_macros.back().wait_open && prev &&
                is_macro_usage(*prev)) {
                active_macros.back().paren_depth = paren_depth;
                active_macros.back().wait_open = false;
            }
            if (case_expr_pending) {
                case_expr_depth = paren_depth; // remember which ')' ends the case expr
                case_expr_pending = false;
            }
            if (control_expr_pending) {
                control_expr_depth = paren_depth; // remember which ')' ends the control expr
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
                // Closing `)` of `case (expr)` → next token goes on a new line
                pending_nl = true;
                case_expr_depth = -1;
            }
            if (control_expr_depth == paren_depth) {
                // Closing `)` of `if (cond)` / `for (…)` / etc.
                // → schedule a newline so the body starts on a fresh line.
                // `if (cond) begin` suppresses this via MustAppend in break_dec.
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
            dim_depth = 0; // `;` always ends any dimension context

        // --- K. Post-emit housekeeping ---
        // After emitting a token, update state for the NEXT token:
        //   • Open new indent levels for indent-opening keywords.
        //   • Schedule newlines (pending_nl) after block-opening keywords, `;`, etc.
        //   • Set flags for upcoming special tokens (case expr, control expr, struct).
        if (tok_is_macro) {
            if (tok_macro_has_args) {
                active_macros.push_back({tok_macro_class, true, -1});
            } else {
                finish_macro_invocation(tok_macro_class);
            }
        } else if (is_keyword(tok)) {
            // Count `do` depth so `end while` can be recognized as do…while tail.
            if (tok.kind == TokenKind::DoKeyword)
                ++do_depth;

            if (tok.kind == TokenKind::ImportKeyword || tok.kind == TokenKind::ExportKeyword)
                in_import_export_decl = true;
            if (tok.kind == TokenKind::ExternKeyword)
                in_extern_decl = true;

            // After `case`/`casex`/`casez`, expect `(expr)`.
            // When the `)` arrives we'll emit a newline before the case items.
            if (tok.kind == TokenKind::CaseKeyword || tok.kind == TokenKind::CaseXKeyword ||
                tok.kind == TokenKind::CaseZKeyword) {
                case_expr_pending = true;
                ++case_depth;
                case_conditional_depth = 0;
            }

            // After `if`/`for`/`foreach`/`while`/`repeat`, expect `(cond)`.
            // When the `)` arrives we'll decide single-statement indentation.
            // `while` that closes a do…while (do_while_tail) is excluded.
            if (tok.kind == TokenKind::IfKeyword || tok.kind == TokenKind::ForKeyword ||
                tok.kind == TokenKind::ForeachKeyword ||
                (tok.kind == TokenKind::WhileKeyword && !do_while_tail) ||
                tok.kind == TokenKind::RepeatKeyword)
                control_expr_pending = true;

            // `else` bodies without begin/end should be formatted like other
            // single-statement control bodies.
            if (tok.kind == TokenKind::ElseKeyword) {
                single_stmt_pending = true;
                pending_nl = true;
            }

            // `begin` cancels single_stmt_pending set by a preceding if/for/while/else,
            // because begin…end is its own indented block.
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
                // Keywords that increase indentation for everything inside them.
                // Outmost design blocks use a configurable delta (default 1);
                // everything else adds exactly 1 level.
                int delta = (tok.kind == TokenKind::ModuleKeyword ||
                             tok.kind == TokenKind::MacromoduleKeyword ||
                             tok.kind == TokenKind::InterfaceKeyword ||
                             tok.kind == TokenKind::PackageKeyword)
                                ? opts.default_indent_level_inside_outmost_block
                                : 1;
                indent_level += delta;
                indent_stack.push_back(delta);
                // Block-opening keywords (begin, fork, …) also schedule a newline
                // so the first statement inside appears on the next line.
                // `case` is handled separately via case_expr_pending.
                if (is_block_open(tok.kind) && tok.kind != TokenKind::CaseKeyword &&
                    tok.kind != TokenKind::CaseXKeyword && tok.kind != TokenKind::CaseZKeyword)
                    pending_nl = true;
            } else if (is_indent_close(tok.kind)) {
                // After emitting `end`/`endmodule`/etc. schedule a newline so
                // the next statement starts on a fresh line.
                pending_nl = true;
            } else if (tok.kind == TokenKind::StructKeyword || tok.kind == TokenKind::UnionKeyword) {
                // Next `{` should be treated as a struct/union brace (indented).
                struct_pend = true;
            }
        } else if (is_open_group(tok) && tok_is(tok, "{", TokenKind::OpenBrace)) {
            if (struct_pend || constraint_pend || (constraint_depth > 0 && single_stmt_pending)) {
                bool is_constraint_brace = constraint_pend || (constraint_depth > 0 && single_stmt_pending);
                // `{` right after `struct`/`union`, `constraint`, or a constraint
                // control item body → indent the members inside.
                brace_stk.push_back(is_constraint_brace ? "constraint" : "struct");
                pending_nl = true;
                indent_level += 1;
                indent_stack.push_back(1);
                if (is_constraint_brace) {
                    ++constraint_depth;
                    constraint_pend = false;
                    single_stmt_pending = false;
                }
            } else {
                // `{` for concatenation, array literal, etc. → no extra indent.
                brace_stk.push_back("other");
            }
            struct_pend = false;
        } else if (is_close_group(tok) && tok_is(tok, "}", TokenKind::CloseBrace)) {
            // Pop the brace stack (indent was already decremented in step G).
            if (!brace_stk.empty()) {
                if (brace_stk.back() == "constraint") {
                    constraint_depth = std::max(0, constraint_depth - 1);
                    pending_nl = true;
                }
                brace_stk.pop_back();
            }
        } else if (tok_is(tok, ";", TokenKind::Semicolon)) {
            in_import_export_decl = false;
            in_extern_decl = false;
            // Semicolon = end of statement.
            // At top-level (paren_depth==0) schedule a newline after it.
            // Inside `for (init; cond; incr)` paren_depth > 0 → no newline.
            bool in_for_header_now =
                std::any_of(for_header_stack.begin(), for_header_stack.end(),
                            [](bool v) { return v; });
            if (paren_depth == 0 || (case_depth > 0 && !in_for_header_now))
                pending_nl = true;
            // If we were in a single-statement body (no begin…end), this `;`
            // ends it → undo the extra indent level we added.
            if (single_stmt_active) {
                indent_level = std::max(0, indent_level - 1);
                single_stmt_active = false;
            }
        } else if (is_line_directive(tok)) {
            // Compiler directives (`define, `include, `ifdef, …) always end
            // their logical line → schedule a newline after them.
            pending_nl = true;
            if (is_pp_cond_bare(tok.directive_kind) || is_pp_cond_with(tok.directive_kind))
                in_pp_cond = false;
        } else if (tok.comment) {
            // A comment that is followed by a newline in the original whitespace
            // → schedule a newline (the comment ends the line).
            if (i + 1 < tokens.size() && tokens[i + 1].whitespace &&
                tokens[i + 1].text.find('\n') != std::string::npos)
                pending_nl = true;
        } else if (tok.kind == TokenKind::Directive) {
            if (is_pp_cond_bare(tok.directive_kind)) {
                pending_nl = true;
                in_pp_cond = false;
            } else if (is_pp_cond_with(tok.directive_kind)) {
                in_pp_cond = true;
            }
        } else if (is_identifier(tok)) {
            // Preprocessor condition argument after a split `ifdef-style directive.
            if (in_pp_cond) {
                pending_nl = true;
                in_pp_cond = false;
            }
            if (is_macro_usage(tok) && tok.text.find(';') != std::string::npos) {
                pending_nl = true;
                if (single_stmt_active) {
                    indent_level = std::max(0, indent_level - 1);
                    single_stmt_active = false;
                }
            }
        } else if (in_pp_cond) {
            // Non-identifier token after a `ifdef-style directive → treat it as
            // the end of the argument and schedule a newline.
            pending_nl = true;
            in_pp_cond = false;
        }

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
        } else if (!tok.comment && !tok.whitespace) {
            case_label_pending_nl = false;
        }

        // --- Block label post-emit: restore newline after label name ---
        if (block_label_state == 2 && !tok.whitespace && !tok.comment && tok.text != ":") {
            pending_nl = true;
            block_label_state = 0;
        }
        if (!tok.whitespace && !tok.comment && is_keyword(tok) &&
            (tok.kind == TokenKind::BeginKeyword || tok.kind == TokenKind::ForkKeyword ||
             is_indent_close(tok.kind)) &&
            !disable_target && !wait_fork_target) {
            block_label_state = 1;
        }

        prev_macro_role_valid = tok_is_macro;
        if (tok_is_macro)
            prev_macro_role = tok_macro_class.role;
        prev_at_procedural = tok.text == "@" && prev && is_procedural_event_keyword(*prev);
        prev = &tok; // remember this token for next iteration's spacing decisions
        original_newline_before_token = false;
    } // end main token loop

    // -----------------------------------------------------------------------
    // STEP 8: Finalize raw output
    //
    // If the last token didn't end with a newline, add one.
    // Collapse multiple trailing newlines to a single one.
    // -----------------------------------------------------------------------
    if (!at_bol) {
        out += '\n';
    }

    // Collapse extra trailing newlines to one
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    write_log(opts, "main_token_loop_output.sv", out);

    // -----------------------------------------------------------------------
    // STEP 9: Convert to plain lines and run post-token pipeline.
    // Post-passes classify lines locally from lexer tokens when needed.
    // -----------------------------------------------------------------------
    auto lines = text_to_lines(out);
    lines = run_post_token_pipeline(std::move(lines), opts);
    out = render_lines(lines);

    // -----------------------------------------------------------------------
    // STEP 10: Final newline normalization
    //
    // Mirrors Python: result.rstrip('\n') + '\n'
    // -----------------------------------------------------------------------
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    out += '\n';

    // -----------------------------------------------------------------------
    // STEP 12: Safe-mode integrity check
    //
    // Verify that only whitespace changed.  This is a safety net against
    // formatter bugs that corrupt code.
    // -----------------------------------------------------------------------
    verify_safe_mode_unchanged(source, out, opts);

    return out;
}
