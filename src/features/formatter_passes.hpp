#pragma once

#include "formatter_token.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace svfmt {

using TK = slang::parsing::TokenKind;

inline bool text_is(const Tok& t, const char* s) { return t.lex->text == s; }
inline bool kind_is(const Tok& t, TK k) { return t.lex->kind == k; }
inline bool is_passthrough(const Tok& t) { return t.mutable_.macro.passthrough || t.lex->is_whitespace_sensitive; }
inline int token_width(const Tok& t) { return static_cast<int>(t.lex->text.size()); }

inline bool is_control_keyword(TK k) {
    return k == TK::IfKeyword || k == TK::ForKeyword || k == TK::ForeachKeyword ||
           k == TK::WhileKeyword || k == TK::CaseKeyword || k == TK::CaseXKeyword ||
           k == TK::CaseZKeyword || k == TK::RepeatKeyword;
}
inline bool is_open_block(TK k) {
    return k == TK::BeginKeyword || k == TK::ClassKeyword || k == TK::FunctionKeyword ||
           k == TK::TaskKeyword || k == TK::CaseKeyword || k == TK::CaseXKeyword ||
           k == TK::CaseZKeyword || k == TK::OpenBrace ||
           k == TK::GenerateKeyword || k == TK::CoverGroupKeyword ||
           k == TK::PropertyKeyword || k == TK::SequenceKeyword || k == TK::CheckerKeyword ||
           k == TK::ClockingKeyword || k == TK::ConfigKeyword || k == TK::PrimitiveKeyword ||
           k == TK::SpecifyKeyword;
}
inline bool is_outer_open(TK k) {
    return k == TK::ModuleKeyword || k == TK::InterfaceKeyword || k == TK::PackageKeyword ||
           k == TK::MacromoduleKeyword || k == TK::ProgramKeyword;
}
inline bool is_close_block(TK k) {
    return k == TK::EndKeyword || k == TK::EndClassKeyword || k == TK::EndFunctionKeyword ||
           k == TK::EndTaskKeyword || k == TK::EndCaseKeyword || k == TK::CloseBrace ||
           k == TK::EndGenerateKeyword || k == TK::EndGroupKeyword || k == TK::EndPropertyKeyword ||
           k == TK::EndSequenceKeyword || k == TK::EndCheckerKeyword || k == TK::EndClockingKeyword ||
           k == TK::EndConfigKeyword || k == TK::EndPrimitiveKeyword || k == TK::EndSpecifyKeyword ||
           k == TK::EndTableKeyword;
}
inline bool is_outer_close(TK k) {
    return k == TK::EndModuleKeyword || k == TK::EndInterfaceKeyword || k == TK::EndPackageKeyword ||
           k == TK::EndProgramKeyword;
}
inline bool is_assignment_op(TK k) {
    return k == TK::Equals || k == TK::LessThanEquals ||
           k == TK::PlusEqual || k == TK::MinusEqual ||
           k == TK::StarEqual || k == TK::SlashEqual || k == TK::PercentEqual ||
           k == TK::AndEqual || k == TK::OrEqual || k == TK::XorEqual ||
           k == TK::LeftShiftEqual || k == TK::RightShiftEqual ||
           k == TK::TripleLeftShiftEqual || k == TK::TripleRightShiftEqual;
}
inline bool is_binary_op(TK k) {
    return k == TK::Plus || k == TK::Minus || k == TK::Star || k == TK::Slash || k == TK::Percent ||
           k == TK::DoubleEquals || k == TK::ExclamationEquals || k == TK::LessThan || k == TK::GreaterThan ||
           k == TK::LessThanEquals || k == TK::GreaterThanEquals || k == TK::DoubleAnd || k == TK::DoubleOr ||
           k == TK::And || k == TK::Or || k == TK::Xor || k == TK::LeftShift || k == TK::RightShift ||
           k == TK::TripleLeftShift || k == TK::TripleRightShift ||
           k == TK::TildeAnd || k == TK::TildeOr || k == TK::TildeXor || k == TK::XorTilde ||
           k == TK::InsideKeyword;
}
inline bool no_space_before(TK k) {
    return k == TK::CloseParenthesis || k == TK::CloseBracket || k == TK::CloseBrace ||
           k == TK::Semicolon || k == TK::Comma || k == TK::Dot || k == TK::DoubleColon ||
           k == TK::PlusColon || k == TK::MinusColon;
}
inline bool no_space_after(TK k) {
    return k == TK::OpenParenthesis || k == TK::OpenBracket || k == TK::OpenBrace ||
           k == TK::ApostropheOpenBrace || k == TK::IntegerBase ||
           k == TK::Dot || k == TK::DoubleColon || k == TK::Hash || k == TK::Apostrophe;
}
inline bool wants_before(const std::string& mode) { return mode == "before" || mode == "both"; }
inline bool wants_after(const std::string& mode) { return mode == "after" || mode == "both"; }

