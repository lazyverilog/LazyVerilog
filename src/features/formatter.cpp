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
#include <regex>
#include <sstream>
#include <stdexcept>
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

enum class CommentKind {
    None,
    Line,
    Block,
};

// Stable, pre-pass comment classification.  Computed once in
// classify_comments_pass() from original source positions and stored
// immutably on each comment token.  All subsequent passes read this field
// instead of re-deriving attachment from whitespace (which would oscillate
// when formatted output has different spacing than the input).
enum class CommentRole {
    None,                  // not a comment, or inside a disabled/define region
    LeadingStatement,      // own-line comment logically attached to next syntax object
    TrailingStatement,     // true statement/construct-tail comment on same source line
    OwnLineInterstitial,   // own-line comment inside an expression/list/group
    TrailingInterstitial,  // same-line comment attached to previous list/expression element
    LeadingInterstitial,   // same-line comment attached to following list/expression element
    InlineInterstitial,    // embedded comment with syntax on both sides in same element
};

enum class LayoutOwner {
    None,
    Basic,
    Comment,
    DisabledRegion,
    DefineBlock,
    ModuleHeaderParameterList,
    ModuleHeaderPortList,
};

struct TokLexeme {
    TokenKind kind{TokenKind::Unknown};
    std::string text;
    std::string lo;
    int pos{0};
    bool whitespace{false};
    bool comment{false};
    CommentKind comment_kind{CommentKind::None};
    bool directive{false};
    bool define_block{false};
    syntax::SyntaxKind directive_kind{syntax::SyntaxKind::Unknown};
};

struct Tok {
    // Lexical identity is immutable after token collection. Formatting passes may
    // only update fmt_* metadata; token text itself is never rewritten.
    std::shared_ptr<const TokLexeme> lex;
    // --- Non-rendering pass metadata ---
    // Populated before basic_formatting() for structural facts reused by later
    // passes. These fields must not directly affect render_tokens(); rendering is
    // controlled only by fmt_* fields below.
    size_t matching_token{SIZE_MAX};
    size_t stmt_end{SIZE_MAX};
    bool is_pp_conditional_directive{false};
    bool in_pp_conditional_line_tail{false};
    bool stmt_has_pp_conditional{false};
    bool stmt_has_define_block{false};
    bool in_modport{false};
    bool in_covergroup{false};
    bool in_function_decl{false};
    bool in_task_decl{false};
    bool in_module_header{false};
    bool in_interface_header{false};
    bool in_program_header{false};
    bool in_class_decl{false};
    bool in_disabled_region{false};   // inside verilog_format/verilog-format off/on region
    LayoutOwner layout_owner{LayoutOwner::None};

    // --- Mutable formatting metadata (set/updated by passes) ---
    int fmt_indent{0};            // indentation level at this token
    int fmt_spaces_before{0};     // spaces before this token (within a line)
    std::string fmt_text_before;   // synthetic text emitted immediately before this token
    bool fmt_newline_before{false}; // emit newline before this token
    int fmt_blank_lines{0};       // blank lines before this token (after newline)
    bool fmt_passthrough{false};  // whitespace-sensitive macro (verbatim)

    // Pre-computed nesting depths at this token position (set by
    // populate_nonrender_metadata_pass, used by classify_comments_pass).
    int paren_depth{0};  // ( ) nesting depth
    int dim_depth{0};    // [ ] nesting depth

    // Stable comment classification (set by classify_comments_pass, read-only
    // for all subsequent passes).
    CommentRole comment_role{CommentRole::None};
    size_t      comment_owner{SIZE_MAX}; // token index of the anchor token
};

static const std::string& tok_text(const Tok& tok) {
    return tok.lex->text;
}
static TokenKind tok_kind(const Tok& tok) { return tok.lex->kind; }
static const std::string& tok_lo(const Tok& tok) { return tok.lex->lo; }
static int tok_pos(const Tok& tok) { return tok.lex->pos; }
static bool tok_whitespace(const Tok& tok) { return tok.lex->whitespace; }
static bool tok_comment(const Tok& tok) { return tok.lex->comment; }
static bool tok_line_comment(const Tok& tok) { return tok.lex->comment_kind == CommentKind::Line; }
static bool tok_block_comment(const Tok& tok) { return tok.lex->comment_kind == CommentKind::Block; }
static bool tok_directive(const Tok& tok) { return tok.lex->directive; }
static bool tok_define_block(const Tok& tok) { return tok.lex->define_block; }
static syntax::SyntaxKind tok_directive_kind(const Tok& tok) { return tok.lex->directive_kind; }

static bool comment_role_interstitial(CommentRole role) {
    return role == CommentRole::OwnLineInterstitial ||
           role == CommentRole::TrailingInterstitial ||
           role == CommentRole::LeadingInterstitial ||
           role == CommentRole::InlineInterstitial;
}

static bool comment_role_inline_rendered(CommentRole role) {
    return role == CommentRole::TrailingStatement ||
           role == CommentRole::TrailingInterstitial ||
           role == CommentRole::LeadingInterstitial ||
           role == CommentRole::InlineInterstitial;
}

static bool comment_role_own_line_rendered(CommentRole role) {
    return role == CommentRole::LeadingStatement ||
           role == CommentRole::OwnLineInterstitial;
}

static std::vector<Tok> collect_lexer_tokens(const std::string& source);
static std::string render_tokens(const std::vector<Tok>& tokens, const FormatOptions& opts);
static void align_port_pass(std::vector<Tok>& tokens, const FormatOptions& opts);
static void assign_layout_owners_pass(std::vector<Tok>& tokens);

static std::vector<size_t> tok_line_starts(const std::vector<Tok>& tokens);

static bool is_pp_cond_with(syntax::SyntaxKind kind);
static bool is_pp_conditional(syntax::SyntaxKind kind);


struct DirectiveMatch {
    syntax::SyntaxKind kind{syntax::SyntaxKind::Unknown};
    size_t start{std::string::npos};
    size_t end{std::string::npos};
};

struct DirectiveIndex {
    std::unordered_map<size_t, DirectiveMatch> by_start;

    explicit DirectiveIndex(const std::string& text) {
        for (const auto& tok : collect_lexer_tokens(text)) {
            if (tok_kind(tok) != TokenKind::Directive)
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
        if (tok_kind(tok) != TokenKind::Directive)
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


static bool is_macro_usage(const Tok& t) {
    return tok_kind(t) == TokenKind::MacroUsage ||
           (tok_kind(t) == TokenKind::Directive && tok_directive_kind(t) == syntax::SyntaxKind::MacroUsage);
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
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) && LexerFacts::isKeyword(tok_kind(t));
}

static bool is_constraint_keyword(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) &&
           tok_kind(t) == TokenKind::ConstraintKeyword;
}

static bool is_begin_keyword(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) && tok_kind(t) == TokenKind::BeginKeyword;
}

static bool is_else_keyword(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) && tok_kind(t) == TokenKind::ElseKeyword;
}

static bool is_identifier(const Tok& t) {
    return !tok_directive(t) && !tok_comment(t) && !tok_whitespace(t) &&
           (tok_kind(t) == TokenKind::Identifier || tok_kind(t) == TokenKind::SystemIdentifier ||
            (!tok_text(t).empty() &&
             (std::isalpha((unsigned char)tok_text(t)[0]) || tok_text(t)[0] == '_' || tok_text(t)[0] == '$' ||
              tok_text(t)[0] == '`') &&
             !is_keyword(t)));
}

static bool is_port_direction_token(const Tok& tok) {
    return tok_kind(tok) == TokenKind::InputKeyword || tok_kind(tok) == TokenKind::OutputKeyword ||
           tok_kind(tok) == TokenKind::InOutKeyword || tok_kind(tok) == TokenKind::RefKeyword;
}

static bool is_sign_qualifier_token(const Tok& tok) {
    return tok_kind(tok) == TokenKind::SignedKeyword || tok_kind(tok) == TokenKind::UnsignedKeyword;
}

static bool is_var_prefix_token(const Tok& tok) {
    return tok_kind(tok) == TokenKind::StaticKeyword || tok_kind(tok) == TokenKind::AutomaticKeyword ||
           tok_kind(tok) == TokenKind::ConstKeyword || tok_kind(tok) == TokenKind::VarKeyword;
}

static bool is_var_builtin_type_token(const Tok& tok) {
    return tok_kind(tok) == TokenKind::WireKeyword || tok_kind(tok) == TokenKind::LogicKeyword ||
           tok_kind(tok) == TokenKind::RegKeyword || tok_kind(tok) == TokenKind::BitKeyword ||
           tok_kind(tok) == TokenKind::ByteKeyword || tok_kind(tok) == TokenKind::IntKeyword ||
           tok_kind(tok) == TokenKind::IntegerKeyword || tok_kind(tok) == TokenKind::TimeKeyword ||
           tok_kind(tok) == TokenKind::ShortIntKeyword || tok_kind(tok) == TokenKind::LongIntKeyword ||
           tok_kind(tok) == TokenKind::SignedKeyword || tok_kind(tok) == TokenKind::UnsignedKeyword;
}

static bool is_function_call_skip_token(const Tok& tok) {
    switch (tok_kind(tok)) {
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


static bool is_numeric(const Tok& t) {
    return tok_kind(t) == TokenKind::IntegerLiteral || tok_kind(t) == TokenKind::IntegerBase ||
           tok_kind(t) == TokenKind::UnbasedUnsizedLiteral || tok_kind(t) == TokenKind::RealLiteral ||
           tok_kind(t) == TokenKind::TimeLiteral;
}

static bool is_open_group(const Tok& t) { return tok_text(t) == "(" || tok_text(t) == "[" || tok_text(t) == "{"; }

static bool is_close_group(const Tok& t) { return tok_text(t) == ")" || tok_text(t) == "]" || tok_text(t) == "}"; }

static bool is_hierarchy(const Tok& t) { return tok_text(t) == "." || tok_text(t) == "::"; }

static bool is_unary_op(const Tok& t) {
    switch (tok_kind(t)) {
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
    switch (tok_kind(t)) {
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
    switch (tok_kind(t)) {
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
    switch (tok_kind(t)) {
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
    if (tok_kind(t) == TokenKind::LessThanEquals && in_parens)
        return false;
    switch (tok_kind(t)) {
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
// Format-disabled markers
// ---------------------------------------------------------------------------

static bool is_format_control_comment(const Tok& tok, const std::regex& pattern) {
    if (!tok_line_comment(tok))
        return false;
    return std::regex_search(tok_text(tok), pattern);
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
    if (is_indent_close(tok_kind(L)) && rx == ":")
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
    if (tok_kind(L) == TokenKind::InsideKeyword || tok_kind(R) == TokenKind::InsideKeyword)
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
        if (tok_kind(L) == TokenKind::WaitKeyword)
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
        if (tok_kind(L) == TokenKind::DefaultKeyword)
            return 0;
        if (in_dim)
            return 0;
        if (is_identifier(L) || is_numeric(L) || is_close_group(L))
            return 0;
        return 1;
    }
    if (lx == "}")
        return 1;
    if (tok_kind(R) == TokenKind::OpenBrace)
        return (is_keyword(L) || is_identifier(L) || tok_kind(L) == TokenKind::CloseParenthesis ||
                tok_kind(L) == TokenKind::CloseBracket) ? 1 : 0;
    if (rx == "[") {
        if (lx == "]")
            return 0;
        if (is_type_keyword(tok_kind(L)))
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
    // A multiline block comment forces a wrap unless it is interstitial (inside
    // parens/brackets), where inline placement is valid.  The old check tested
    // whether R was a block comment — a right-neighbor heuristic that could
    // produce different results depending on what followed across passes.
    if (tok_block_comment(L) && tok_text(L).find('\n') != std::string::npos &&
        !comment_role_interstitial(L.comment_role))
        return SD::MustWrap;
    if (tok_line_comment(L))
        return SD::MustWrap;
    if (is_unary_op(L))
        return SD::MustAppend;
    if (is_indent_close(tok_kind(L)) && rx == ":")
        return SD::MustAppend;
    if (is_indent_close(tok_kind(R)))
        return SD::MustWrap;
    if (is_else_keyword(R)) {
        if (tok_kind(L) == TokenKind::EndKeyword)
            return opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        if (tok_kind(L) == TokenKind::CloseBrace)
            return opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        return SD::MustWrap;
    }
    if (is_else_keyword(L) &&
        (is_begin_keyword(R) || tok_kind(R) == TokenKind::IfKeyword || tok_kind(R) == TokenKind::OpenBrace))
        return (tok_kind(R) == TokenKind::OpenBrace && opts.statement.begin_newline) ? SD::MustWrap
                                                                                : SD::MustAppend;
    if (tok_kind(L) == TokenKind::CloseParenthesis &&
        (is_begin_keyword(R) || tok_kind(R) == TokenKind::OpenBrace))
        return opts.statement.begin_newline ? SD::MustWrap : SD::MustAppend;
    if (lx == "#")
        return SD::MustAppend;
    return SD::Undecided;
}

static void push_tok(std::vector<Tok>& toks, TokenKind kind, std::string text, int pos,
                     bool whitespace = false, bool comment = false, bool directive = false,
                     syntax::SyntaxKind directive_kind = syntax::SyntaxKind::Unknown,
                     bool define_block = false,
                     CommentKind comment_kind = CommentKind::None) {
    auto lex = std::make_shared<TokLexeme>();
    lex->kind = kind;
    lex->lo = lower(text);
    lex->text = std::move(text);
    lex->pos = pos;
    lex->whitespace = whitespace;
    lex->comment = comment;
    lex->comment_kind = comment ? comment_kind : CommentKind::None;
    lex->directive = directive;
    lex->define_block = define_block;
    lex->directive_kind = directive_kind;
    Tok t;
    t.lex = std::move(lex);
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
                     false, syntax::SyntaxKind::Unknown, define_block, CommentKind::Line);
            i = j;
        } else if (source[i] == '/' && i + 1 < end && source[i + 1] == '*') {
            // Block comment: consume up to and including the closing */.
            size_t j = i + 2;
            while (j + 1 < end && !(source[j] == '*' && source[j + 1] == '/'))
                ++j;
            if (j + 1 < end)
                j += 2;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, true,
                     false, syntax::SyntaxKind::Unknown, define_block, CommentKind::Block);
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

    for (size_t line_start = 0; line_start < source.size();) {
        size_t first = line_start;
        while (first < source.size() && (source[first] == ' ' || source[first] == '\t'))
            ++first;
        if (first < source.size() && source.compare(first, 7, "`define") == 0 &&
            (first + 7 == source.size() ||
             !(std::isalnum((unsigned char)source[first + 7]) ||
               source[first + 7] == '_' || source[first + 7] == '$'))) {
            size_t end = scan_directive_end(source, first, source.size());
            if (end < source.size() && source[end] == '\n')
                ++end;
            define_ranges.push_back(
                {line_start, end, syntax::SyntaxKind::DefineDirective});
            line_start = end;
        } else {
            size_t nl = source.find('\n', line_start);
            if (nl == std::string::npos)
                break;
            line_start = nl + 1;
        }
    }

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
        if (const DefineRange* dr = find_define_range(off)) {
            if (cursor < dr->start)
                append_gap(cursor, dr->start);
            append_gap(std::max(cursor, dr->start), dr->end);
            cursor = std::max(cursor, dr->end);
            continue;
        }
        std::string raw(token.rawText());
        if (token.kind == TokenKind::Directive && !raw.empty() && raw[0] != '`') {
            size_t tick = raw.find('`');
            if (tick != std::string::npos) {
                off += tick;
                raw.erase(0, tick);
                if (off > source.size() || off < cursor)
                    continue;
            }
        }
        bool token_define_block = find_define_range(off) != nullptr;
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
                    if (end < source.size() && source[end] == '\n')
                        ++end;
                    // Treat leading indentation on the directive line as part
                    // of the define block so the whole macro definition can be
                    // rendered verbatim.
                    size_t range_start = line_start >= cursor ? line_start : off;
                    define_ranges.push_back({range_start, end, token.directiveKind()});
                    if (cursor < range_start)
                        append_gap(cursor, range_start);
                    append_gap(range_start, end);
                } else {
                    if (cursor < off)
                        append_gap(cursor, off);
                    push_tok(toks, token.kind, source.substr(off, end - off), (int)off,
                             false, false, true, token.directiveKind());
                }
                cursor = std::max(cursor, end);
                continue;
            }
            if ((token.kind == TokenKind::MacroUsage ||
                 (token.kind == TokenKind::Directive &&
                  token.directiveKind() == syntax::SyntaxKind::MacroUsage)) &&
                !token_define_block) {
                size_t name_end = off + 1;
                while (name_end < source.size() &&
                       (std::isalnum((unsigned char)source[name_end]) ||
                        source[name_end] == '_' || source[name_end] == '$'))
                    ++name_end;
                if (cursor < off)
                    append_gap(cursor, off);
                push_tok(toks, token.kind, source.substr(off, name_end - off), (int)off,
                         false, false, true, token.directiveKind(), token_define_block);
                cursor = std::max(cursor, name_end);
                continue;
            }
        }
        // Fill gap between cursor and this token with trivia text for all
        // non-define tokens.  Define directives handle their own range above
        // so their leading indentation can be preserved as part of the block.
        if (cursor < off)
            append_gap(cursor, off);
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

    return toks;
}


static std::vector<Tok> significant_tokens(const std::string& text) {
    std::vector<Tok> out;
    for (auto& tok : collect_lexer_tokens(text)) {
        if (!tok_whitespace(tok) && !tok_comment(tok) && !tok_directive(tok))
            out.push_back(std::move(tok));
    }
    return out;
}


static bool tok_is(const Tok& tok, const std::string& text, TokenKind kind) {
    return tok_text(tok) == text || tok_kind(tok) == kind;
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
    return tok_kind(toks[0]);
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




// ---------------------------------------------------------------------------
// Statement assignment alignment pass
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Variable declaration alignment pass — ported from _align_variable_declarations_pass
// ---------------------------------------------------------------------------

struct VarParsed {
    std::string indent, type_kw, qualifier, dim;
    std::vector<std::pair<std::string, std::string>> declarators; // (name, trailing)
    std::string comment;
};



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

// Extract content of outermost (...) immediately before ;

static std::string take_leading_portlist_block_comments_tokenized(std::string& ports_str);




// Parse named port connections .name(signal), ...
// Returns false if positional

struct InstancePortEntry {
    bool directive{false};
    bool comment{false};
    std::string text;
    std::string port;
    std::string sig;
};


// Split flat into (module_type, param_block, inst_name) and identify the exact
// port-list parens belonging to that instance header.



// ---------------------------------------------------------------------------
// Function/task call formatting pass
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Typedef enum and modport formatting passes
// ---------------------------------------------------------------------------


struct EnumItemParsed {
    std::string name;
    std::string value;
    bool has_value{false};
};




struct ModportItemParsed {
    std::string direction;
    std::string name;
};

struct ModportParsed {
    std::string name;
    std::vector<ModportItemParsed> items;
};









// ---------------------------------------------------------------------------
// Module port-list formatting pass
// ---------------------------------------------------------------------------

// Given a single line that starts a module declaration,
// extract prefix (including '('), ports_str (between outermost parens), suffix (')...;').
// Returns false if not a single-line module header.



struct ModuleHeaderScan {
    int paren{0};
    int brace{0};
    int bracket{0};
    bool saw_paren{false};
    bool saw_import_before_port_list{false};
};

















enum class PortListEntryKind { Port, Comment, Directive, Other };



static std::string take_leading_portlist_block_comments_tokenized(std::string& ports_str) {
    size_t erase_end = 0;
    std::string comments;
    for (const auto& tok : collect_lexer_tokens(ports_str)) {
        if (tok_whitespace(tok))
            continue;
        if (!tok_block_comment(tok))
            break;
        comments += ports_str.substr(erase_end, (size_t)tok_pos(tok) + tok_text(tok).size() - erase_end);
        erase_end = (size_t)tok_pos(tok) + tok_text(tok).size();
    }
    if (erase_end > 0)
        ports_str.erase(0, erase_end);
    return comments;
}





static bool token_range_has_pp_conditional(const std::vector<Tok>& tokens, size_t first,
                                           size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]))
            continue;
        if (tokens[i].is_pp_conditional_directive ||
            (is_line_directive(tokens[i]) && is_pp_conditional(tok_directive_kind(tokens[i]))))
            return true;
    }
    return false;
}


static bool token_range_has_line_directive(const std::vector<Tok>& tokens, size_t first,
                                           size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]))
            continue;
        if (is_line_directive(tokens[i]))
            return true;
    }
    return false;
}

static bool token_range_disabled_or_passthrough(const std::vector<Tok>& tokens, size_t first,
                                                size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (!tok_whitespace(tokens[i]) && (tokens[i].in_disabled_region || tokens[i].fmt_passthrough))
            return true;
    }
    return false;
}

static bool token_range_disabled_or_non_pp_passthrough(const std::vector<Tok>& tokens,
                                                       size_t first, size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]))
            continue;
        if (tokens[i].in_disabled_region)
            return true;
        if (tokens[i].fmt_passthrough && !is_line_directive(tokens[i]) &&
            !tokens[i].is_pp_conditional_directive &&
            !tokens[i].in_pp_conditional_line_tail)
            return true;
    }
    return false;
}

static bool token_range_has_define_block(const std::vector<Tok>& tokens, size_t first,
                                         size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (!tok_whitespace(tokens[i]) && tok_define_block(tokens[i]))
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
    if (open_idx < tokens.size() && tok_is(tokens[open_idx], "(", TokenKind::OpenParenthesis) &&
        tokens[open_idx].matching_token != SIZE_MAX && tokens[open_idx].matching_token < end_limit)
        return tokens[open_idx].matching_token;

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
        if (sigs.empty() || tok_kind(tokens[sigs[0]]) != TokenKind::ClassKeyword)
            continue;
        bool has_extends = false;
        for (size_t idx : sigs)
            has_extends = has_extends || tok_kind(tokens[idx]) == TokenKind::ExtendsKeyword;
        if (!has_extends)
            continue;

        size_t hash_idx = SIZE_MAX, open_idx = SIZE_MAX;
        for (size_t si = 0; si + 1 < sigs.size(); ++si) {
            if (tok_text(tokens[sigs[si]]) == "#" &&
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
    size_t leading_comma{SIZE_MAX};
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
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) ||
            (tok_directive(tokens[i]) && !is_macro_usage(tokens[i])))
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
        else if (sigs.empty())
            e.leading_comma = idx;
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

static bool range_has_port_separator_comment_barrier(const std::vector<Tok>& tokens, size_t first,
                                                     size_t last) {
    for (size_t k = first; k < last && k < tokens.size(); ++k) {
        if (!tok_comment(tokens[k]))
            continue;
        if (comment_role_own_line_rendered(tokens[k].comment_role) || tok_line_comment(tokens[k]))
            return true;
    }
    return false;
}

static void format_comment_layout_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    // Pass 1: set baseline fmt_* for every Comment-owned token using its pre-classified
    // comment_role.  The port-list fixup below may override specific cases.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!tok_comment(tokens[i])) continue;
        if (tokens[i].layout_owner != LayoutOwner::Comment) continue;
        auto& ctok = tokens[i];

        bool source_newline_before_comment = false;
        for (size_t j = i; j > 0; --j) {
            if (!tok_whitespace(tokens[j - 1])) break;
            if (tok_text(tokens[j - 1]).find('\n') != std::string::npos) {
                source_newline_before_comment = true;
                break;
            }
        }
        if (tok_block_comment(ctok) && source_newline_before_comment) {
            ctok.fmt_newline_before = true;
            ctok.fmt_blank_lines = 0;
            ctok.fmt_spaces_before = 0;
        }

        if (tok_block_comment(ctok) && tok_text(ctok).find('\n') != std::string::npos) {
            // Multi-line block comments are whitespace-sensitive inside the
            // token.  Leave their text intact so a second format does not
            // reclassify/reflow their continuation lines.  If the previous
            // significant token is a line comment, force a new line; otherwise
            // the block opener would be swallowed by the line comment and the
            // block's continuation text would be lexed as code on pass two.
            size_t prev_sig = SIZE_MAX;
            for (size_t j = i; j > 0; --j) {
                if (tok_whitespace(tokens[j - 1]))
                    continue;
                prev_sig = j - 1;
                break;
            }
            if (prev_sig != SIZE_MAX && tok_line_comment(tokens[prev_sig])) {
                ctok.fmt_newline_before = true;
                ctok.fmt_blank_lines = 0;
                ctok.fmt_spaces_before = 0;
            }
            ctok.fmt_passthrough = true;
            continue;
        }

        if (comment_role_inline_rendered(ctok.comment_role)) {
            ctok.fmt_newline_before = false;
            ctok.fmt_blank_lines   = 0;
            ctok.fmt_spaces_before = 1;
            ctok.fmt_indent        = 0;
        } else {
            // Own-line (standalone) comment: indent to match the next Basic token.
            ctok.fmt_newline_before = true;
            ctok.fmt_spaces_before  = 0;

            int indent = 0;
            for (size_t j = i + 1; j < tokens.size(); ++j) {
                if (tok_whitespace(tokens[j])) continue;
                if (tokens[j].layout_owner == LayoutOwner::Basic) {
                    indent = tokens[j].fmt_indent;
                    break;
                }
            }
            ctok.fmt_indent = indent;

            // Preserve blank lines visible in the source whitespace before this comment,
            // except at file start.  Leading file whitespace is intentionally
            // normalized away; preserving it on the first formatting pass but not
            // on the second makes files that start with blank lines non-idempotent.
            bool has_prev_non_ws = false;
            for (size_t j = i; j > 0; --j) {
                if (!tok_whitespace(tokens[j - 1])) {
                    has_prev_non_ws = true;
                    break;
                }
            }
            int blank = 0;
            if (has_prev_non_ws) {
                for (size_t j = i; j > 0; --j) {
                    if (!tok_whitespace(tokens[j - 1])) break;
                    int nl = (int)std::count(tok_text(tokens[j - 1]).begin(),
                                              tok_text(tokens[j - 1]).end(), '\n');
                    blank = std::max(0, nl - 1);
                }
            }
            ctok.fmt_blank_lines = std::min(blank, opts.blank_lines_between_items);
        }
    }

    // Pass 2: port-list comment fixup (overrides the baseline above for comments
    // that appear inside module/interface header port or parameter lists).
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].in_disabled_region || tokens[i].fmt_passthrough)
            continue;
        bool header_kw = tok_kind(tokens[i]) == TokenKind::ModuleKeyword ||
                         tok_kind(tokens[i]) == TokenKind::MacromoduleKeyword ||
                         tok_kind(tokens[i]) == TokenKind::InterfaceKeyword ||
                         tok_kind(tokens[i]) == TokenKind::ProgramKeyword;
        if (!header_kw)
            continue;

        size_t port_open = SIZE_MAX;
        int paren = 0, bracket = 0, brace = 0;
        for (size_t j = i + 1; j < tokens.size(); ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) {
                size_t prev = prev_code_sig(tokens, i, j);
                if (paren == 0 && !(prev != SIZE_MAX && tok_text(tokens[prev]) == "#")) {
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
                    if (tok_kind(tokens[k]) == TokenKind::ImportKeyword)
                        has_import = true;
                size_t next = next_code_sig(tokens, j + 1, tokens.size());
                if (!(has_import && next != SIZE_MAX &&
                      (tok_is(tokens[next], "(", TokenKind::OpenParenthesis) ||
                       tok_is(tokens[next], "#", TokenKind::Hash))))
                    break;
            } else if ((tok_kind(tokens[j]) == TokenKind::EndModuleKeyword ||
                        tok_kind(tokens[j]) == TokenKind::EndInterfaceKeyword ||
                        tok_kind(tokens[j]) == TokenKind::EndProgramKeyword) && paren == 0) {
                break;
            }
        }
        if (port_open == SIZE_MAX)
            continue;
        size_t port_close = matching_close_paren(tokens, port_open, tokens.size());
        if (port_close == SIZE_MAX)
            continue;
        size_t semi = next_code_sig(tokens, port_close + 1, tokens.size());
        if (semi == SIZE_MAX || !tok_is(tokens[semi], ";", TokenKind::Semicolon))
            continue;

        int base_indent = tokens[i].fmt_indent;

        auto format_header_list_comments = [&](size_t open_idx, size_t close_idx) {
            for (size_t j = open_idx + 1; j < close_idx; ++j) {
                if (!tok_comment(tokens[j]) || tokens[j].layout_owner != LayoutOwner::Comment)
                    continue;
                if (tok_block_comment(tokens[j]) &&
                    tok_text(tokens[j]).find('\n') != std::string::npos) {
                    tokens[j].fmt_newline_before = true;
                    tokens[j].fmt_blank_lines = 0;
                    tokens[j].fmt_indent = base_indent + 1;
                    tokens[j].fmt_spaces_before = 0;
                    tokens[j].fmt_passthrough = true;
                    continue;
                }
                if (comment_role_own_line_rendered(tokens[j].comment_role)) {
                    tokens[j].fmt_newline_before = true;
                    tokens[j].fmt_blank_lines = 0;
                    tokens[j].fmt_indent = base_indent + 1;
                    tokens[j].fmt_spaces_before = 0;
                } else if (comment_role_inline_rendered(tokens[j].comment_role)) {
                    tokens[j].fmt_newline_before = false;
                    tokens[j].fmt_spaces_before = 1;
                } else {
                    if (tokens[j].fmt_newline_before) {
                        tokens[j].fmt_blank_lines = 0;
                        tokens[j].fmt_indent = base_indent + 1;
                        tokens[j].fmt_spaces_before = 0;
                    } else {
                        tokens[j].fmt_spaces_before = 1;
                    }
                }
            }
        };

        // Comments in both the optional module-header parameter block and the
        // port list need stable own-line indentation.  If parameter comments are
        // left to the generic comment pass, leading-comma parameter style can
        // render a comma after a line comment; the second format then lexes that
        // comma as comment text and changes the output.
        size_t param_open = SIZE_MAX, param_close = SIZE_MAX;
        for (size_t j = i + 1; j < port_open; ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_text(tokens[j]) == "#") {
                size_t n = next_code_sig(tokens, j + 1, port_open);
                if (n != SIZE_MAX && tok_is(tokens[n], "(", TokenKind::OpenParenthesis)) {
                    param_open = n;
                    param_close = matching_close_paren(tokens, param_open, port_open);
                    break;
                }
            }
        }
        if (param_open != SIZE_MAX && param_close != SIZE_MAX)
            format_header_list_comments(param_open, param_close);
        format_header_list_comments(port_open, port_close);
        i = semi;
    }
}