inline bool is_single_stmt_control(TK k) {
    return k == TK::IfKeyword || k == TK::ForKeyword || k == TK::ForeachKeyword ||
           k == TK::WhileKeyword || k == TK::RepeatKeyword;
}
inline bool is_unary_op(TK k) {
    return k == TK::Tilde || k == TK::Exclamation ||
           k == TK::TildeAnd || k == TK::TildeOr || k == TK::TildeXor || k == TK::XorTilde ||
           k == TK::DoublePlus || k == TK::DoubleMinus;
}
inline bool is_type_keyword(TK k) {
    return k == TK::LogicKeyword || k == TK::WireKeyword || k == TK::RegKeyword ||
           k == TK::BitKeyword || k == TK::ByteKeyword || k == TK::ShortIntKeyword ||
           k == TK::IntKeyword || k == TK::LongIntKeyword || k == TK::IntegerKeyword ||
           k == TK::RealKeyword || k == TK::RealTimeKeyword || k == TK::ShortRealKeyword ||
           k == TK::TimeKeyword || k == TK::StringKeyword || k == TK::CHandleKeyword ||
           k == TK::EventKeyword || k == TK::VoidKeyword ||
           k == TK::SignedKeyword || k == TK::UnsignedKeyword || k == TK::PackedKeyword;
}
inline bool is_port_direction(TK k) {
    return k == TK::InputKeyword || k == TK::OutputKeyword ||
           k == TK::InOutKeyword || k == TK::RefKeyword;
}
inline bool is_numeric(const Tok& t) {
    return t.lex->kind == TK::IntegerLiteral || t.lex->kind == TK::IntegerBase ||
           t.lex->kind == TK::UnbasedUnsizedLiteral || t.lex->kind == TK::RealLiteral ||
           t.lex->kind == TK::TimeLiteral;
}
inline bool is_identifier_like(const Tok& t) {
    return t.lex->kind == TK::Identifier || t.lex->kind == TK::SystemIdentifier ||
           t.lex->kind == TK::MacroUsage;
}

// SyntaxPass is the early fact-freeze pass.  It writes SyntaxFacts,
// TopologyFacts, and CommentFacts from immutable lexemes/input trivia.  These
// are parser-ish facts, not formatter decisions; downstream formatting-policy
// passes must not mutate them.
class SyntaxPass final : public IFormatPass {
public:
    const char* name() const override { return "syntax"; }
    void run(TokenStream& tokens) override {
        std::vector<size_t> parens, brackets, braces;
        int pd = 0, bd = 0, brd = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            t.immutable.syntax.paren_depth = pd;
            t.immutable.syntax.bracket_depth = bd;
            t.immutable.syntax.brace_depth = brd;
            // Freeze input-line topology before any wrap decision exists.
            t.immutable.topology.begins_line_construct = t.immutable.input_trivia.starts_original_line;
            t.immutable.topology.ends_line_construct = kind_is(t, TK::Semicolon) || kind_is(t, TK::Comma) ||
                                             (t.lex->is_comment && t.lex->text.rfind("//", 0) == 0);
            t.immutable.topology.opens_indent_scope = is_open_block(t.lex->kind) || is_outer_open(t.lex->kind);
            t.immutable.topology.closes_indent_scope = is_close_block(t.lex->kind) || is_outer_close(t.lex->kind);

            if (kind_is(t, TK::OpenParenthesis)) {
                const Tok* prev = i == 0 ? nullptr : &tokens[i - 1];
                t.immutable.topology.starts_parameter_list = prev && kind_is(*prev, TK::Hash);
                t.immutable.topology.starts_argument_list = prev &&
                    (kind_is(*prev, TK::Identifier) || kind_is(*prev, TK::SystemIdentifier) ||
                     kind_is(*prev, TK::MacroUsage) || kind_is(*prev, TK::CloseParenthesis));
                t.immutable.topology.starts_port_list = prev &&
                    (kind_is(*prev, TK::ModuleKeyword) || kind_is(*prev, TK::InterfaceKeyword));
            }
            // ends_argument_list is set later when matching token is known (see below)

            if (t.lex->is_comment) {
                t.immutable.comment.role = t.immutable.input_trivia.starts_original_line ? CommentRole::OwnLine : CommentRole::Trailing;
                t.immutable.comment.anchor_token = i == 0 ? npos : i - 1;
                t.immutable.comment.inside_expression = pd > 0 || bd > 0 || brd > 0;
                t.immutable.comment.inside_arg_list = pd > 0;
            }
            if (t.lex->is_directive) {
                (void)0; // pp-conditional tracking reserved for future use
            }
            if (kind_is(t, TK::OpenParenthesis)) { parens.push_back(i); ++pd; }
            else if (kind_is(t, TK::CloseParenthesis)) { if (!parens.empty()) { auto j = parens.back(); parens.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; t.immutable.topology.ends_argument_list = tokens[j].immutable.topology.starts_argument_list; } pd = std::max(0, pd - 1); }
            else if (kind_is(t, TK::OpenBracket)) { brackets.push_back(i); ++bd; }
            else if (kind_is(t, TK::CloseBracket)) { if (!brackets.empty()) { auto j = brackets.back(); brackets.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } bd = std::max(0, bd - 1); }
            else if (kind_is(t, TK::OpenBrace)) { braces.push_back(i); ++brd; }
            else if (kind_is(t, TK::CloseBrace)) { if (!braces.empty()) { auto j = braces.back(); braces.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } brd = std::max(0, brd - 1); }
        }
        size_t stmt_start = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (kind_is(tokens[i], TK::Semicolon) || kind_is(tokens[i], TK::Comma)) {
                for (size_t j = stmt_start; j <= i && j < tokens.size(); ++j) { tokens[j].immutable.syntax.stmt_begin = stmt_start; tokens[j].immutable.syntax.stmt_end = i; }
                stmt_start = i + 1;
            }
        }
    }
};

// MacroClassifier + MacroRole — used by MacroPass to categorise macro tokens.
enum class MacroRole { ObjectLikeExpr, FunctionLikeExpr, StatementLike, DeclarationLike, ControlFlowLike, BlockBeginLike, BlockEndLike };

struct MacroClassifier {
    std::unordered_set<std::string> statement_like;
    std::unordered_set<std::string> declaration_like;
    std::unordered_set<std::string> control_flow_like;
    std::unordered_set<std::string> block_begin_like;
    std::unordered_set<std::string> block_end_like;
    std::unordered_set<std::string> whitespace_sensitive;

    explicit MacroClassifier(const MacroOptions& m) {
        auto add = [](std::unordered_set<std::string>& dst, const std::vector<std::string>& src) {
            for (const auto& n : src) {
                std::string s = n;
                if (!s.empty() && s[0] == '`') s = s.substr(1);
                dst.insert(s);
            }
        };
        add(statement_like,    m.statement_like);
        add(declaration_like,  m.declaration_like);
        add(control_flow_like, m.control_flow_like);
        add(block_begin_like,  m.block_begin_like);
        add(block_end_like,    m.block_end_like);
        add(whitespace_sensitive, m.whitespace_sensitive);
    }

    static std::string extract_name(const std::string& raw_text) {
        std::string name = raw_text;
        if (!name.empty() && name[0] == '`') name = name.substr(1);
        size_t end = 0;
        while (end < name.size() && (std::isalnum((unsigned char)name[end]) || name[end] == '_' || name[end] == '$'))
            ++end;
        return name.substr(0, end);
    }

    MacroRole classify(const std::string& raw_text) const {
        std::string name = extract_name(raw_text);
        if (block_end_like.count(name))    return MacroRole::BlockEndLike;
        if (block_begin_like.count(name))  return MacroRole::BlockBeginLike;
        if (control_flow_like.count(name)) return MacroRole::ControlFlowLike;
        if (declaration_like.count(name))  return MacroRole::DeclarationLike;
        if (statement_like.count(name))    return MacroRole::StatementLike;
        return MacroRole::ObjectLikeExpr;
    }

    bool is_whitespace_sensitive(const std::string& raw_text) const {
        return whitespace_sensitive.count(extract_name(raw_text)) > 0;
    }
};

// MacroPass owns MacroMetadata.  It never changes wrapping or spacing directly;
// it only marks regions/macros that downstream passes must treat conservatively.
class MacroPass final : public IFormatPass {
public:
    explicit MacroPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "macro"; }
    void run(TokenStream& tokens) override {
        MacroClassifier mc(opts_.macros);
        for (auto& t : tokens) {
            if (t.lex->is_whitespace_sensitive) {
                t.mutable_.macro.passthrough = true;
                t.mutable_.macro.suppress_alignment = true;
                t.mutable_.macro.suppress_wrapping = true;
            } else if (t.lex->is_directive) {
                t.mutable_.macro.suppress_alignment = true;
            } else if (t.lex->kind == TK::MacroUsage) {
                if (mc.is_whitespace_sensitive(t.lex->text)) {
                    t.mutable_.macro.passthrough = true;
                    t.mutable_.macro.suppress_alignment = true;
                    t.mutable_.macro.suppress_wrapping = true;
                } else {
                    t.mutable_.macro.suppress_alignment = true;
                    MacroRole role = mc.classify(t.lex->text);
                    if (role == MacroRole::StatementLike || role == MacroRole::DeclarationLike ||
                        role == MacroRole::ControlFlowLike || role == MacroRole::BlockBeginLike ||
                        role == MacroRole::BlockEndLike) {
                        t.mutable_.macro.suppress_wrapping = false;
                    }
                    if (role == MacroRole::BlockBeginLike)
                        t.mutable_.macro.opens_indent_scope = true;
                    if (role == MacroRole::BlockEndLike)
                        t.mutable_.macro.closes_indent_scope = true;
                    if (role == MacroRole::StatementLike || role == MacroRole::DeclarationLike ||
                        role == MacroRole::BlockBeginLike || role == MacroRole::BlockEndLike)
                        t.mutable_.macro.force_line_break = true;
                }
            }
        }
    }