static void format_portlist_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].in_disabled_region || tokens[i].fmt_passthrough)
            continue;
        bool header_kw = tok_kind(tokens[i]) == TokenKind::ModuleKeyword ||
                         tok_kind(tokens[i]) == TokenKind::MacromoduleKeyword ||
                         tok_kind(tokens[i]) == TokenKind::InterfaceKeyword ||
                         tok_kind(tokens[i]) == TokenKind::ProgramKeyword;
        if (!header_kw)
            continue;

        size_t port_open = SIZE_MAX;
        int paren = 0, bracket = 0, brace = 0;
        for (size_t j = i + 1; j < tokens.size(); ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) {
                size_t prev = prev_code_sig(tokens, i, j);
                if (paren == 0 && !(prev != SIZE_MAX && tok_text(tokens[prev]) == "#")) {
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
                    if (tok_kind(tokens[k]) == TokenKind::ImportKeyword)
                        has_import = true;
                size_t next = next_code_sig(tokens, j + 1, tokens.size());
                if (!(has_import && next != SIZE_MAX &&
                      (tok_is(tokens[next], "(", TokenKind::OpenParenthesis) ||
                       tok_is(tokens[next], "#", TokenKind::Hash))))
                    break;
            } else if ((tok_kind(tokens[j]) == TokenKind::EndModuleKeyword ||
                      tok_kind(tokens[j]) == TokenKind::EndInterfaceKeyword ||
                      tok_kind(tokens[j]) == TokenKind::EndProgramKeyword) && paren == 0)
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
            token_range_has_line_directive(tokens, i, semi + 1) ||
            token_range_disabled_or_non_pp_passthrough(tokens, i, semi + 1))
            continue;

        std::vector<size_t> sigs = code_sig_indices(tokens, i, semi + 1);
        int base_indent = tokens[i].fmt_indent;

        // Format module/interface parameter block immediately before the port list.
        size_t hash_idx = SIZE_MAX, param_open = SIZE_MAX, param_close = SIZE_MAX;
        for (size_t si = 0; si + 1 < sigs.size(); ++si) {
            if (sigs[si] >= port_open)
                break;
            if (tok_text(tokens[sigs[si]]) == "#" &&
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
                    bool comma_after_own_line_comment = false;
                    if (!params.empty())
                        comma_after_own_line_comment =
                            range_has_port_separator_comment_barrier(tokens, params.back() + 1, j);
                    if (comma_after_own_line_comment) {
                        // Leading-comma parameter style after an own-line comment:
                        //
                        //   parameter A = 1
                        //   // comment
                        //   , parameter B = 2
                        //
                        // Keep the comma as the line-start token for the next
                        // parameter.  Rendering it after the line comment makes
                        // the comma part of the comment on the next formatter pass.
                        params.push_back(j);
                    } else {
                        size_t n = next_code_sig(tokens, j + 1, param_close);
                        if (n != SIZE_MAX)
                            params.push_back(n);
                    }
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
                    if (tok_is(tokens[pidx], ",", TokenKind::Comma)) {
                        size_t n = next_code_sig(tokens, pidx + 1, param_close);
                        if (n != SIZE_MAX) {
                            tokens[n].fmt_newline_before = false;
                            tokens[n].fmt_spaces_before = 1;
                        }
                    }
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
            if (tok_is(tokens[j], ",", TokenKind::Comma) && paren == 0 && bracket == 0 && brace == 0) {
                bool comma_after_own_line_comment = false;
                if (!current.empty()) {
                    size_t prev_in_entry = current.back();
                    comma_after_own_line_comment =
                        range_has_port_separator_comment_barrier(tokens, prev_in_entry + 1, j);
                }
                if (comma_after_own_line_comment) {
                    // BlackParrot-style ANSI headers sometimes use a leading
                    // comma after an own-line section comment:
                    //
                    //   input clk_i
                    //   // request channel
                    //   , input req_i
                    //
                    // The comma is a prefix separator for the following port,
                    // not a trailing delimiter owned by the comment line.  Keep
                    // it with the next raw entry so later alignment cannot
                    // render it as "// request channel,".
                    raw_entries.push_back(current);
                    current.clear();
                    current.push_back(j);
                    continue;
                }
                current.push_back(j);
                raw_entries.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(j);
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
        // If a statement (e.g. import p::*;) precedes the port list on the same
        // line, the opening '(' should start on a new line.  basic_formatting
        // used to propagate pending_nl here; format_portlist_pass must now do it.
        {
            size_t prev_sig = prev_code_sig(tokens, i, port_open);
            if (prev_sig != SIZE_MAX && tok_is(tokens[prev_sig], ";", TokenKind::Semicolon))
                tokens[port_open].fmt_newline_before = true;
        }
        for (size_t ei = 0; ei < entries.size(); ++ei) {
            auto& e = entries[ei];
            if (!e.valid)
                continue;
            bool put_newline = true;
            if (!ansi && opts.module.non_ansi_port_per_line_enabled &&
                opts.module.non_ansi_port_per_line > 1) {
                put_newline = (ei % (size_t)opts.module.non_ansi_port_per_line) == 0;
            }
            size_t line_start = e.leading_comma != SIZE_MAX ? e.leading_comma : e.start;
            tokens[line_start].fmt_newline_before = put_newline;
            tokens[line_start].fmt_blank_lines = 0;
            tokens[line_start].fmt_indent = base_indent + 1;
            tokens[line_start].fmt_spaces_before = put_newline ? 0 : 1;
            if (e.leading_comma != SIZE_MAX) {
                tokens[e.start].fmt_newline_before = false;
                tokens[e.start].fmt_spaces_before = 1;
            }
        }
        // Comment tokens in this range are owned by
        // format_comment_layout_pass(); this pass may format code tokens around
        // them but must not rewrite comment layout metadata.
        tokens[port_close].fmt_newline_before = true;
        tokens[port_close].fmt_blank_lines = 0;
        tokens[port_close].fmt_indent = base_indent;
        tokens[port_close].fmt_spaces_before = 0;
        tokens[semi].fmt_newline_before = false;
        tokens[semi].fmt_spaces_before = 0;

        // Set fmt_spaces_before for all tokens within parameter and port ranges
        // using spaces_req.  The delimiter tokens already have fmt_newline_before /
        // fmt_indent set above; here we fill in horizontal spacing so that
        // within-port declarations (e.g. "input logic [W-1:0] name") render with
        // correct token-pair spacing even though basic_formatting no longer writes
        // to these tokens.  This must run BEFORE align_header_ports_metadata so
        // the alignment pass can adjust rather than clobber these baseline spaces.
        auto apply_intra_spacing = [&](size_t open_idx, size_t close_idx) {
            const Tok* prev_sig = nullptr;
            int dim = 0;
            for (size_t j = open_idx; j <= close_idx && j < tokens.size(); ++j) {
                if (tok_whitespace(tokens[j])) continue;
                if (tok_comment(tokens[j])) continue;
                // Don't override spacing for line-start tokens; their indent
                // is already set and fmt_spaces_before should stay 0.
                if (!tokens[j].fmt_newline_before && prev_sig != nullptr) {
                    tokens[j].fmt_spaces_before =
                        spaces_req(*prev_sig, tokens[j], opts,
                                   dim > 0, false, true, ParenKind::Ordinary, false);
                }
                if (tok_is(tokens[j], "[", TokenKind::OpenBracket))      ++dim;
                else if (tok_is(tokens[j], "]", TokenKind::CloseBracket) && dim > 0) --dim;
                prev_sig = &tokens[j];
            }
        };
        if (hash_idx != SIZE_MAX && param_close != SIZE_MAX) {
            apply_intra_spacing(hash_idx, param_close);
        }
        apply_intra_spacing(port_open, port_close);

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





// ---------------------------------------------------------------------------
// `define continuation backslash alignment pass
// ---------------------------------------------------------------------------
// Groups consecutive lines ending with '\' and aligns the '\' to a common
// column (max content width + 1 space).



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
            if (tok_define_block(tokens[j])) {
                info.disabled = true;
                break;
            }
            if (tokens[j].in_disabled_region) {
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
            if (tok.in_disabled_region || tok.fmt_passthrough || tok_define_block(tok))
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
                if (tok_comment(tok) && comment_role_own_line_rendered(tok.comment_role))
                    break;
                if (tok_comment(tok))
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
            tok_kind(tokens[sig_indices[0]]) == TokenKind::ParameterKeyword) {
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
                    (tok_kind(tokens[prev_sigs[0]]) == TokenKind::ModuleKeyword ||
                     tok_kind(tokens[prev_sigs[0]]) == TokenKind::InterfaceKeyword ||
                     tok_kind(tokens[prev_sigs[0]]) == TokenKind::ProgramKeyword)) {
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
            bool skip_next = tok_kind(tokens[ln.first_sig]) == TokenKind::AssignKeyword;
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
        if (tok_kind(tok) == TokenKind::EnumKeyword) {
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
            lines[li].prev_starts_for = (tok_kind(tokens[prev.first_sig]) == TokenKind::ForKeyword);
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
            if (comment_idx == SIZE_MAX &&
                tokens[k].comment_role == CommentRole::TrailingStatement)
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
                if (tokens[k].comment_role != CommentRole::TrailingStatement)
                    break;
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
            if (tokens[k].in_disabled_region || tokens[k].fmt_passthrough ||
                tok_define_block(tokens[k]) ||
                tokens[k].layout_owner == LayoutOwner::ModuleHeaderParameterList ||
                tokens[k].layout_owner == LayoutOwner::ModuleHeaderPortList)
                ln.disabled = true;
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
static bool render_needs_lexical_space(const Tok& L, const Tok& R) {
    auto wordlike = [](const Tok& t) { return is_identifier(t) || is_keyword(t) || is_numeric(t); };
    return wordlike(L) && wordlike(R);
}

static std::string render_tokens(const std::vector<Tok>& tokens, const FormatOptions& opts) {
    std::string out;
    size_t total_len = 0;
    for (const auto& t : tokens) total_len += tok_text(t).size() + t.fmt_text_before.size();
    out.reserve(total_len + total_len / 4);

    const std::string indent_unit(opts.indent_size, ' ');
    bool at_bol = true;
    bool line_comment_open = false;
    const Tok* prev_rendered = nullptr;

    for (const auto& tok : tokens) {
        if (tok.fmt_passthrough) {
            if (!at_bol && line_comment_open && !tok_whitespace(tok)) {
                out += '\n';
                at_bol = true;
                line_comment_open = false;
            }
            if (tok.fmt_newline_before) {
                if (!at_bol) out += '\n';
                for (int k = 0; k < tok.fmt_blank_lines; ++k)
                    out += '\n';
                at_bol = true;
                line_comment_open = false;
            }
            if (!at_bol && tok.fmt_spaces_before > 0)
                out.append(tok.fmt_spaces_before, ' ');
            out += tok.fmt_text_before;
            out += tok_text(tok);
            if (tok_text(tok).find('\n') != std::string::npos)
                line_comment_open = false;
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            if (!tok_whitespace(tok)) {
                prev_rendered = &tok;
                line_comment_open = tok_line_comment(tok) &&
                                    tok_text(tok).find('\n') == std::string::npos;
            }
            continue;
        }
        if (tok_whitespace(tok)) continue;

        if (tok.fmt_newline_before) {
            if (!at_bol) out += '\n';
            for (int k = 0; k < tok.fmt_blank_lines; ++k)
                out += '\n';
            at_bol = true;
            line_comment_open = false;
        }

        if (!at_bol && line_comment_open) {
            out += '\n';
            at_bol = true;
            line_comment_open = false;
        }

        int spaces_before = tok.fmt_spaces_before;
        if (!at_bol && spaces_before == 0 && prev_rendered != nullptr) {
            if (render_needs_lexical_space(*prev_rendered, tok))
                spaces_before = 1;
            else if (is_assignment_op(tok, true))
                spaces_before = binary_space_before(opts.spacing.assignment_operator_spacing) ? 1 : 0;
            else if (is_assignment_op(*prev_rendered, true))
                spaces_before = binary_space_after(opts.spacing.assignment_operator_spacing) ? 1 : 0;
        }

        if (at_bol && !tok_text(tok).empty()) {
            for (int k = 0; k < tok.fmt_indent; ++k)
                out += indent_unit;
            if (spaces_before > 0)
                out.append(spaces_before, ' ');
            at_bol = false;
        } else if (!at_bol && spaces_before > 0) {
            out.append(spaces_before, ' ');
        }

        out += tok.fmt_text_before;
        out += tok_text(tok);
        if (tok_text(tok).find('\n') != std::string::npos)
            line_comment_open = false;
        if (!tok_text(tok).empty() && tok_text(tok).back() == '\n')
            at_bol = true;
        prev_rendered = &tok;
        line_comment_open = tok_line_comment(tok) && tok_text(tok).find('\n') == std::string::npos;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Format metadata snapshot helpers — used by basic_formatting and later passes
// ---------------------------------------------------------------------------

struct FormatMetadataSnapshot {
    int fmt_indent{0};
    int fmt_spaces_before{0};
    std::string fmt_text_before;
    bool fmt_newline_before{false};
    int fmt_blank_lines{0};
    bool fmt_passthrough{false};
};

static FormatMetadataSnapshot snapshot_format_metadata(const Tok& tok) {
    return {tok.fmt_indent,
            tok.fmt_spaces_before,
            tok.fmt_text_before,
            tok.fmt_newline_before,
            tok.fmt_blank_lines,
            tok.fmt_passthrough};
}


static bool format_metadata_changed(const Tok& tok, const FormatMetadataSnapshot& before) {
    return tok.fmt_indent != before.fmt_indent ||
           tok.fmt_spaces_before != before.fmt_spaces_before ||
           tok.fmt_text_before != before.fmt_text_before ||
           tok.fmt_newline_before != before.fmt_newline_before ||
           tok.fmt_blank_lines != before.fmt_blank_lines ||
           tok.fmt_passthrough != before.fmt_passthrough;
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
    //   in_disabled_region token belongs to a format-off region (set earlier)
    //   fmt_passthrough     token should be emitted exactly as collected
    //
    // Later passes can still adjust this metadata, but this pass establishes the
    // baseline line breaking and indentation by walking the token stream once and
    // maintaining lightweight parser state (indent stack, grouping depths, macro
    // roles, preprocessor conditionals, case labels, etc.).
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

    // Format-disabled regions are passthrough. after_dis bridges the transition
    // back to normal formatting.
    bool after_dis = false;

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
    bool coverpoint_brace_pend = false;
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
    auto apply_newline = [&](size_t idx, bool write_fmt = true) {
        // Materialize a deferred newline/blank-line request on the current token.
        // Once attached to fmt_* metadata, the pending request is consumed.
        if (pending_nl || blank_pend > 0) {
            if (write_fmt) {
                tokens[idx].fmt_newline_before = true;
                tokens[idx].fmt_blank_lines = blank_pend;
            }
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
            // Macro definitions are whitespace-sensitive.  Preserve every token
            // in the collected define range verbatim instead of reconstructing
            // spacing/indentation metadata.  fmt_passthrough and fmt_newline_before
            // are set by passthrough_regions_pass; here we only update local state.
            bool starts_define_block = !(prev && tok_define_block(*prev));
            if (starts_define_block && !at_bol) {
                // Consume pending_nl and treat as if a newline was emitted.
                pending_nl = true;
            }
            if (pending_nl || blank_pend > 0) {
                pending_nl = false;
                blank_pend = 0;
                at_bol = true;
            }
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            prev_macro_role_valid = false;
            prev = &tok;
            original_newline_before_token = false;
            continue;
        }

        // --- A. Disabled region ---
        if (tok.in_disabled_region) {
            // verilog_format/verilog-format:off regions are preserved exactly.
            // fmt_passthrough and fmt_newline_before are set by passthrough_regions_pass;
            // here we only consume pending_nl to keep local state consistent.
            if (pending_nl || blank_pend > 0) {
                pending_nl = false;
                blank_pend = 0;
                at_bol = true;
            }
            at_bol = !tok_text(tok).empty() && tok_text(tok).back() == '\n';
            after_dis = !at_bol;
            continue;
        }

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
            int blank_nl = (prev && tok_define_block(*prev)) ? nl : (nl - 1);
            if (blank_nl > 0) {
                int extra = std::min(blank_nl, opts.blank_lines_between_items);
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
            // Keep ternary question-mark spacing controlled solely by the binary
            // operator spacing option.  Without this explicit normalization, a
            // formatted token sequence like ?$clog2(...) can be re-lexed and the
            // second pass may add a space before the system function, causing an
            // idempotency failure.
            if (tok_text(*prev) == "?")
                spaces = binary_space_after(opts.spacing.binary_operator_spacing) ? 1 : 0;
            else if (tok_text(tok) == "?")
                spaces = is_numeric(*prev) ? 1 :
                    (binary_space_before(opts.spacing.binary_operator_spacing) ? 1 : 0);
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
        if (prev_macro_role_valid && tok_kind(tok) == TokenKind::ElseKeyword &&
            prev_macro_role == MacroRole::BlockEndLike)
            dec = opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        if (tok_is_macro && macro_force_own_line(tok_macro_class.role)) {
            if (tok_macro_class.role == MacroRole::BlockBeginLike && prev &&
                tok_kind(*prev) == TokenKind::CloseParenthesis) {
                dec = opts.statement.begin_newline ? SD::MustWrap : SD::MustAppend;
            } else if (!at_bol) {
                dec = SD::MustWrap;
            }
        }
        if (tok_is_macro && original_newline_before_token && !at_bol && paren_depth == 0)
            dec = SD::MustWrap;
        if (prev && is_macro_usage(*prev) && original_newline_before_token &&
            paren_depth == 0 && !tok_whitespace(tok) && !tok_comment(tok) &&
            !tok_is(tok, "(", TokenKind::OpenParenthesis)) {
            dec = SD::MustWrap;
        }
        if (prev && prev->in_covergroup && tok.in_covergroup &&
            tok_is(*prev, "}", TokenKind::CloseBrace) &&
            original_newline_before_token && !tok_comment(tok)) {
            dec = SD::MustWrap;
        }

        // --- D. Inline-comment suppression ---
        // Use the pre-classified comment_role (set once by classify_comments_pass
        // from original source positions) instead of re-deriving attachment from
        // the current-pass whitespace.  This prevents oscillation when formatted
        // output has different spacing than the input.
        bool inline_comment = tok_comment(tok) &&
                              comment_role_inline_rendered(tok.comment_role);

        do_while_tail = false;
        if (prev && tok_kind(*prev) == TokenKind::EndKeyword && tok_kind(tok) == TokenKind::WhileKeyword) {
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
        bool disable_target = prev && tok_kind(*prev) == TokenKind::DisableKeyword;
        bool wait_fork_target = prev && tok_kind(*prev) == TokenKind::WaitKeyword &&
                                tok_kind(tok) == TokenKind::ForkKeyword;

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
        if (case_label_pending_nl && tok_kind(tok) == TokenKind::BeginKeyword)
            case_label_pending_nl = false;

        if (inline_comment && pending_nl) {
            if (!case_label_pending_nl)
                pending_nl = false;
        } else if (tok_comment(tok) &&
                   (tok.comment_role == CommentRole::LeadingStatement ||
                    tok.comment_role == CommentRole::OwnLineInterstitial)) {
            dec = SD::MustWrap;
        } else if (tok_line_comment(tok) && tok.comment_role == CommentRole::None) {
            // Fallback only for comments intentionally excluded from stable
            // classification, such as disabled/passthrough regions. Classified
            // comments must be formatted by CommentRole, not by re-reading
            // original source line position.
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
        // Non-Basic tokens (Comment, PortList, ParameterList) still run through
        // this state machine so at_bol / pending_nl stay consistent, but their
        // owning passes set fmt_* instead.
        const bool is_basic_owned = (tok.layout_owner == LayoutOwner::Basic);
        if (dec == SD::MustWrap) {
            pending_nl = false;
            if (is_basic_owned) tok.fmt_newline_before = true;
            if (is_basic_owned) tok.fmt_blank_lines = blank_pend;
            blank_pend = 0;
            at_bol = true;
        } else if (dec == SD::MustAppend) {
            if (pending_nl) {
                pending_nl = false;
                blank_pend = 0;
            }
            if (!at_bol && spaces > 0 && is_basic_owned)
                tok.fmt_spaces_before = spaces;
        } else {
            apply_newline(i, is_basic_owned);
            if (!at_bol && spaces > 0 && is_basic_owned)
                tok.fmt_spaces_before = spaces;
        }

        // --- F. Single-statement indent ---
        // Apply the one-token-late indent for control statements without begin.
        // If the next token is begin, the normal block opener handles indentation
        // instead.  Constraint braces are also allowed to become real scopes.
        if (single_stmt_pending && at_bol) {
            if (constraint_depth > 0 && tok_is(tok, "{", TokenKind::OpenBrace)) {
                // constraint body brace — let brace handler below create the scope
            } else if (tok_kind(tok) == TokenKind::BeginKeyword) {
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
        } else if (is_indent_close(tok_kind(tok))) {
            if (tok_kind(tok) == TokenKind::EndCaseKeyword) {
                case_depth = std::max(0, case_depth - 1);
                case_conditional_depth = 0;
            }
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        } else if (is_close_group(tok) && tok_is(tok, "}", TokenKind::CloseBrace) && !brace_stk.empty() &&
                   (brace_stk.back() == "struct" || brace_stk.back() == "constraint" ||
                    brace_stk.back() == "enum" || brace_stk.back() == "coverpoint")) {
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        }

        // --- H. Set indent for the token ---
        // At this point all pre-token indentation adjustments are complete.
        // render_tokens() will multiply fmt_indent by opts.indent_size if this
        // token begins a line.
        if (is_basic_owned) {
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
            if (tok.in_covergroup && tok.fmt_newline_before)
                tok.fmt_blank_lines = 0;
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
            for_header_stack.push_back(prev && (tok_kind(*prev) == TokenKind::ForKeyword ||
                                                tok_kind(*prev) == TokenKind::ForeachKeyword));
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
            if (tok_kind(tok) == TokenKind::DoKeyword)
                ++do_depth;

            if (tok_kind(tok) == TokenKind::ImportKeyword || tok_kind(tok) == TokenKind::ExportKeyword)
                in_import_export_decl = true;
            if (tok_kind(tok) == TokenKind::ExternKeyword)
                in_extern_decl = true;

            if (tok_kind(tok) == TokenKind::CaseKeyword || tok_kind(tok) == TokenKind::CaseXKeyword ||
                tok_kind(tok) == TokenKind::CaseZKeyword) {
                case_expr_pending = true;
                ++case_depth;
                case_conditional_depth = 0;
            }

            if (tok_kind(tok) == TokenKind::IfKeyword || tok_kind(tok) == TokenKind::ForKeyword ||
                tok_kind(tok) == TokenKind::ForeachKeyword ||
                (tok_kind(tok) == TokenKind::WhileKeyword && !do_while_tail) ||
                tok_kind(tok) == TokenKind::RepeatKeyword)
                control_expr_pending = true;

            if (tok_kind(tok) == TokenKind::ElseKeyword) {
                single_stmt_pending = true;
                pending_nl = true;
            }

            if (tok_kind(tok) == TokenKind::BeginKeyword)
                single_stmt_pending = false;

            if (is_constraint_keyword(tok))
                constraint_pend = true;

            bool import_export_function_or_task = in_import_export_decl &&
                                                  (tok_kind(tok) == TokenKind::FunctionKeyword ||
                                                   tok_kind(tok) == TokenKind::TaskKeyword);
            bool extern_function_or_task = in_extern_decl &&
                                           (tok_kind(tok) == TokenKind::FunctionKeyword ||
                                            tok_kind(tok) == TokenKind::TaskKeyword);
            bool typedef_class_forward_decl = tok_kind(tok) == TokenKind::ClassKeyword && prev &&
                                              tok_kind(*prev) == TokenKind::TypedefKeyword;
            if (is_indent_open(tok_kind(tok)) && !disable_target && !wait_fork_target &&
                !import_export_function_or_task && !extern_function_or_task &&
                !typedef_class_forward_decl) {
                int delta = (tok_kind(tok) == TokenKind::ModuleKeyword ||
                             tok_kind(tok) == TokenKind::MacromoduleKeyword ||
                             tok_kind(tok) == TokenKind::InterfaceKeyword ||
                             tok_kind(tok) == TokenKind::PackageKeyword)
                                ? opts.default_indent_level_inside_outmost_block
                                : 1;
                indent_level += delta;
                indent_stack.push_back(delta);
                if (is_block_open(tok_kind(tok)) && tok_kind(tok) != TokenKind::CaseKeyword &&
                    tok_kind(tok) != TokenKind::CaseXKeyword && tok_kind(tok) != TokenKind::CaseZKeyword &&
                    tok_kind(tok) != TokenKind::CoverGroupKeyword)
                    pending_nl = true;
            } else if (is_indent_close(tok_kind(tok))) {
                pending_nl = true;
            } else if (tok_kind(tok) == TokenKind::StructKeyword || tok_kind(tok) == TokenKind::UnionKeyword) {
                struct_pend = true;
            } else if (tok_kind(tok) == TokenKind::EnumKeyword) {
                enum_pend = true;
            } else if (tokens[i].in_covergroup &&
                       (tok_kind(tok) == TokenKind::CoverPointKeyword ||
                        tok_kind(tok) == TokenKind::CrossKeyword)) {
                coverpoint_brace_pend = true;
            }
        } else if (is_open_group(tok) && tok_is(tok, "{", TokenKind::OpenBrace)) {
            bool coverpoint_body_brace = coverpoint_brace_pend && paren_depth == 0;
            if (struct_pend || enum_pend || constraint_pend || coverpoint_body_brace ||
                (constraint_depth > 0 && single_stmt_pending)) {
                bool is_constraint_brace = constraint_pend || (constraint_depth > 0 && single_stmt_pending);
                bool is_enum_brace = enum_pend && !is_constraint_brace;
                bool is_coverpoint_brace = coverpoint_body_brace && !is_constraint_brace && !is_enum_brace;
                brace_stk.push_back(is_constraint_brace ? "constraint" :
                                    (is_enum_brace ? "enum" :
                                     (is_coverpoint_brace ? "coverpoint" : "struct")));
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
            coverpoint_brace_pend = false;
        } else if (is_close_group(tok) && tok_is(tok, "}", TokenKind::CloseBrace)) {
            if (!brace_stk.empty()) {
                if (brace_stk.back() == "constraint") {
                    constraint_depth = std::max(0, constraint_depth - 1);
                    pending_nl = true;
                } else if (brace_stk.back() == "enum") {
                    tok.fmt_newline_before = true;
                    tok.fmt_blank_lines = 0;
                } else if (brace_stk.back() == "coverpoint") {
                    pending_nl = true;
                }
                brace_stk.pop_back();
            }
        } else if (tok_is(tok, ";", TokenKind::Semicolon)) {
            in_import_export_decl = false;
            in_extern_decl = false;
            coverpoint_brace_pend = false;
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
            // A line comment consumes the rest of its physical line, so any
            // following token must render on a new line even if the original
            // formatter classification considered the comment trailing/inline.
            // Otherwise the next token can become part of the comment on the
            // second formatter pass.
            if (tok_line_comment(tok)) {
                pending_nl = true;
            } else if (tok.comment_role == CommentRole::LeadingStatement ||
                       tok.comment_role == CommentRole::OwnLineInterstitial) {
                pending_nl = true;
            } else if (tok.comment_role == CommentRole::None) {
                if (i + 1 < tokens.size() && tok_whitespace(tokens[i + 1]) &&
                    tok_text(tokens[i + 1]).find('\n') != std::string::npos)
                    pending_nl = true;
            }
        } else if (tok_kind(tok) == TokenKind::Directive) {
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
        if (tok_text(tok) == "?" && case_depth > 0 && dim_depth == 0)
            ++case_conditional_depth;
        else if (tok_is(tok, ":", TokenKind::Colon) && case_depth > 0 && dim_depth == 0) {
            if (case_conditional_depth > 0) {
                --case_conditional_depth;
            } else if (block_label_state == 0) {
                pending_nl = false;
                case_label_pending_nl = true;
            }
        } else if (tok_is(tok, ";", TokenKind::Semicolon) || tok_kind(tok) == TokenKind::EndCaseKeyword) {
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
            (tok_kind(tok) == TokenKind::BeginKeyword || tok_kind(tok) == TokenKind::ForkKeyword ||
             is_indent_close(tok_kind(tok))) &&
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

    // Normalize leading file whitespace away for Basic-owned first tokens.
    // Otherwise the first pass can preserve an input-only leading blank that the
    // second pass no longer sees.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]))
            continue;
        if (tokens[i].layout_owner == LayoutOwner::Basic) {
            tokens[i].fmt_newline_before = false;
            tokens[i].fmt_blank_lines = 0;
        }
        break;
    }

    // Final pairwise cleanup for ternary question tokens.  This pass is kept
    // local to Basic-owned tokens so layout-owner assertions still catch any
    // accidental cross-pass mutation.
    size_t prev_sig_idx = SIZE_MAX;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (prev_sig_idx != SIZE_MAX && tokens[i].layout_owner == LayoutOwner::Basic) {
            if (tok_text(tokens[prev_sig_idx]) == "?")
                tokens[i].fmt_spaces_before =
                    binary_space_after(opts.spacing.binary_operator_spacing) ? 1 : 0;
            else if (tok_text(tokens[i]) == "?")
                tokens[i].fmt_spaces_before = is_numeric(tokens[prev_sig_idx]) ? 1 :
                    (binary_space_before(opts.spacing.binary_operator_spacing) ? 1 : 0);
        }
        prev_sig_idx = i;
    }
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
                tok_kind(tokens[k]) == kind)
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
        bool starts_header = tok_kind(tokens[first]) == TokenKind::ModuleKeyword ||
                             tok_kind(tokens[first]) == TokenKind::MacromoduleKeyword ||
                             tok_kind(tokens[first]) == TokenKind::InterfaceKeyword ||
                             tok_kind(tokens[first]) == TokenKind::ProgramKeyword ||
                             tok_kind(tokens[first]) == TokenKind::TaskKeyword ||
                             tok_kind(tokens[first]) == TokenKind::FunctionKeyword;
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
                if (!comment_role_own_line_rendered(tokens[k].comment_role) &&
                    tokens[k].comment_role != CommentRole::None)
                    return false;
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
            else if (tok_is(tokens[k], "{", TokenKind::OpenBrace) || tok_kind(tokens[k]) == TokenKind::ApostropheOpenBrace) ++brace;
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
    if (open_idx < tokens.size() && tok_is(tokens[open_idx], open_text, open_kind) &&
        tokens[open_idx].matching_token != SIZE_MAX && tokens[open_idx].matching_token < end_limit &&
        tok_is(tokens[tokens[open_idx].matching_token], close_text, close_kind))
        return tokens[open_idx].matching_token;

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
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) || tok_kind(tokens[i]) == TokenKind::ApostropheOpenBrace) ++brace;
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
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) || tok_kind(tokens[i]) == TokenKind::ApostropheOpenBrace) ++brace;
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && brace > 0) --brace;
    }
    return SIZE_MAX;
}

static int token_range_width_compact(const std::vector<Tok>& tokens, size_t first, size_t end) {
    return (int)token_join_compact(tokens, first, end).size();
}

static size_t statement_end_semicolon(const std::vector<Tok>& tokens, size_t start) {
    if (start < tokens.size() && tokens[start].stmt_end != SIZE_MAX)
        return tokens[start].stmt_end;

    int paren = 0, bracket = 0, brace = 0;
    for (size_t i = start; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis)) ++paren;
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
        else if (tok_is(tokens[i], "[", TokenKind::OpenBracket)) ++bracket;
        else if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) || tok_kind(tokens[i]) == TokenKind::ApostropheOpenBrace) ++brace;
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && brace > 0) --brace;
        else if (tok_is(tokens[i], ";", TokenKind::Semicolon) && paren == 0 && bracket == 0 && brace == 0)
            return i;
    }
    return SIZE_MAX;
}

// Rewrite whitespace token text to a canonical form so that basic_formatting
// always reads the same whitespace regardless of the original source layout.
// Rule:
//   - whitespace with N newlines  → "\n" repeated N times  (spaces/tabs stripped)
//   - whitespace with no newlines → " "  (single space)
//
// Blank-line counts are preserved (newline count is kept), but leading/trailing
// spaces and mixed indentation are discarded.  Define-block whitespace is left
// untouched here because macro continuation bodies are whitespace-sensitive;
// basic_formatting/align_define_continuation_pass_v2 handle their render
// metadata without rewriting token text.
//
// Result: same non-whitespace token sequence → same fmt_* values every run
// (idempotency guarantee).
static void normalization_pass(std::vector<Tok>& tokens) {
    for (auto& tok : tokens) {
        if (tok.in_disabled_region || tok_define_block(tok))
            continue;
        if (!tok_whitespace(tok))
            continue;
        const std::string& text = tok_text(tok);
        int nl = (int)std::count(text.begin(), text.end(), '\n');
        std::string normalized = nl > 0 ? std::string(nl, '\n') : " ";
        if (normalized == text)
            continue;
        auto new_lex = std::make_shared<TokLexeme>(*tok.lex);
        new_lex->text = normalized;
        new_lex->lo   = normalized;
        tok.lex = std::move(new_lex);
    }
}

static bool comment_has_newline_between(const std::vector<Tok>& tokens, size_t first, size_t last) {
    for (size_t k = first; k < last && k < tokens.size(); ++k)
        if (tok_whitespace(tokens[k]) && tok_text(tokens[k]).find('\n') != std::string::npos)
            return true;
    return false;
}

static bool is_comment_list_leader_token(const Tok& tok) {
    return tok_is(tok, ",", TokenKind::Comma) ||
           tok_is(tok, "(", TokenKind::OpenParenthesis) ||
           tok_is(tok, "[", TokenKind::OpenBracket) ||
           tok_is(tok, "{", TokenKind::OpenBrace) ||
           tok_kind(tok) == TokenKind::ApostropheOpenBrace;
}

static bool is_statement_tail_comment_owner(const Tok& tok) {
    return tok_is(tok, ";", TokenKind::Semicolon) ||
           is_indent_close(tok_kind(tok));
}

// Classify every comment token with a stable CommentRole based on its
// structural position in the original source.  Must run AFTER
// populate_nonrender_metadata_pass (which stamps paren_depth/dim_depth and
// stmt_end) and BEFORE basic_formatting (which consumes comment_role).
//
// The central policy is that "trailing" is a statement-tail property, not
// merely "there was code earlier on the same line".  Embedded comments are
// classified as interstitial and then refined by their physical relation to
// neighboring syntax:
//   OwnLineInterstitial   newline -> comment -> newline
//   TrailingInterstitial  element -> comment -> newline
//   LeadingInterstitial   separator/newline -> comment -> element
//   InlineInterstitial    token -> comment -> token within the same element
//
// disabled-region and define-block comments keep CommentRole::None and are
// handled verbatim by basic_formatting before reaching any role-sensitive path.
static void classify_comments_pass(std::vector<Tok>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!tok_comment(tokens[i]))
            continue;
        // Disabled regions and define blocks are handled as passthrough;
        // do not classify them so basic_formatting's early-continue paths
        // remain authoritative.
        if (tokens[i].in_disabled_region || tok_define_block(tokens[i]))
            continue;

        size_t prev_code = SIZE_MAX;
        for (size_t j = i; j-- > 0; ) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]))
                continue;
            prev_code = j;
            break;
        }

        // Find the next non-whitespace, non-comment code token.
        size_t next_code = SIZE_MAX;
        for (size_t j = i + 1; j < tokens.size(); ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]))
                continue;
            next_code = j;
            break;
        }

        bool code_before_same_line =
            prev_code != SIZE_MAX && !comment_has_newline_between(tokens, prev_code + 1, i);
        bool code_after_same_line =
            next_code != SIZE_MAX && !comment_has_newline_between(tokens, i + 1, next_code);

        // --- TrailingStatement: true statement/construct tail only ---
        if (code_before_same_line && prev_code != SIZE_MAX &&
            is_statement_tail_comment_owner(tokens[prev_code])) {
            tokens[i].comment_role  = CommentRole::TrailingStatement;
            tokens[i].comment_owner = prev_code;
            continue;
        }

        bool embedded = tokens[i].paren_depth > 0 || tokens[i].dim_depth > 0 ||
                        (prev_code != SIZE_MAX && next_code != SIZE_MAX &&
                         tokens[prev_code].stmt_end != SIZE_MAX &&
                         tokens[prev_code].stmt_end == tokens[next_code].stmt_end &&
                         !is_indent_open(tok_kind(tokens[prev_code])));

        if (embedded) {
            if (!code_before_same_line && !code_after_same_line) {
                tokens[i].comment_role = CommentRole::OwnLineInterstitial;
                tokens[i].comment_owner = next_code;
            } else if (code_before_same_line && !code_after_same_line) {
                tokens[i].comment_role = CommentRole::TrailingInterstitial;
                tokens[i].comment_owner = prev_code;
            } else if (!code_before_same_line ||
                       (prev_code != SIZE_MAX && is_comment_list_leader_token(tokens[prev_code]))) {
                tokens[i].comment_role = CommentRole::LeadingInterstitial;
                tokens[i].comment_owner = next_code;
            } else {
                tokens[i].comment_role = CommentRole::InlineInterstitial;
                tokens[i].comment_owner = prev_code;
            }
            continue;
        }

        // --- LeadingStatement: own-line comment with following code ---
        if (next_code != SIZE_MAX) {
            tokens[i].comment_role  = CommentRole::LeadingStatement;
            tokens[i].comment_owner = next_code;
        }
        // else: end-of-file comment stays CommentRole::None (no owner)
    }
}

static void passthrough_regions_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    // Mark every token (including whitespace) inside a define block or disabled
    // region as passthrough so render_tokens emits them verbatim.  Whitespace
    // tokens must also be marked so their raw spacing text is preserved.
    bool in_passthrough = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        auto& tok = tokens[i];
        bool is_pt = tok.in_disabled_region || tok_define_block(tok);
        if (!is_pt) {
            in_passthrough = false;
            continue;
        }
        // Leading whitespace before the first non-whitespace token of a passthrough
        // region (e.g. indentation spaces before a `define) must not be emitted
        // verbatim: they would appear as trailing spaces on the preceding line once
        // the intervening newline token (which is outside the define range) is
        // dropped by render_tokens.  Only internal whitespace (after the region's
        // first non-whitespace token) needs to be preserved verbatim.
        if (tok_whitespace(tok)) {
            if (in_passthrough)
                tok.fmt_passthrough = true;
            continue;
        }
        tok.fmt_passthrough = true;
        if (!in_passthrough) {
            tok.fmt_newline_before = true;
            // Count blank lines from preceding whitespace
            int blank = 0;
            for (size_t j = i; j > 0; --j) {
                if (!tok_whitespace(tokens[j - 1])) break;
                int nl = (int)std::count(tok_text(tokens[j - 1]).begin(),
                                         tok_text(tokens[j - 1]).end(), '\n');
                blank = std::max(0, nl - 1);
            }
            tok.fmt_blank_lines = std::min(blank, opts.blank_lines_between_items);
        }
        in_passthrough = true;
    }
}