private:
    const FormatOptions& opts_;
};

// WrapPass owns WrapMetadata and reads syntax/macro metadata.  It uses stable
// syntax/source facts only; there is no dependency on rendered columns.
class WrapPass final : public IFormatPass {
public:
    explicit WrapPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "wrap"; }
    void run(TokenStream& tokens) override {
        int group = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (t.mutable_.macro.suppress_wrapping) continue;
            t.mutable_.wrap.wrap_group = group;
            if (i == 0 || t.immutable.input_trivia.starts_original_line || t.immutable.input_trivia.original_newlines_before > 1) t.mutable_.wrap.must_break_before = true;
            if (t.lex->is_comment && t.immutable.comment.role == CommentRole::OwnLine) t.mutable_.wrap.must_break_before = true;
            // PP directives (ifdef/endif/else/define/…) are always on their own line.
            if (t.lex->is_directive) {
                t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = true;
            }
            // Force line break after statement-/declaration-/block-like macro invocations.
            // If the macro is followed by '(args)', break after the matching ')'; otherwise
            // break after the macro token itself.
            if (kind_is(t, TK::MacroUsage) && t.mutable_.macro.force_line_break) {
                size_t break_at = i;
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (tokens[j].lex->is_comment) continue;
                    if (kind_is(tokens[j], TK::OpenParenthesis) &&
                        tokens[j].immutable.syntax.matching_token != npos)
                        break_at = tokens[j].immutable.syntax.matching_token;
                    break;
                }
                tokens[break_at].mutable_.wrap.must_break_after = true;
            }
            if (i > 0 && tokens[i - 1].lex->is_comment && tokens[i - 1].lex->text.rfind("//", 0) == 0) t.mutable_.wrap.must_break_before = true;
            if (kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth == 0) {
                t.mutable_.wrap.must_break_after = true;
                ++group;
                // If a trailing comment immediately follows on the same line,
                // defer the line break to after the comment so it renders inline.
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (is_passthrough(tokens[j])) continue;
                    if (tokens[j].lex->is_comment &&
                        !tokens[j].immutable.input_trivia.starts_original_line) {
                        t.mutable_.wrap.must_break_after = false;
                        tokens[j].mutable_.wrap.must_break_after = true;
                    }
                    break;
                }
            }
            if (kind_is(t, TK::Comma)) t.mutable_.wrap.can_break_after = true;
            // Close-block keywords always start a new line; CloseBrace only when
            // not inside a parenthesised expression (e.g. `inside {A, B}`).
            if (is_outer_close(t.lex->kind) ||
                (is_close_block(t.lex->kind) &&
                 !(kind_is(t, TK::CloseBrace) && t.immutable.syntax.paren_depth > 0)))
                t.mutable_.wrap.must_break_before = true;
            if (opts_.statement.begin_newline && (kind_is(t, TK::BeginKeyword) || kind_is(t, TK::OpenBrace))) t.mutable_.wrap.must_break_before = true;
            if (opts_.statement.wrap_end_else_clauses && kind_is(t, TK::ElseKeyword) && i > 0 && (kind_is(tokens[i - 1], TK::EndKeyword) || kind_is(tokens[i - 1], TK::CloseBrace))) t.mutable_.wrap.must_break_before = true;
            // else always breaks unless wrap_end_else_clauses handled it above
            if (kind_is(t, TK::ElseKeyword)) {
                bool prev_is_end_or_brace = (i > 0 && (kind_is(tokens[i-1], TK::EndKeyword) || kind_is(tokens[i-1], TK::CloseBrace)));
                if (!prev_is_end_or_brace)
                    t.mutable_.wrap.must_break_before = true;
            }
        }
    }
private: const FormatOptions& opts_;
};

// IndentPass owns IndentMetadata and reads wrap/source facts.  The level is a
// deterministic stack over token kinds, so format(format(x)) recomputes the same
// levels from canonical output.
class IndentPass final : public IFormatPass {
public:
    explicit IndentPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "indent"; }
    void run(TokenStream& tokens) override {
        int level = 0;
        // Declaration-qualifier tracking: `import "DPI-C" function/task` and
        // `extern function/task` are header-only declarations — their function/
        // task keyword must NOT open an indent scope.  `typedef class` forward
        // declarations must NOT open a class scope either.
        bool in_import  = false; // active from ImportKeyword until ;
        bool in_extern  = false; // active from ExternKeyword until ;
        bool in_typedef = false; // active from TypedefKeyword until ;

        // Single-statement control-flow indent state
        bool ctrl_expr_pending  = false;  // seen if/for/while/foreach/repeat, waiting for (
        int  ctrl_paren_open    = 0;      // paren_depth when ( was pushed for control expr
        bool single_stmt_pending = false; // control expr closed, waiting for begin or non-begin
        bool single_stmt_active  = false; // extra +1 indent applied; waiting for ;
        int  single_stmt_paren_depth = 0; // paren depth at single_stmt start (for ; detection)
        int  paren_depth = 0;

        bool ctrl_just_closed = false; // defers single_stmt_pending resolution by one token

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (is_passthrough(t)) continue;

            // Track parens for single-stmt detection
            if (kind_is(t, TK::OpenParenthesis)) ++paren_depth;
            else if (kind_is(t, TK::CloseParenthesis) && paren_depth > 0) --paren_depth;

            // Detect control expression start
            if (is_single_stmt_control(t.lex->kind)) {
                ctrl_expr_pending = true;
            }
            if (ctrl_expr_pending && kind_is(t, TK::OpenParenthesis)) {
                ctrl_expr_pending = false;
                ctrl_paren_open = paren_depth; // depth after increment above
            }
            // Detect control expression close — set pending but don't resolve yet (defer to next token)
            if (kind_is(t, TK::CloseParenthesis) && ctrl_paren_open > 0 && paren_depth == ctrl_paren_open - 1) {
                ctrl_paren_open = 0;
                single_stmt_pending = true;
                ctrl_just_closed = true;
            }

            // Track declaration-qualifier context
            if (kind_is(t, TK::ImportKeyword))  in_import  = true;
            if (kind_is(t, TK::ExternKeyword))  in_extern  = true;
            if (kind_is(t, TK::TypedefKeyword)) in_typedef = true;
            if (kind_is(t, TK::Semicolon))      { in_import = false; in_extern = false; in_typedef = false; }

            // Compute indent — close tokens first so they dedent before assignment
            bool closes = is_close_block(t.lex->kind) || is_outer_close(t.lex->kind) ||
                          t.mutable_.macro.closes_indent_scope;
            if (closes) level = std::max(0, level - 1);

            // Resolve single-stmt pending on first non-passthrough, non-comment token AFTER control expr close
            if (single_stmt_pending && !ctrl_just_closed && !t.lex->is_comment) {
                single_stmt_pending = false;
                bool is_block = kind_is(t, TK::BeginKeyword) || kind_is(t, TK::OpenBrace);
                if (!is_block && !closes) {
                    level++;
                    single_stmt_active = true;
                    single_stmt_paren_depth = paren_depth;
                    t.mutable_.wrap.must_break_before = true;
                }
            }
            ctrl_just_closed = false;

            t.mutable_.indent.base_indent = level * opts_.indent_size;
            if (is_outer_close(t.lex->kind))
                t.mutable_.indent.base_indent = 0;
            else if (is_outer_open(t.lex->kind) && opts_.default_indent_level_inside_outmost_block == 0)
                t.mutable_.indent.base_indent = 0; // OuterOpen itself is at outer level
            if (t.mutable_.wrap.continuation)
                t.mutable_.indent.continuation_indent = opts_.indent_size;

            // Open scope after assigning indent to the opener token.
            // Suppress for function/task used as qualifiers in import/extern
            // declarations, and for class used in typedef forward declarations.
            if (!closes) {
                bool is_qualifier_fn_task =
                    (in_import || in_extern) &&
                    (kind_is(t, TK::FunctionKeyword) || kind_is(t, TK::TaskKeyword));
                bool is_typedef_class =
                    in_typedef && kind_is(t, TK::ClassKeyword);
                if (!is_qualifier_fn_task && !is_typedef_class &&
                    (is_open_block(t.lex->kind) ||
                     (is_outer_open(t.lex->kind) && opts_.default_indent_level_inside_outmost_block > 0) ||
                     t.mutable_.macro.opens_indent_scope))
                    ++level;
            }

            // Close single-stmt at semicolon at the right depth
            if (single_stmt_active && kind_is(t, TK::Semicolon) && paren_depth == single_stmt_paren_depth) {
                level = std::max(0, level - 1);
                single_stmt_active = false;
            }
        }
    }
private: const FormatOptions& opts_;
};