static void set_layout_owner_if_unowned(std::vector<Tok>& tokens, size_t first, size_t last,
                                         LayoutOwner owner) {
    for (size_t k = first; k < last && k < tokens.size(); ++k) {
        if (tok_whitespace(tokens[k]))
            continue;
        // Directive tokens (e.g. `ifdef, `endif) are always owned by Basic so
        // basic_formatting can emit them as passthrough regardless of context.
        if (tok_directive(tokens[k]))
            continue;
        if (tokens[k].layout_owner == LayoutOwner::None)
            tokens[k].layout_owner = owner;
    }
}

static void assign_layout_owners_pass(std::vector<Tok>& tokens) {
    // Strong token owners first.  Comments are intentionally owned by the
    // comment policy even when they appear inside a module header, function
    // call, or other syntactic construct; the enclosing construct may format
    // around them but should not change their standalone/inline decision.
    for (auto& tok : tokens) {
        if (tok.in_disabled_region) {
            tok.layout_owner = LayoutOwner::DisabledRegion;
        } else if (tok_define_block(tok)) {
            tok.layout_owner = LayoutOwner::DefineBlock;
        } else if (tok_whitespace(tok)) {
            continue;
        } else if (tok_comment(tok)) {
            tok.layout_owner = LayoutOwner::Comment;
        }
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].in_disabled_region || tok_define_block(tokens[i]))
            continue;
        bool header_kw = tok_kind(tokens[i]) == TokenKind::ModuleKeyword ||
                         tok_kind(tokens[i]) == TokenKind::MacromoduleKeyword ||
                         tok_kind(tokens[i]) == TokenKind::InterfaceKeyword ||
                         tok_kind(tokens[i]) == TokenKind::ProgramKeyword;
        if (!header_kw)
            continue;

        size_t port_open = SIZE_MAX;
        int paren = 0, bracket = 0, brace = 0;
        for (size_t j = i + 1; j < tokens.size(); ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) {
                size_t prev = prev_code_sig(tokens, i, j);
                if (paren == 0 && !(prev != SIZE_MAX && tok_text(tokens[prev]) == "#")) {
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
                    if (tok_kind(tokens[k]) == TokenKind::ImportKeyword)
                        has_import = true;
                size_t next = next_code_sig(tokens, j + 1, tokens.size());
                if (!(has_import && next != SIZE_MAX &&
                      (tok_is(tokens[next], "(", TokenKind::OpenParenthesis) ||
                       tok_is(tokens[next], "#", TokenKind::Hash))))
                    break;
            } else if ((tok_kind(tokens[j]) == TokenKind::EndModuleKeyword ||
                        tok_kind(tokens[j]) == TokenKind::EndInterfaceKeyword ||
                        tok_kind(tokens[j]) == TokenKind::EndProgramKeyword) && paren == 0) {
                break;
            }
        }
        if (port_open == SIZE_MAX)
            continue;
        size_t port_close = matching_close_paren(tokens, port_open, tokens.size());
        if (port_close == SIZE_MAX)
            continue;
        size_t semi = next_code_sig(tokens, port_close + 1, tokens.size());
        if (semi == SIZE_MAX || !tok_is(tokens[semi], ";", TokenKind::Semicolon))
            continue;

        size_t hash_idx = SIZE_MAX, param_open = SIZE_MAX, param_close = SIZE_MAX;
        for (size_t j = i + 1; j < port_open; ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) || tok_directive(tokens[j]))
                continue;
            if (tok_text(tokens[j]) == "#") {
                size_t n = next_code_sig(tokens, j + 1, port_open);
                if (n != SIZE_MAX && tok_is(tokens[n], "(", TokenKind::OpenParenthesis)) {
                    hash_idx = j;
                    param_open = n;
                    param_close = matching_close_paren(tokens, param_open, port_open);
                    break;
                }
            }
        }
        // Skip PortList/ParameterList ownership when the header contains PP
        // conditionals or passthrough regions — format_portlist_pass already
        // skips those headers, so their tokens stay Basic and basic_formatting
        // handles them (including the in_pp_cond passthrough paths).
        if (!token_range_has_pp_conditional(tokens, i, semi + 1) &&
            !token_range_has_line_directive(tokens, i, semi + 1) &&
            !token_range_disabled_or_passthrough(tokens, i, semi + 1)) {
            if (hash_idx != SIZE_MAX && param_close != SIZE_MAX)
                set_layout_owner_if_unowned(tokens, hash_idx, param_close + 1,
                                            LayoutOwner::ModuleHeaderParameterList);
            set_layout_owner_if_unowned(tokens, port_open, port_close + 1,
                                        LayoutOwner::ModuleHeaderPortList);
        }
        set_layout_owner_if_unowned(tokens, i, semi + 1, LayoutOwner::Basic);
        i = semi;
    }

    for (auto& tok : tokens) {
        if (!tok_whitespace(tok) && tok.layout_owner == LayoutOwner::None)
            tok.layout_owner = LayoutOwner::Basic;
    }
}

static const char* layout_owner_name(LayoutOwner owner) {
    switch (owner) {
        case LayoutOwner::None: return "None";
        case LayoutOwner::Basic: return "Basic";
        case LayoutOwner::Comment: return "Comment";
        case LayoutOwner::DisabledRegion: return "DisabledRegion";
        case LayoutOwner::DefineBlock: return "DefineBlock";
        case LayoutOwner::ModuleHeaderParameterList: return "ModuleHeaderParameterList";
        case LayoutOwner::ModuleHeaderPortList: return "ModuleHeaderPortList";
    }
    return "Unknown";
}

static bool layout_owner_allowed(LayoutOwner owner, std::initializer_list<LayoutOwner> allowed) {
    for (LayoutOwner a : allowed)
        if (owner == a)
            return true;
    return false;
}

template <typename Fn>
static void run_layout_owned_pass(const char* pass_name, std::vector<Tok>& tokens,
                                  std::initializer_list<LayoutOwner> allowed, Fn&& fn) {
    std::vector<FormatMetadataSnapshot> before;
    before.reserve(tokens.size());
    for (const auto& tok : tokens)
        before.push_back(snapshot_format_metadata(tok));

    fn();

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!format_metadata_changed(tokens[i], before[i]))
            continue;
        if (tok_whitespace(tokens[i]))
            continue;
        if (layout_owner_allowed(tokens[i].layout_owner, allowed))
            continue;
        std::ostringstream oss;
        oss << pass_name << " mutated token outside allowed layout ownership: token #"
            << i << " text '" << tok_text(tokens[i]) << "' owner "
            << layout_owner_name(tokens[i].layout_owner);
        throw std::logic_error(oss.str());
    }
}

static void populate_nonrender_metadata_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    for (auto& tok : tokens) {
        tok.matching_token = SIZE_MAX;
        tok.stmt_end = SIZE_MAX;
        tok.is_pp_conditional_directive = false;
        tok.in_pp_conditional_line_tail = false;
        tok.stmt_has_pp_conditional = false;
        tok.stmt_has_define_block = false;
        tok.in_modport = false;
        tok.in_covergroup = false;
        tok.in_function_decl = false;
        tok.in_task_decl = false;
        tok.in_module_header = false;
        tok.in_interface_header = false;
        tok.in_program_header = false;
        tok.in_class_decl = false;
        tok.in_disabled_region = false;
    }

    std::regex format_off_regex(opts.format_off_comment_pattern, std::regex::icase);
    std::regex format_on_regex(opts.format_on_comment_pattern, std::regex::icase);
    bool disabled_region = false;
    for (auto& tok : tokens) {
        if (is_format_control_comment(tok, format_off_regex)) {
            disabled_region = true;
            tok.in_disabled_region = true;
            continue;
        }
        if (is_format_control_comment(tok, format_on_regex)) {
            tok.in_disabled_region = true;
            disabled_region = false;
            continue;
        }
        tok.in_disabled_region = disabled_region;
    }

    std::vector<size_t> parens;
    std::vector<size_t> brackets;
    std::vector<size_t> braces;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis))
            parens.push_back(i);
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && !parens.empty()) {
            size_t open = parens.back();
            parens.pop_back();
            tokens[open].matching_token = i;
            tokens[i].matching_token = open;
        } else if (tok_is(tokens[i], "[", TokenKind::OpenBracket))
            brackets.push_back(i);
        else if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && !brackets.empty()) {
            size_t open = brackets.back();
            brackets.pop_back();
            tokens[open].matching_token = i;
            tokens[i].matching_token = open;
        } else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) ||
                   tok_kind(tokens[i]) == TokenKind::ApostropheOpenBrace)
            braces.push_back(i);
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && !braces.empty()) {
            size_t open = braces.back();
            braces.pop_back();
            tokens[open].matching_token = i;
            tokens[i].matching_token = open;
        }
    }

    // Stamp paren_depth and dim_depth on every token (including whitespace and
    // comments) so classify_comments_pass can detect interstitial comments
    // without re-scanning brackets.  Depth is the nesting level at entry to each
    // token: open brackets increment AFTER stamping, close brackets decrement
    // BEFORE stamping, so tokens strictly between a pair have depth > 0.
    {
        int pd = 0, dd = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && pd > 0) --pd;
            if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && dd > 0) --dd;
            tokens[i].paren_depth = pd;
            tokens[i].dim_depth   = dd;
            if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis)) ++pd;
            if (tok_is(tokens[i], "[", TokenKind::OpenBracket)) ++dd;
        }
    }

    bool pp_tail = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (pp_tail)
            tokens[i].in_pp_conditional_line_tail = true;

        if (is_line_directive(tokens[i]) && is_pp_conditional(tok_directive_kind(tokens[i]))) {
            tokens[i].is_pp_conditional_directive = true;
            tokens[i].in_pp_conditional_line_tail = false;
            pp_tail = tok_text(tokens[i]).find('\n') == std::string::npos;
            continue;
        }

        if (pp_tail && tok_whitespace(tokens[i]) && tok_text(tokens[i]).find('\n') != std::string::npos) {
            pp_tail = false;
            tokens[i].in_pp_conditional_line_tail = false;
        }
    }

    size_t stmt_start = 0;
    int paren = 0, bracket = 0, brace = 0;
    auto mark_stmt = [&](size_t first, size_t semi) {
        bool has_pp = false;
        bool has_define = false;
        for (size_t k = first; k <= semi && k < tokens.size(); ++k) {
            if (tok_whitespace(tokens[k]))
                continue;
            has_pp = has_pp || tokens[k].is_pp_conditional_directive;
            has_define = has_define || tok_define_block(tokens[k]);
        }
        for (size_t k = first; k <= semi && k < tokens.size(); ++k) {
            tokens[k].stmt_end = semi;
            tokens[k].stmt_has_pp_conditional = has_pp;
            tokens[k].stmt_has_define_block = has_define;
        }
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis)) ++paren;
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
        else if (tok_is(tokens[i], "[", TokenKind::OpenBracket)) ++bracket;
        else if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) ||
                 tok_kind(tokens[i]) == TokenKind::ApostropheOpenBrace) ++brace;
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && brace > 0) --brace;
        else if (tok_is(tokens[i], ";", TokenKind::Semicolon) && paren == 0 &&
                 bracket == 0 && brace == 0) {
            mark_stmt(stmt_start, i);
            stmt_start = i + 1;
        }
    }

    auto mark_range = [&](size_t first, size_t last, auto member) {
        for (size_t k = first; k <= last && k < tokens.size(); ++k)
            tokens[k].*member = true;
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_kind(tokens[i]) == TokenKind::ModPortKeyword) {
            size_t semi = statement_end_semicolon(tokens, i);
            if (semi != SIZE_MAX) {
                mark_range(i, semi, &Tok::in_modport);
                i = semi;
            }
        }
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_kind(tokens[i]) != TokenKind::CoverGroupKeyword)
            continue;
        size_t end = SIZE_MAX;
        for (size_t j = i + 1; j < tokens.size(); ++j) {
            if (tok_kind(tokens[j]) == TokenKind::EndGroupKeyword) {
                end = j;
                break;
            }
        }
        if (end != SIZE_MAX) {
            mark_range(i, end, &Tok::in_covergroup);
            i = end;
        }
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        TokenKind kind = tok_kind(tokens[i]);
        if (kind != TokenKind::FunctionKeyword && kind != TokenKind::TaskKeyword &&
            kind != TokenKind::ModuleKeyword && kind != TokenKind::MacromoduleKeyword &&
            kind != TokenKind::InterfaceKeyword && kind != TokenKind::ProgramKeyword &&
            kind != TokenKind::ClassKeyword)
            continue;

        size_t semi = statement_end_semicolon(tokens, i);
        if (semi == SIZE_MAX)
            continue;

        if (kind == TokenKind::FunctionKeyword) {
            mark_range(i, semi, &Tok::in_function_decl);
            i = semi;
        } else if (kind == TokenKind::TaskKeyword) {
            mark_range(i, semi, &Tok::in_task_decl);
            i = semi;
        } else if (kind == TokenKind::ModuleKeyword || kind == TokenKind::MacromoduleKeyword) {
            mark_range(i, semi, &Tok::in_module_header);
            i = semi;
        } else if (kind == TokenKind::InterfaceKeyword) {
            mark_range(i, semi, &Tok::in_interface_header);
            i = semi;
        } else if (kind == TokenKind::ProgramKeyword) {
            mark_range(i, semi, &Tok::in_program_header);
            i = semi;
        } else if (kind == TokenKind::ClassKeyword) {
            mark_range(i, semi, &Tok::in_class_decl);
            i = semi;
        }
    }
}