// AlignPass owns AlignMetadata.  Groups consecutive lines with assignment
// operators at the same indent level and aligns them.
class AlignPass final : public IFormatPass {
public:
    explicit AlignPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "alignment"; }
    void run(TokenStream& tokens) override {
        if (!opts_.statement.align) return;

        struct Line {
            size_t first{npos};
            size_t end{npos};
            size_t assign_idx{npos};
            int    lhs_width{0};
            int    indent{0};
            bool   disabled{false};
        };

        std::vector<Line> lines;
        size_t cur_first = npos;
        size_t cur_start = 0;

        auto push_line = [&](size_t end_idx) {
            Line ln;
            ln.first = cur_first;
            ln.end = end_idx;
            if (cur_first != npos)
                ln.indent = tokens[cur_first].mutable_.indent.base_indent;
            // Find assignment op at depth 0
            int pd = 0, bd = 0, brd = 0;
            size_t scan_start = (cur_first != npos ? cur_first : cur_start);
            for (size_t k = scan_start; k < end_idx; ++k) {
                auto& tok = tokens[k];
                if (is_passthrough(tok)) { ln.disabled = true; continue; }
                if (tok.lex->is_comment) continue;
                if (kind_is(tok, TK::OpenParenthesis))  ++pd;
                else if (kind_is(tok, TK::CloseParenthesis) && pd > 0)  --pd;
                else if (kind_is(tok, TK::OpenBracket))   ++bd;
                else if (kind_is(tok, TK::CloseBracket)  && bd > 0)   --bd;
                else if (kind_is(tok, TK::OpenBrace))    ++brd;
                else if (kind_is(tok, TK::CloseBrace)   && brd > 0)   --brd;
                if (pd == 0 && bd == 0 && brd == 0 &&
                    !tok.mutable_.macro.suppress_alignment &&
                    is_assignment_op(tok.lex->kind)) {
                    ln.assign_idx = k;
                    break;
                }
            }
            // Compute LHS width
            if (ln.assign_idx != npos) {
                int w = 0; bool first = true;
                for (size_t k = scan_start; k < ln.assign_idx; ++k) {
                    auto& tok = tokens[k];
                    if (is_passthrough(tok) || tok.lex->is_comment) continue;
                    if (first) { w += token_width(tok); first = false; }
                    else { w += tok.mutable_.space.spaces_before + token_width(tok); }
                }
                ln.lhs_width = w;
            }
            lines.push_back(ln);
            cur_first = npos;
            cur_start = end_idx;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& tok = tokens[i];
            if (is_passthrough(tok)) continue;
            if (tok.mutable_.wrap.must_break_before && i > 0) {
                push_line(i);
            }
            if (cur_first == npos) cur_first = i;
        }
        if (cur_first != npos) push_line(tokens.size());

        // Group consecutive alignable lines and align
        const bool space_before = wants_before(opts_.spacing.assignment_operator_spacing);
        const int  min_sp = space_before ? 1 : 0;

        size_t li = 0;
        while (li < lines.size()) {
            if (lines[li].disabled || lines[li].assign_idx == npos) { ++li; continue; }
            int base_indent = lines[li].indent;
            // Collect group of consecutive lines with same indent that have assign ops
            size_t j = li;
            while (j < lines.size() && !lines[j].disabled && lines[j].assign_idx != npos && lines[j].indent == base_indent)
                ++j;
            if (j - li >= 2) {
                int max_lhs = opts_.statement.lhs_min_width;
                for (size_t k = li; k < j; ++k)
                    max_lhs = std::max(max_lhs, lines[k].lhs_width);
                if (opts_.statement.align_adaptive)
                    max_lhs = opts_.statement.lhs_min_width;
                int target = max_lhs + (space_before ? 1 : 2);
                for (size_t k = li; k < j; ++k) {
                    int sp = std::max(min_sp, target - lines[k].lhs_width);
                    tokens[lines[k].assign_idx].mutable_.space.spaces_before = sp;
                }
            } else if (j - li == 1 && opts_.statement.lhs_min_width > 0) {
                int target = opts_.statement.lhs_min_width + (space_before ? 1 : 2);
                int sp = std::max(min_sp, target - lines[li].lhs_width);
                tokens[lines[li].assign_idx].mutable_.space.spaces_before = sp;
            }
            li = j;
        }
    }
private: const FormatOptions& opts_;
};

// CommentPass owns CommentMetadata and reads stable syntax comment roles.
class CommentPass final : public IFormatPass {
public:
    const char* name() const override { return "comment"; }
    void run(TokenStream& tokens) override {
        for (auto& t : tokens)
            if (t.lex->is_comment && t.immutable.comment.role == CommentRole::OwnLine)
                t.mutable_.comment.force_own_line = true;
    }
};