static void format_enum_declaration_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    const auto& eo = opts.enum_declaration;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]) ||
            tokens[i].in_disabled_region || tokens[i].fmt_passthrough || tok_kind(tokens[i]) != TokenKind::TypedefKeyword)
            continue;
        size_t semi = tokens[i].stmt_end;
        if (semi == SIZE_MAX || tokens[i].stmt_has_pp_conditional ||
            token_range_disabled_or_passthrough(tokens, i, semi + 1))
            continue;
        size_t enum_i = SIZE_MAX, open = SIZE_MAX;
        for (size_t j = i; j <= semi; ++j) {
            if (tok_kind(tokens[j]) == TokenKind::EnumKeyword)
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
                value_width = snap_to_indent_grid(item_base_col + name_width + eq_gap + value_width,
                                                  opts.indent_size) -
                              item_base_col - name_width - eq_gap;
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
            tokens[i].in_disabled_region || tokens[i].fmt_passthrough || tok_kind(tokens[i]) != TokenKind::ModPortKeyword)
            continue;
        size_t semi = tokens[i].stmt_end;
        if (semi == SIZE_MAX || tokens[i].stmt_has_pp_conditional ||
            token_range_disabled_or_passthrough(tokens, i, semi + 1))
            continue;
        auto modports = top_level_ranges_between(tokens, next_code_sig(tokens, i + 1, semi), semi);
        int base = tokens[i].fmt_indent;
        for (auto mp : modports) {
            size_t name = mp.first;
            size_t line_head = name;
            bool has_explicit_modport_keyword = false;
            if (tok_kind(tokens[name]) == TokenKind::ModPortKeyword) {
                has_explicit_modport_keyword = true;
                line_head = name;
                size_t explicit_name = next_code_sig(tokens, name + 1, mp.second);
                if (explicit_name == SIZE_MAX)
                    continue;
                name = explicit_name;
            }
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
                if (!is_modport_dir_kind(tok_kind(tokens[item.first])))
                    continue;
                dir_width = std::max(dir_width, (int)tok_text(tokens[item.first]).size() + 1);
                if (mo.align && !mo.align_adaptive)
                    sig_width = std::max(sig_width, token_range_width_compact(tokens, item.first + 1, item.second));
            }
            if (opts.tab_align) {
                int col = (base + 1) * std::max(0, opts.indent_size);
                dir_width = snap_to_indent_grid(col + dir_width, opts.indent_size) - col;
                sig_width = snap_to_indent_grid(col + dir_width + sig_width, opts.indent_size) -
                            col - dir_width;
            }
            if (mp.first == next_code_sig(tokens, i + 1, semi)) {
                tokens[name].fmt_spaces_before = 1;
            } else {
                size_t comma = prev_code_sig(tokens, i + 1, name);
                if (comma != SIZE_MAX && tok_is(tokens[comma], ",", TokenKind::Comma))
                    tokens[comma].fmt_spaces_before = 0;
                tokens[line_head].fmt_newline_before = true;
                tokens[line_head].fmt_blank_lines = 0;
                tokens[line_head].fmt_indent = base;
                tokens[line_head].fmt_spaces_before = 0;
                tokens[name].fmt_spaces_before = 0;
                if (has_explicit_modport_keyword)
                    tokens[name].fmt_spaces_before = 1;
            }
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
            tokens[i].in_disabled_region || tokens[i].fmt_passthrough || !is_identifier(tokens[i]) ||
            tok_define_block(tokens[i]))
            continue;
        size_t mod = i;
        if (is_keyword(tokens[mod]))
            continue;
        size_t j = next_code_sig(tokens, mod + 1, tokens.size());
        if (j != SIZE_MAX && tok_text(tokens[j]) == "#") {
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
        if (semi == SIZE_MAX || !tok_is(tokens[semi], ";", TokenKind::Semicolon))
            continue;

        auto pp_conditional_line_token = [&](size_t k) {
            return tokens[k].is_pp_conditional_directive || tokens[k].in_pp_conditional_line_tail;
        };

        bool protected_range = false;
        for (size_t k = i; k <= semi && k < tokens.size(); ++k) {
            if (tok_whitespace(tokens[k]))
                continue;
            if (tokens[k].in_disabled_region ||
                (tokens[k].fmt_passthrough && !pp_conditional_line_token(k))) {
                protected_range = true;
                break;
            }
        }
        if (protected_range)
            continue;

        auto ignored_or_trivia = [&](size_t k) {
            return pp_conditional_line_token(k) || tok_whitespace(tokens[k]) ||
                   tok_comment(tokens[k]) || tok_directive(tokens[k]);
        };
        auto next_sig = [&](size_t first, size_t end) {
            for (size_t k = first; k < end && k < tokens.size(); ++k)
                if (!ignored_or_trivia(k))
                    return k;
            return SIZE_MAX;
        };
        auto prev_sig = [&](size_t first, size_t before) {
            if (before > tokens.size())
                before = tokens.size();
            for (size_t n = before; n > first; --n) {
                size_t k = n - 1;
                if (!ignored_or_trivia(k))
                    return k;
            }
            return SIZE_MAX;
        };
        auto matching_paren = [&](size_t op, size_t end) {
            if (op < tokens.size() && tokens[op].matching_token != SIZE_MAX &&
                tokens[op].matching_token < end)
                return tokens[op].matching_token;
            int depth = 0;
            for (size_t k = op; k < end && k < tokens.size(); ++k) {
                if (ignored_or_trivia(k))
                    continue;
                if (tok_is(tokens[k], "(", TokenKind::OpenParenthesis))
                    ++depth;
                else if (tok_is(tokens[k], ")", TokenKind::CloseParenthesis)) {
                    --depth;
                    if (depth == 0)
                        return k;
                }
            }
            return SIZE_MAX;
        };

        struct Range { size_t first, second, comma; };
        std::vector<Range> ports;
        size_t start = next_sig(open + 1, close);
        int paren = 0, bracket_depth = 0, brace = 0;
        for (size_t k = open + 1; k < close && k < tokens.size(); ++k) {
            if (ignored_or_trivia(k))
                continue;
            if (tok_is(tokens[k], "(", TokenKind::OpenParenthesis)) ++paren;
            else if (tok_is(tokens[k], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
            else if (tok_is(tokens[k], "[", TokenKind::OpenBracket)) ++bracket_depth;
            else if (tok_is(tokens[k], "]", TokenKind::CloseBracket) && bracket_depth > 0) --bracket_depth;
            else if (tok_is(tokens[k], "{", TokenKind::OpenBrace) ||
                     tok_kind(tokens[k]) == TokenKind::ApostropheOpenBrace) ++brace;
            else if (tok_is(tokens[k], "}", TokenKind::CloseBrace) && brace > 0) --brace;
            else if (tok_is(tokens[k], ",", TokenKind::Comma) && paren == 0 &&
                     bracket_depth == 0 && brace == 0) {
                size_t end = prev_sig(start, k);
                if (start != SIZE_MAX && end != SIZE_MAX && end < k)
                    ports.push_back({start, end + 1, k});
                start = next_sig(k + 1, close);
            }
        }
        if (start != SIZE_MAX) {
            size_t end = prev_sig(start, close);
            if (end != SIZE_MAX && end >= start)
                ports.push_back({start, end + 1, SIZE_MAX});
        }
        if (ports.empty())
            continue;

        bool named = true;
        int max_port = 0, max_sig = 0;
        struct P { size_t dot, name, op, cl, comma; int sigw; };
        std::vector<P> ps;
        for (auto r : ports) {
            size_t dot = r.first;
            if (tok_text(tokens[dot]) != ".") { named = false; break; }
            size_t name = next_sig(dot + 1, r.second);
            size_t op = next_sig(name == SIZE_MAX ? r.second : name + 1, r.second);
            if (name == SIZE_MAX || op == SIZE_MAX || !tok_is(tokens[op], "(", TokenKind::OpenParenthesis)) { named = false; break; }
            size_t cl = matching_paren(op, r.second);
            if (cl == SIZE_MAX) { named = false; break; }
            size_t comma = r.comma;
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
        int name_field_width = tab_aligned_width(opts.instance.instance_port_name_width, opts);
        int eff_before = std::max(1, name_field_width - max_port - 1);
        int eff_inside = std::max(0, opts.instance.instance_port_between_paren_width - max_sig);
        for (const auto& p : ps) {
            tokens[p.dot].fmt_newline_before = true;
            tokens[p.dot].fmt_blank_lines = 0;
            tokens[p.dot].fmt_indent = base + port_indent;
            tokens[p.dot].fmt_spaces_before = 0;
            tokens[p.name].fmt_spaces_before = 0;
            int before = opts.instance.align_adaptive ? std::max(1, name_field_width - (int)tok_text(tokens[p.name]).size() - 1) : eff_before + (max_port - (int)tok_text(tokens[p.name]).size());
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


static int rendered_column_after_token(const std::vector<Tok>& tokens, size_t line_start,
                                       size_t token_idx, const FormatOptions& opts) {
    int col = 0;
    for (size_t i = line_start; i <= token_idx && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]))
            continue;
        if (i == line_start || tokens[i].fmt_newline_before)
            col = tokens[i].fmt_indent * std::max(0, opts.indent_size);
        col += tokens[i].fmt_spaces_before;
        col += (int)tok_text(tokens[i]).size();
    }
    return col;
}

static void column_to_indent_spaces(int col, const FormatOptions& opts, int& indent,
                                    int& spaces) {
    int indent_size = std::max(0, opts.indent_size);
    if (indent_size > 0) {
        indent = col / indent_size;
        spaces = col - indent * indent_size;
    } else {
        indent = 0;
        spaces = col;
    }
}

static std::vector<std::pair<size_t, size_t>> function_arg_ranges_between(
    const std::vector<Tok>& tokens, size_t first, size_t last);

static void format_arg_list_metadata(std::vector<Tok>& tokens, size_t open, size_t close,
                                     int base_indent, int hanging_indent, bool hanging,
                                     int hanging_spaces = 0, int block_base_spaces = 0) {
    auto args = function_arg_ranges_between(tokens, open + 1, close);
    if (args.empty())
        return;
    auto first_arg_code = [&](size_t first, size_t end) {
        for (size_t k = first; k < end && k < tokens.size(); ++k) {
            if (!tok_whitespace(tokens[k]) && !tok_comment(tokens[k]) &&
                (!tok_directive(tokens[k]) || is_macro_usage(tokens[k])))
                return k;
        }
        return SIZE_MAX;
    };
    auto place_arg_start = [&](std::pair<size_t, size_t> r, bool newline, int indent,
                               int spaces) {
        tokens[r.first].fmt_newline_before = newline;
        tokens[r.first].fmt_blank_lines = 0;
        tokens[r.first].fmt_indent = indent;
        tokens[r.first].fmt_spaces_before = spaces;

        if (!tok_comment(tokens[r.first]))
            return;

        size_t code = first_arg_code(r.first + 1, r.second);
        if (code == SIZE_MAX)
            return;

        if (tokens[r.first].comment_role == CommentRole::OwnLineInterstitial) {
            tokens[code].fmt_newline_before = true;
            tokens[code].fmt_blank_lines = 0;
            tokens[code].fmt_indent = indent;
            tokens[code].fmt_spaces_before = spaces;
        } else if (tokens[r.first].comment_role == CommentRole::LeadingInterstitial) {
            tokens[code].fmt_newline_before = false;
            tokens[code].fmt_spaces_before = 1;
        }
    };
    auto place_gap_comments = [&](int indent, int spaces) {
        for (size_t k = open + 1; k < close && k < tokens.size(); ++k) {
            if (!tok_comment(tokens[k]))
                continue;
            if (tokens[k].comment_role == CommentRole::TrailingInterstitial) {
                tokens[k].fmt_newline_before = false;
                tokens[k].fmt_spaces_before = 1;
            } else if (tokens[k].comment_role == CommentRole::OwnLineInterstitial) {
                tokens[k].fmt_newline_before = true;
                tokens[k].fmt_blank_lines = 0;
                tokens[k].fmt_indent = indent;
                tokens[k].fmt_spaces_before = spaces;
            }
        }
    };
    bool trailing_line_comment = false;
    for (size_t k = args.back().second; k < close && k < tokens.size(); ++k) {
        if (tok_line_comment(tokens[k])) {
            trailing_line_comment = true;
            break;
        }
    }
    if (hanging) {
        bool leading_line_comment = false;
        if (tok_line_comment(tokens[args[0].first]) &&
            (tokens[args[0].first].comment_role == CommentRole::OwnLineInterstitial ||
             tokens[args[0].first].comment_role == CommentRole::LeadingInterstitial))
            leading_line_comment = true;
        for (size_t k = open + 1; k < args[0].first && k < tokens.size(); ++k) {
            if (tok_line_comment(tokens[k])) {
                leading_line_comment = true;
                break;
            }
        }
        if (leading_line_comment) {
            // Do not pull the first real argument up after a leading // comment.
            // That would make the argument part of the comment text on the next
            // formatter run, changing the token stream even when the rendered
            // normalized layout looked identical.
            place_arg_start(args[0], true, hanging_indent, hanging_spaces);
        } else {
            place_arg_start(args[0], false, hanging_indent, 0);
        }
        for (size_t k = 1; k < args.size(); ++k) {
            place_arg_start(args[k], true, hanging_indent, hanging_spaces);
        }
        tokens[close].fmt_newline_before = trailing_line_comment;
        tokens[close].fmt_blank_lines = 0;
        tokens[close].fmt_indent = base_indent;
        tokens[close].fmt_spaces_before = 0;
    } else {
        for (auto r : args) {
            place_arg_start(r, true, base_indent + 1, block_base_spaces);
        }
        tokens[close].fmt_newline_before = true;
        tokens[close].fmt_blank_lines = 0;
        tokens[close].fmt_indent = base_indent;
        tokens[close].fmt_spaces_before = block_base_spaces;
    }
    place_gap_comments(hanging ? hanging_indent : base_indent + 1,
                       hanging ? hanging_spaces : block_base_spaces);
    for (auto r : args) {
        size_t comma = find_top_level_token(tokens, r.second, close, ",", TokenKind::Comma);
        if (comma != SIZE_MAX)
            tokens[comma].fmt_spaces_before = 0;
    }
}

static bool function_arg_skip_token(const Tok& tok) {
    bool structural_arg_comment =
        tok_comment(tok) &&
        (tok.comment_role == CommentRole::OwnLineInterstitial ||
         tok.comment_role == CommentRole::LeadingInterstitial);
    return tok_whitespace(tok) || (tok_comment(tok) && !structural_arg_comment) ||
           tok.is_pp_conditional_directive || tok.in_pp_conditional_line_tail ||
           (tok_directive(tok) && !is_macro_usage(tok));
}

static size_t next_function_arg_sig(const std::vector<Tok>& tokens, size_t first,
                                    size_t end) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (!function_arg_skip_token(tokens[i]))
            return i;
    }
    return SIZE_MAX;
}

static size_t prev_function_arg_sig(const std::vector<Tok>& tokens, size_t first,
                                    size_t before) {
    if (before > tokens.size())
        before = tokens.size();
    for (size_t i = before; i > first; --i) {
        size_t idx = i - 1;
        if (!function_arg_skip_token(tokens[idx]))
            return idx;
    }
    return SIZE_MAX;
}

static std::vector<std::pair<size_t, size_t>> function_arg_ranges_between(
    const std::vector<Tok>& tokens, size_t first, size_t last) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t start = next_function_arg_sig(tokens, first, last);
    int paren = 0, bracket = 0, brace = 0;
    for (size_t i = first; i < last && i < tokens.size(); ++i) {
        if (function_arg_skip_token(tokens[i]))
            continue;
        if (tok_is(tokens[i], "(", TokenKind::OpenParenthesis)) ++paren;
        else if (tok_is(tokens[i], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
        else if (tok_is(tokens[i], "[", TokenKind::OpenBracket)) ++bracket;
        else if (tok_is(tokens[i], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
        else if (tok_is(tokens[i], "{", TokenKind::OpenBrace) ||
                 tok_kind(tokens[i]) == TokenKind::ApostropheOpenBrace) ++brace;
        else if (tok_is(tokens[i], "}", TokenKind::CloseBrace) && brace > 0) --brace;
        else if (tok_is(tokens[i], ",", TokenKind::Comma) && paren == 0 &&
                 bracket == 0 && brace == 0) {
            size_t end = prev_function_arg_sig(tokens, start, i);
            if (start != SIZE_MAX && end != SIZE_MAX && end < i)
                ranges.push_back({start, i});
            start = next_function_arg_sig(tokens, i + 1, last);
        }
    }
    if (start != SIZE_MAX) {
        size_t end = prev_function_arg_sig(tokens, start, last);
        if (end != SIZE_MAX && end >= start)
            ranges.push_back({start, end + 1});
    }
    return ranges;
}

static size_t find_simple_call_tokens(const std::vector<Tok>& tokens, size_t first, size_t end,
                                      size_t& name, size_t& open, size_t& close,
                                      bool use_global_prev) {
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) ||
            (tok_directive(tokens[i]) && !is_macro_usage(tokens[i])))
            continue;
        bool callable = is_identifier(tokens[i]) || starts_with_chars(tok_text(tokens[i]), '`') ||
                        starts_with_chars(tok_text(tokens[i]), '$');
        if (!callable || is_function_call_skip_token(tokens[i]))
            continue;
        size_t prev = prev_code_sig(tokens, use_global_prev ? 0 : first, i);
        if (prev != SIZE_MAX && tok_text(tokens[prev]) == ".") {
            size_t before_dot = prev_code_sig(tokens, first, prev);
            bool member_call = before_dot != SIZE_MAX &&
                               (is_identifier(tokens[before_dot]) ||
                                tok_is(tokens[before_dot], ")", TokenKind::CloseParenthesis) ||
                                tok_is(tokens[before_dot], "]", TokenKind::CloseBracket));
            if (!member_call)
                continue;
        }
        if (prev != SIZE_MAX && (is_identifier(tokens[prev]) ||
                                 tok_is(tokens[prev], ")", TokenKind::CloseParenthesis) ||
                                 tok_is(tokens[prev], "]", TokenKind::CloseBracket)))
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

static bool is_format_function_call_name(const Tok& tok, const MacroClassifier& macros) {
    if (!is_macro_usage(tok))
        return true;
    MacroClassification cls = classify_macro(tok, true, macros);
    return cls.role == MacroRole::FunctionLikeExpr && !cls.whitespace_sensitive;
}

static bool has_unsafe_macro_call_argument(const std::vector<Tok>& tokens, size_t first,
                                           size_t last, const MacroClassifier& macros) {
    for (size_t k = first; k < last && k < tokens.size(); ++k) {
        if (!is_macro_usage(tokens[k]))
            continue;
        size_t next = next_code_sig(tokens, k + 1, last);
        bool has_args = next != SIZE_MAX &&
                        tok_is(tokens[next], "(", TokenKind::OpenParenthesis);
        MacroClassification cls = classify_macro(tokens[k], has_args, macros);
        if (cls.whitespace_sensitive ||
            (has_args && cls.role == MacroRole::FunctionLikeExpr) ||
            cls.role == MacroRole::StatementLike ||
            cls.role == MacroRole::DeclarationLike ||
            cls.role == MacroRole::ControlFlowLike ||
            cls.role == MacroRole::BlockBeginLike ||
            cls.role == MacroRole::BlockEndLike)
            return true;
    }
    return false;
}

static void format_function_calls_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    const auto& fo = opts.function;
    MacroClassifier macro_classifier(opts.macros);

    // Statement-range path for calls that are already split across physical
    // lines, plus calls with macro arguments.  The line-based pass below cannot
    // see the whole call in cases like:
    //
    //   foo(a,
    //       b,
    //       c);
    //
    // because each physical line is scanned independently.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].in_disabled_region || tokens[i].fmt_passthrough || tokens[i].in_modport ||
            tok_define_block(tokens[i]) || tok_whitespace(tokens[i]) || tok_comment(tokens[i]))
            continue;
        size_t semi = tokens[i].stmt_end;
        if (semi == SIZE_MAX ||
            token_range_disabled_or_non_pp_passthrough(tokens, i, semi + 1) ||
            tokens[i].stmt_has_define_block)
            continue;
        size_t name, open, close;
        if (find_simple_call_tokens(tokens, i, semi + 1, name, open, close, true) == SIZE_MAX) {
            i = semi; // No call in statement — skip to avoid O(stmt_len²) rescanning
            continue;
        }
        if (!is_format_function_call_name(tokens[name], macro_classifier) || tokens[name].in_modport) {
            i = name; // Skip past this non-qualifying identifier
            continue;
        }
        if (tokens[name].in_function_decl || tokens[name].in_task_decl ||
            tokens[name].in_module_header || tokens[name].in_class_decl) {
            i = name;
            continue;
        }
        if (has_unsafe_macro_call_argument(tokens, open + 1, close, macro_classifier)) {
            i = name;
            continue;
        }
        auto args = function_arg_ranges_between(tokens, open + 1, close);
        // Decide from the current normalized/rendered layout only.  Do not
        // inspect whitespace token text here: whitespace tokens preserve source
        // tokenization details that render_tokens() ignores, so using them makes
        // this pass depend on pre-format history even when the rendered input to
        // the pass is identical.
        bool already_multiline = tokens[close].fmt_newline_before;
        for (auto r : args)
            already_multiline = already_multiline || tokens[r.first].fmt_newline_before;
        if (!already_multiline || args.size() <= 1) {
            i = name;
            continue;
        }
        bool do_break = false;
        if (fo.break_policy == "always") do_break = true;
        else if (fo.break_policy == "auto") do_break = fo.arg_count >= 0 && (int)args.size() >= fo.arg_count;
        tokens[open].fmt_spaces_before = fo.space_before_paren ? 1 : 0;
        if (!do_break || fo.break_policy == "never") {
            if (!args.empty() && fo.space_inside_paren)
                tokens[args.front().first].fmt_spaces_before = 1;
            tokens[close].fmt_spaces_before = (!args.empty() && fo.space_inside_paren) ? 1 : 0;
            i = semi;
            continue;
        }
        int hang = tokens[name].fmt_indent;
        int hang_spaces = 0;
        bool hanging = fo.layout == "hanging";
        if (hanging)
            column_to_indent_spaces(rendered_column_after_token(tokens, i, open, opts), opts,
                                    hang, hang_spaces);
        int block_base = tokens[name].fmt_indent;
        int block_base_spaces = 0;
        if (!hanging) {
            int name_col = rendered_column_after_token(tokens, i, name, opts) -
                           (int)tok_text(tokens[name]).size();
            column_to_indent_spaces(name_col, opts, block_base, block_base_spaces);
        }
        format_arg_list_metadata(tokens, open, close, block_base, hang, hanging,
                                 hang_spaces, block_base_spaces);
        i = semi;
    }

    auto starts = tok_line_starts(tokens);
    for (size_t li = 0; li < starts.size(); ++li) {
        size_t s = starts[li], e = (li + 1 < starts.size()) ? starts[li + 1] : tokens.size();
        bool line_in_modport = false;
        for (size_t k = s; k < e && k < tokens.size(); ++k)
            line_in_modport = line_in_modport || tokens[k].in_modport;
        if (line_in_modport || token_range_disabled_or_passthrough(tokens, s, e) ||
            token_range_has_pp_conditional(tokens, s, e) || token_range_has_define_block(tokens, s, e))
            continue;
        size_t name, open, close;
        if (find_simple_call_tokens(tokens, s, e, name, open, close, false) == SIZE_MAX ||
            !is_format_function_call_name(tokens[name], macro_classifier))
            continue;
        if (tokens[name].in_function_decl || tokens[name].in_task_decl ||
            tokens[name].in_module_header || tokens[name].in_class_decl)
            continue;
        auto args = function_arg_ranges_between(tokens, open + 1, close);
        if (has_unsafe_macro_call_argument(tokens, open + 1, close, macro_classifier))
            continue;
        bool do_break = false;
        if (fo.break_policy == "always") do_break = !args.empty();
        else if (fo.break_policy == "auto") do_break = fo.arg_count >= 0 && (int)args.size() >= fo.arg_count;
        tokens[open].fmt_spaces_before = fo.space_before_paren ? 1 : 0;
        if (!do_break || fo.break_policy == "never") {
            if (!args.empty() && fo.space_inside_paren)
                tokens[args.front().first].fmt_spaces_before = 1;
            tokens[close].fmt_spaces_before = (!args.empty() && fo.space_inside_paren) ? 1 : 0;
            continue;
        }
        int hang = tokens[name].fmt_indent;
        int hang_spaces = 0;
        if (fo.layout == "hanging")
            column_to_indent_spaces(rendered_column_after_token(tokens, s, open, opts), opts,
                                    hang, hang_spaces);
        int block_base = tokens[name].fmt_indent;
        int block_base_spaces = 0;
        if (fo.layout != "hanging") {
            int name_col = rendered_column_after_token(tokens, s, name, opts) -
                           (int)tok_text(tokens[name]).size();
            column_to_indent_spaces(name_col, opts, block_base, block_base_spaces);
        }
        format_arg_list_metadata(tokens, open, close, block_base, hang,
                                 fo.layout == "hanging", hang_spaces, block_base_spaces);
    }
}