// SpacingPass owns SpaceMetadata.  It reads upstream wrap/comment metadata to
// avoid assigning in-line spaces before tokens that will start a line.
class SpacingPass final : public IFormatPass {
public:
    explicit SpacingPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "spacing"; }
    void run(TokenStream& tokens) override {
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (i == 0 || t.mutable_.wrap.must_break_before || t.mutable_.comment.force_own_line || is_passthrough(t)) {
                t.mutable_.space.spaces_before = 0;
                continue;
            }
            const Tok& L = tokens[i - 1];
            int spaces = 1;

            // Basic no-space rules
            if (no_space_before(t.lex->kind) || no_space_after(L.lex->kind)) spaces = 0;
            // Empty positional argument: `, ,` — keep one space so the slot is visible
            if (kind_is(t, TK::Comma) && kind_is(L, TK::Comma)) spaces = 1;

            // Unary ops: no space after.
            // Exception: `~ &a`, `~ |a`, `~ ^a` — tilde followed by a
            // reduction operator must keep a space so it isn't read as
            // the compound `~&` / `~|` / `~^` operator.
            if (is_unary_op(L.lex->kind)) {
                bool tilde_before_reduction = kind_is(L, TK::Tilde) &&
                    (kind_is(t, TK::And) || kind_is(t, TK::Or) || kind_is(t, TK::Xor));
                if (!tilde_before_reduction) {
                    t.mutable_.space.spaces_before = 0;
                    t.mutable_.space.suppress_space = true;
                    continue;
                }
            }

            // Hierarchy: . and ::
            if (kind_is(L, TK::Dot) || kind_is(L, TK::DoubleColon)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }
            if (kind_is(t, TK::Dot) || kind_is(t, TK::DoubleColon)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // # hash: no space before next token
            if (kind_is(L, TK::Hash)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // No space before '[' when it's an index/dimension on an identifier or closer
            if (kind_is(t, TK::OpenBracket) &&
                (is_identifier_like(L) || kind_is(L, TK::CloseBracket) || kind_is(L, TK::CloseParenthesis)))
                spaces = 0;

            // Function/task call spacing
            if (kind_is(t, TK::OpenParenthesis) && (kind_is(L, TK::Identifier) || kind_is(L, TK::SystemIdentifier) || kind_is(L, TK::MacroUsage)))
                spaces = opts_.function.space_before_paren ? 1 : 0;
            if (kind_is(t, TK::OpenParenthesis) && is_control_keyword(L.lex->kind))
                spaces = opts_.spacing.control_keyword_space ? 1 : 0;
            if ((kind_is(L, TK::OpenParenthesis) || kind_is(t, TK::CloseParenthesis)) && opts_.spacing.space_inside_parens) spaces = 1;
            // function.space_inside_paren: space inside argument-list parens only
            if (kind_is(L, TK::OpenParenthesis) && L.immutable.topology.starts_argument_list && opts_.function.space_inside_paren) spaces = 1;
            if (kind_is(t, TK::CloseParenthesis) && t.immutable.topology.ends_argument_list && opts_.function.space_inside_paren) spaces = 1;
            if ((kind_is(L, TK::OpenBracket) || kind_is(t, TK::CloseBracket)) && opts_.spacing.space_inside_dimension_brackets) spaces = 1;

            // } brace: 1 space after (unless followed by ; or ,)
            if (kind_is(L, TK::CloseBrace) && !kind_is(t, TK::Semicolon) && !kind_is(t, TK::Comma)) spaces = 1;

            // Apostrophe / cast: no space
            if (kind_is(t, TK::Apostrophe) || kind_is(L, TK::Apostrophe)) spaces = 0;
            // '{ casts (text starts with ')
            if (!t.lex->text.empty() && t.lex->text[0] == '\'') spaces = 0;

            // Postfix ++ / --: no space before when attached to an identifier, ], or )
            if ((kind_is(t, TK::DoublePlus) || kind_is(t, TK::DoubleMinus)) &&
                (is_identifier_like(L) || kind_is(L, TK::CloseBracket) || kind_is(L, TK::CloseParenthesis)))
                spaces = 0;

            // Assignment operators.
            // LessThanEquals is context-sensitive: inside parens it's a comparison, not
            // non-blocking assignment.  Treat it as a regular binary op in that context.
            auto is_assign = [&](const Tok& tok) {
                if (tok.lex->kind == TK::LessThanEquals && tok.immutable.syntax.paren_depth > 0)
                    return false;
                return is_assignment_op(tok.lex->kind);
            };
            if (is_assign(t)) spaces = wants_before(opts_.spacing.assignment_operator_spacing) ? 1 : 0;
            if (is_assign(L)) spaces = wants_after(opts_.spacing.assignment_operator_spacing) ? 1 : 0;

            // Binary operators (non-assignment).
            // Closing brackets carry depth=1 (before decrement), so exclude them from the
            // L-side dim check to avoid treating tokens after ] as inside a dimension.
            bool in_dim = t.immutable.syntax.bracket_depth > 0 ||
                          (L.immutable.syntax.bracket_depth > 0 && !kind_is(L, TK::CloseBracket));
            const std::string& bop_mode = in_dim ? opts_.spacing.dimension_binary_operator_spacing : opts_.spacing.binary_operator_spacing;
            if (is_binary_op(t.lex->kind) && !is_assign(t))
                spaces = wants_before(bop_mode) ? 1 : 0;
            if (is_binary_op(L.lex->kind) && !is_assign(L))
                spaces = wants_after(bop_mode) ? 1 : 0;
            // `inside` is a keyword operator — always needs spaces regardless of bop_mode
            if (kind_is(t, TK::InsideKeyword)) spaces = 1;
            if (kind_is(L, TK::InsideKeyword)) spaces = 1;

            // Range/part-select
            if (kind_is(t, TK::Colon) && in_dim) spaces = wants_before(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (kind_is(L, TK::Colon) && in_dim) spaces = wants_after(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (kind_is(t, TK::PlusColon) || kind_is(t, TK::MinusColon)) spaces = wants_before(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;
            if (kind_is(L, TK::PlusColon) || kind_is(L, TK::MinusColon)) spaces = wants_after(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;

            // wait keyword: no space before ( (like a function call, not a control keyword)
            if (kind_is(t, TK::OpenParenthesis) && kind_is(L, TK::WaitKeyword)) spaces = 0;

            // @ event control spacing.
            // Standalone delay-control `@ (...)` appears after `;` — suppress spacing for it.
            // Only `always @`, `always_ff @`, `always_comb @`, `always_latch @`, `covergroup @`
            // etc. get the event-control spacing applied.
            if (kind_is(t, TK::At)) {
                bool standalone = kind_is(L, TK::Semicolon);
                spaces = (!standalone && wants_before(opts_.spacing.procedural_event_control_at_spacing)) ? 1 : 0;
            }
            if (kind_is(L, TK::At)) {
                bool standalone = i >= 2 && kind_is(tokens[i-2], TK::Semicolon);
                spaces = (!standalone && wants_after(opts_.spacing.procedural_event_control_at_spacing)) ? 1 : 0;
            }
            // space_inside_event_control_parens: add space inside ( ) of procedural event control.
            // Only applies when ( directly follows @ which is not a standalone delay control.
            if (opts_.spacing.space_inside_event_control_parens) {
                if (kind_is(L, TK::OpenParenthesis) && i >= 2 && kind_is(tokens[i-2], TK::At)) {
                    bool standalone = i >= 3 && kind_is(tokens[i-3], TK::Semicolon);
                    if (!standalone) spaces = 1;
                }
                if (kind_is(t, TK::CloseParenthesis) && t.immutable.syntax.matching_token != npos) {
                    size_t j = t.immutable.syntax.matching_token;
                    if (j >= 1 && j < tokens.size() && kind_is(tokens[j], TK::OpenParenthesis) &&
                        j >= 1 && kind_is(tokens[j-1], TK::At)) {
                        bool standalone = j >= 2 && kind_is(tokens[j-2], TK::Semicolon);
                        if (!standalone) spaces = 1;
                    }
                }
            }

            // End-label colon: `endclass: Foo`, `endfunction: bar`, etc. — no space before `:`
            if (kind_is(t, TK::Colon) && (is_close_block(L.lex->kind) || is_outer_close(L.lex->kind) ||
                                           kind_is(L, TK::BeginKeyword) || kind_is(L, TK::ForkKeyword)))
                spaces = 0;

            // semicolon_spacing: controls space before/after `;` inside for-loop headers
            // (paren_depth > 0 identifies the for(;;) context vs statement-ending `;`)
            if (kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth > 0)
                spaces = wants_before(opts_.spacing.semicolon_spacing) ? 1 : 0;
            if (kind_is(L, TK::Semicolon) && L.immutable.syntax.paren_depth > 0)
                spaces = wants_after(opts_.spacing.semicolon_spacing) ? 1 : 0;

            t.mutable_.space.spaces_before = std::max(0, spaces);
            t.mutable_.space.suppress_space = spaces == 0;
        }
    }
private: const FormatOptions& opts_;
};

// BlankLinePass owns BlankLineMetadata and clamps source blank lines to config.
class BlankLinePass final : public IFormatPass {
public:
    explicit BlankLinePass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "blank_line"; }
    void run(TokenStream& tokens) override {
        for (auto& t : tokens)
            if (t.immutable.input_trivia.original_newlines_before > 1)
                t.mutable_.blank.before = std::min(t.immutable.input_trivia.original_newlines_before - 1,
                                                   std::max(0, opts_.blank_lines_between_items));
    }
private: const FormatOptions& opts_;
};

} // namespace svfmt