static void format_covergroup_pass(std::vector<Tok>& tokens, const FormatOptions& opts) {
    int brace_depth = 0;
    int cover_base = 0;
    auto coverpoint_block_open = [&](size_t coverpoint, size_t limit) -> size_t {
        int paren = 0, bracket = 0, brace = 0;
        for (size_t j = coverpoint + 1; j < limit && j < tokens.size(); ++j) {
            if (tok_whitespace(tokens[j]) || tok_comment(tokens[j]) ||
                (tok_directive(tokens[j]) && !is_macro_usage(tokens[j])))
                continue;
            if (tok_is(tokens[j], ";", TokenKind::Semicolon) && paren == 0 &&
                bracket == 0 && brace == 0)
                return SIZE_MAX;
            if (tok_kind(tokens[j]) == TokenKind::EndGroupKeyword && paren == 0 &&
                bracket == 0 && brace == 0)
                return SIZE_MAX;
            if (tok_is(tokens[j], "(", TokenKind::OpenParenthesis)) ++paren;
            else if (tok_is(tokens[j], ")", TokenKind::CloseParenthesis) && paren > 0) --paren;
            else if (tok_is(tokens[j], "[", TokenKind::OpenBracket)) ++bracket;
            else if (tok_is(tokens[j], "]", TokenKind::CloseBracket) && bracket > 0) --bracket;
            else if (tok_is(tokens[j], "{", TokenKind::OpenBrace) ||
                     tok_kind(tokens[j]) == TokenKind::ApostropheOpenBrace) {
                if (paren == 0 && bracket == 0 && brace == 0)
                    return j;
                ++brace;
            } else if (tok_is(tokens[j], "}", TokenKind::CloseBrace) && brace > 0) {
                --brace;
            }
        }
        return SIZE_MAX;
    };

    auto force_coverpoint_block = [&](size_t point, size_t open, size_t close) {
        int base = tokens[point].fmt_indent;
        if (opts.statement.begin_newline) {
            tokens[open].fmt_newline_before = true;
            tokens[open].fmt_blank_lines = 0;
            tokens[open].fmt_indent = base;
            tokens[open].fmt_spaces_before = 0;
        } else {
            tokens[open].fmt_newline_before = false;
            tokens[open].fmt_spaces_before = 1;
        }

        for (size_t j = open + 1; j < close && j < tokens.size(); ++j) {
            if (tok_kind(tokens[j]) == TokenKind::BinsKeyword ||
                tok_kind(tokens[j]) == TokenKind::IllegalBinsKeyword ||
                tok_kind(tokens[j]) == TokenKind::IgnoreBinsKeyword) {
                tokens[j].fmt_newline_before = true;
                tokens[j].fmt_blank_lines = 0;
                tokens[j].fmt_indent = base + 1;
                tokens[j].fmt_spaces_before = 0;
            }
        }
        size_t first_body = SIZE_MAX;
        for (size_t j = open + 1; j < close && j < tokens.size(); ++j) {
            if (!tok_whitespace(tokens[j]) && !tok_comment(tokens[j]) &&
                (!tok_directive(tokens[j]) || is_macro_usage(tokens[j]))) {
                first_body = j;
                break;
            }
        }
        if (first_body != SIZE_MAX && is_macro_usage(tokens[first_body])) {
            tokens[first_body].fmt_newline_before = true;
            tokens[first_body].fmt_blank_lines = 0;
            tokens[first_body].fmt_indent = base + 1;
            tokens[first_body].fmt_spaces_before = 0;
        }

        tokens[close].fmt_newline_before = true;
        tokens[close].fmt_blank_lines = 0;
        tokens[close].fmt_indent = base;
        tokens[close].fmt_spaces_before = 0;

        size_t next = next_code_sig(tokens, close + 1, tokens.size());
        if (next != SIZE_MAX && tok_kind(tokens[next]) != TokenKind::Semicolon &&
            tok_kind(tokens[next]) != TokenKind::EndGroupKeyword) {
            tokens[next].fmt_newline_before = true;
            tokens[next].fmt_blank_lines = 0;
            tokens[next].fmt_indent = base;
            tokens[next].fmt_spaces_before = 0;
        }
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tok_whitespace(tokens[i]) || tok_comment(tokens[i]) || tok_directive(tokens[i]))
            continue;
        if (tok_kind(tokens[i]) == TokenKind::CoverGroupKeyword) {
            cover_base = tokens[i].fmt_indent;
        } else if (tok_kind(tokens[i]) == TokenKind::EndGroupKeyword) {
            tokens[i].fmt_indent = cover_base;
            brace_depth = 0;
        } else if (tokens[i].in_covergroup &&
                   (tok_kind(tokens[i]) == TokenKind::CoverPointKeyword ||
                    tok_kind(tokens[i]) == TokenKind::CrossKeyword) &&
                   !tokens[i].in_disabled_region && !tokens[i].fmt_passthrough) {
            size_t open = coverpoint_block_open(i, tokens.size());
            if (open != SIZE_MAX) {
                size_t close = matching_close_token(tokens, open, tokens.size(), "{", "}",
                                                    TokenKind::OpenBrace,
                                                    TokenKind::CloseBrace);
                if (close != SIZE_MAX && !token_range_has_pp_conditional(tokens, open, close + 1)) {
                    force_coverpoint_block(i, open, close);
                    i = close;
                    continue;
                }
            }
        } else if (tokens[i].in_covergroup && !tokens[i].in_disabled_region &&
                   !tokens[i].fmt_passthrough) {
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
        if (tok_kind(tokens[i]) != TokenKind::DistKeyword || tokens[i].in_disabled_region || tokens[i].fmt_passthrough)
            continue;
        size_t open = next_code_sig(tokens, i + 1, tokens.size());
        if (open == SIZE_MAX || !tok_is(tokens[open], "{", TokenKind::OpenBrace))
            continue;
        size_t close = matching_close_token(tokens, open, tokens.size(), "{", "}", TokenKind::OpenBrace, TokenKind::CloseBrace);
        if (close == SIZE_MAX || token_range_has_pp_conditional(tokens, i, close + 1))
            continue;
        auto items = function_arg_ranges_between(tokens, open + 1, close);
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
        if (first == SIZE_MAX || (tok_kind(tokens[first]) != TokenKind::FunctionKeyword && tok_kind(tokens[first]) != TokenKind::TaskKeyword) ||
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
    const std::string& input = source;
    write_log(opts, "format_source_input.sv", input);

    auto tokens = collect_lexer_tokens(input);
    write_log(opts, "00_input.sv", input);
    populate_nonrender_metadata_pass(tokens, opts);
    // classify_comments_pass runs after populate (paren_depth/dim_depth ready)
    // and before normalization so it sees original source newlines in whitespace
    // token text.  The result is stored in comment_role/comment_owner and is
    // never recomputed — all subsequent passes treat it as read-only.
    classify_comments_pass(tokens);
    assign_layout_owners_pass(tokens);
    normalization_pass(tokens);
    write_log(opts, "01_normalization_pass.sv", render_tokens(tokens, opts));
    run_layout_owned_pass("passthrough_regions_pass", tokens,
                          {LayoutOwner::DefineBlock, LayoutOwner::DisabledRegion},
                          [&] { passthrough_regions_pass(tokens, opts); });
    run_layout_owned_pass("basic_formatting", tokens, {LayoutOwner::Basic},
                          [&] { basic_formatting(tokens, input, opts); });
    write_log(opts, "02_basic_formatting.sv", render_tokens(tokens, opts));

    run_layout_owned_pass("format_comment_layout_pass", tokens,
                          {LayoutOwner::Comment},
                          [&] { format_comment_layout_pass(tokens, opts); });

    // Module-header reflow mutates token metadata directly.
    run_layout_owned_pass("format_class_extends_parameter_pass", tokens,
                          {LayoutOwner::Basic},
                          [&] { format_class_extends_parameter_pass(tokens, opts); });
    write_log(opts, "03_format_class_extends_parameter_pass.sv", render_tokens(tokens, opts));
    run_layout_owned_pass("format_portlist_pass", tokens,
                          {LayoutOwner::ModuleHeaderParameterList,
                           LayoutOwner::ModuleHeaderPortList},
                          [&] { format_portlist_pass(tokens, opts); });
    write_log(opts, "04_format_portlist_pass.sv", render_tokens(tokens, opts));

    if (opts.statement.align) {
        run_layout_owned_pass("align_assign_pass", tokens,
                              {LayoutOwner::Basic, LayoutOwner::Comment,
                               LayoutOwner::ModuleHeaderParameterList},
                              [&] { align_assign_pass(tokens, opts); });
        write_log(opts, "05_align_assign_pass.sv", render_tokens(tokens, opts));
    }
    if (opts.var_declaration.align) {
        run_layout_owned_pass("align_var_pass", tokens,
                              {LayoutOwner::Basic, LayoutOwner::Comment},
                              [&] { align_var_pass(tokens, opts); });
        write_log(opts, "06_align_var_pass.sv", render_tokens(tokens, opts));
    }
    if (opts.port_declaration.align) {
        run_layout_owned_pass("align_port_pass", tokens,
                              {LayoutOwner::Basic, LayoutOwner::Comment,
                               LayoutOwner::ModuleHeaderPortList},
                              [&] { align_port_pass(tokens, opts); });
        write_log(opts, "07_align_port_pass.sv", render_tokens(tokens, opts));
    }
    // Must run after all alignment passes so fmt_spaces_before for define-block tokens
    // reflects the final aligned spacing, giving a stable content_col for \ placement.
    // align_define_continuation_pass_v2(tokens, opts);
    // write_log(opts, "08_align_define_continuation_pass.sv", render_tokens(tokens, opts));

    run_layout_owned_pass("format_enum_declaration_pass", tokens,
                          {LayoutOwner::Basic, LayoutOwner::Comment},
                          [&] { format_enum_declaration_pass(tokens, opts); });
    write_log(opts, "09_format_enum_declaration_pass.sv", render_tokens(tokens, opts));
    run_layout_owned_pass("format_modport_pass", tokens,
                          {LayoutOwner::Basic, LayoutOwner::Comment},
                          [&] { format_modport_pass(tokens, opts); });
    write_log(opts, "10_format_modport_pass.sv", render_tokens(tokens, opts));
    if (opts.instance.align) {
        run_layout_owned_pass("expand_instances_pass", tokens,
                              {LayoutOwner::Basic, LayoutOwner::Comment},
                              [&] { expand_instances_pass(tokens, opts); });
        write_log(opts, "11_expand_instances_pass.sv", render_tokens(tokens, opts));
    }
    run_layout_owned_pass("format_function_calls_pass", tokens,
                          {LayoutOwner::Basic, LayoutOwner::Comment,
                           LayoutOwner::ModuleHeaderParameterList,
                           LayoutOwner::ModuleHeaderPortList},
                          [&] { format_function_calls_pass(tokens, opts); });
    write_log(opts, "12_format_function_calls_pass.sv", render_tokens(tokens, opts));
    run_layout_owned_pass("format_covergroup_pass", tokens,
                          {LayoutOwner::Basic, LayoutOwner::Comment},
                          [&] { format_covergroup_pass(tokens, opts); });
    write_log(opts, "13_format_covergroup_pass.sv", render_tokens(tokens, opts));
    run_layout_owned_pass("format_constraint_dist_pass", tokens,
                          {LayoutOwner::Basic, LayoutOwner::Comment},
                          [&] { format_constraint_dist_pass(tokens, opts); });
    write_log(opts, "14_format_constraint_dist_pass.sv", render_tokens(tokens, opts));
    run_layout_owned_pass("format_function_declaration_pass", tokens,
                          {LayoutOwner::Basic, LayoutOwner::Comment},
                          [&] { format_function_declaration_pass(tokens, opts); });
    write_log(opts, "15_format_function_declaration_pass.sv", render_tokens(tokens, opts));

    std::string out = render_tokens(tokens, opts);

    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    out += '\n';

    verify_safe_mode_unchanged(source, out, opts);

    return out;
}
