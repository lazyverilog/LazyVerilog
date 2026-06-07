#pragma once

#include "formatter_token.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace svfmt {

using TK = slang::parsing::TokenKind;

inline bool kind_is(const Tok& t, TK k) { return t.lex.kind == k; }
inline bool is_passthrough(const Tok& t) { return t.mutable_.macro.passthrough || t.lex.is_whitespace_sensitive; }
inline int token_width(const Tok& t) { return static_cast<int>(t.lex.text.size()); }

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
           k == TK::SpecifyKeyword || k == TK::ForkKeyword;
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
           k == TK::EndTableKeyword || k == TK::JoinKeyword || k == TK::JoinAnyKeyword ||
           k == TK::JoinNoneKeyword;
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
           k == TK::WhileKeyword || k == TK::RepeatKeyword || k == TK::ForeverKeyword;
}
inline bool is_procedural_block_keyword(TK k) {
    return k == TK::InitialKeyword || k == TK::FinalKeyword ||
           k == TK::AlwaysKeyword || k == TK::AlwaysCombKeyword ||
           k == TK::AlwaysFFKeyword || k == TK::AlwaysLatchKeyword;
}
inline bool is_unary_op(TK k) {
    return k == TK::Tilde || k == TK::Exclamation ||
           k == TK::TildeAnd || k == TK::TildeOr || k == TK::TildeXor || k == TK::XorTilde ||
           k == TK::DoublePlus || k == TK::DoubleMinus;
}

inline bool can_begin_unary_expression(TK k) {
    // SystemVerilog unary operators include the arithmetic signs, logical and
    // bitwise negation, and reduction operators.  Some of these tokens are also
    // binary operators (`+`, `-`, `&`, `|`, `^`), so `is_unary_op` deliberately
    // does not classify them globally as unary.  This helper is used only in
    // the syntactic slot immediately after a binary operator, where another
    // expression operand is expected and these tokens can start that operand.
    return k == TK::Plus || k == TK::Minus ||
           k == TK::And || k == TK::Or || k == TK::Xor ||
           k == TK::Tilde || k == TK::Exclamation ||
           k == TK::TildeAnd || k == TK::TildeOr ||
           k == TK::TildeXor || k == TK::XorTilde ||
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
inline bool is_var_decl_leading_keyword(TK k) {
    return is_type_keyword(k) || k == TK::AutomaticKeyword || k == TK::StaticKeyword ||
           k == TK::ConstKeyword;
}
inline bool is_port_direction(TK k) {
    return k == TK::InputKeyword || k == TK::OutputKeyword ||
           k == TK::InOutKeyword || k == TK::RefKeyword;
}
inline bool is_numeric(const Tok& t) {
    return t.lex.kind == TK::IntegerLiteral || t.lex.kind == TK::IntegerBase ||
           t.lex.kind == TK::UnbasedUnsizedLiteral || t.lex.kind == TK::RealLiteral ||
           t.lex.kind == TK::TimeLiteral;
}

inline bool is_based_literal_body_piece(const Tok& t) {
    // Slang lexes based literals by kind, not as one raw token.  For example:
    //
    //   12'h7c4  -> IntegerLiteral("12"), IntegerBase("'h"),
    //               IntegerLiteral("7"), Identifier("c4")
    //   4'b10xz  -> IntegerLiteral("4"),  IntegerBase("'b"),
    //               IntegerLiteral("10"), Identifier("xz")
    //
    // The formatter must keep every body piece adjacent to the preceding
    // IntegerBase; otherwise it can render illegal/changed literals such as
    // `12'h7 c4`.
    return t.lex.kind == TK::IntegerLiteral ||
           t.lex.kind == TK::UnbasedUnsizedLiteral ||
           t.lex.kind == TK::Identifier;
}

inline bool is_based_literal_continuation(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size() || !is_based_literal_body_piece(tokens[idx]))
        return false;
    for (size_t n = idx; n > 0; --n) {
        size_t p = n - 1;
        if (kind_is(tokens[p], TK::IntegerBase))
            return true;
        if (!is_based_literal_body_piece(tokens[p]))
            return false;
    }
    return false;
}

inline bool is_identifier_like(const Tok& t) {
    return t.lex.kind == TK::Identifier || t.lex.kind == TK::SystemIdentifier ||
           t.lex.kind == TK::MacroUsage;
}

inline bool is_code_token(const Tok& t) {
    return t.lex.comment_kind == CommentLexemeKind::None && !t.lex.is_directive && !is_passthrough(t);
}

inline bool is_covergroup_event_at(const TokenStream& tokens, size_t at) {
    if (at >= tokens.size() || !kind_is(tokens[at], TK::At))
        return false;
    for (size_t n = at; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::Semicolon))
            break;
        if (kind_is(tokens[i], TK::CoverGroupKeyword))
            return true;
    }
    return false;
}

inline size_t prev_code(const TokenStream& tokens, size_t before) {
    before = std::min(before, tokens.size());
    for (size_t n = before; n > 0; --n) {
        size_t i = n - 1;
        if (is_code_token(tokens[i]))
            return i;
    }
    return npos;
}

inline bool is_fork_block_open(const TokenStream& tokens, size_t fork_idx) {
    if (fork_idx >= tokens.size() || !kind_is(tokens[fork_idx], TK::ForkKeyword))
        return false;

    // `fork` is a block opener in a fork-join statement:
    //
    //   fork
    //     a();
    //     b();
    //   join_any
    //
    // But the same keyword also appears in statement forms that do *not* open
    // a scope:
    //
    //   wait fork;
    //   disable fork;
    //
    // Keep the token-kind predicate (`is_open_block`) broad enough to describe
    // SystemVerilog block keywords, then apply this contextual guard at actual
    // token sites that mutate wrap/indent metadata.  That preserves correct
    // scope tracking for real fork blocks without regressing the non-opening
    // control statements above.
    size_t prev = prev_code(tokens, fork_idx);
    return prev == npos ||
           (!kind_is(tokens[prev], TK::WaitKeyword) &&
            !kind_is(tokens[prev], TK::DisableKeyword));
}

inline bool opens_indent_scope_at(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size())
        return false;
    if (kind_is(tokens[idx], TK::ForkKeyword))
        return is_fork_block_open(tokens, idx);
    return is_open_block(tokens[idx].lex.kind);
}

inline size_t next_code(const TokenStream& tokens, size_t first, size_t end) {
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i)
        if (is_code_token(tokens[i]))
            return i;
    return npos;
}

inline size_t procedural_body_start(const TokenStream& tokens, size_t proc) {
    if (proc >= tokens.size() || !is_procedural_block_keyword(tokens[proc].lex.kind))
        return npos;

    size_t cur = next_code(tokens, proc + 1, tokens.size());
    if (cur == npos)
        return npos;

    // always / always_ff can be followed by an event control.  Skip the common
    // forms `@(...)`, `@*`, `@ name`, and delay controls.  The body is the first
    // statement token after the timing control.
    if (kind_is(tokens[cur], TK::At) || kind_is(tokens[cur], TK::Hash)) {
        size_t after_control = next_code(tokens, cur + 1, tokens.size());
        if (after_control == npos)
            return npos;
        if ((kind_is(tokens[after_control], TK::OpenParenthesis) ||
             kind_is(tokens[after_control], TK::OpenBrace)) &&
            tokens[after_control].immutable.syntax.matching_token != npos) {
            cur = next_code(tokens, tokens[after_control].immutable.syntax.matching_token + 1,
                            tokens.size());
        } else {
            cur = next_code(tokens, after_control + 1, tokens.size());
        }
    }
    return cur;
}

inline size_t simple_statement_end_from(const TokenStream& tokens, size_t body);

inline size_t single_statement_control_body_start(const TokenStream& tokens, size_t control) {
    if (control == npos || control >= tokens.size() ||
        !is_single_stmt_control(tokens[control].lex.kind))
        return npos;

    // `forever` is the one procedural control in this family that has no
    // parenthesized control expression:
    //
    //   forever statement_or_null
    //
    // The controlled statement begins immediately after the keyword.  Timing
    // controls (`forever #5 clk = ~clk;`) remain part of the statement body and
    // are intentionally returned as the body start so wrapping can place them
    // on their own indented line.
    if (kind_is(tokens[control], TK::ForeverKeyword))
        return next_code(tokens, control + 1, tokens.size());

    // `if`, `for`, `foreach`, `while`, and `repeat` all have a parenthesized
    // header before their `statement_or_null` body.  This token-level formatter
    // should not infer the body by looking for a semicolon in the header; the
    // header may contain declarations, assignments, calls, and nested
    // parentheses.  Use SyntaxPass' matching-parenthesis metadata instead.
    size_t open = next_code(tokens, control + 1, tokens.size());
    if (open == npos || !kind_is(tokens[open], TK::OpenParenthesis) ||
        tokens[open].immutable.syntax.matching_token == npos)
        return npos;
    return next_code(tokens, tokens[open].immutable.syntax.matching_token + 1,
                     tokens.size());
}

inline size_t begin_end_statement_end_from(const TokenStream& tokens, size_t begin_idx) {
    int depth = 0;
    for (size_t i = begin_idx; i < tokens.size(); ++i) {
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::BeginKeyword))
            ++depth;
        else if (kind_is(tokens[i], TK::EndKeyword)) {
            if (depth > 0)
                --depth;
            if (depth == 0)
                return i;
        }
    }
    return npos;
}

inline size_t skip_leading_timing_control(const TokenStream& tokens, size_t first) {
    size_t cur = first;
    while (cur != npos && cur < tokens.size() &&
           (kind_is(tokens[cur], TK::Hash) || kind_is(tokens[cur], TK::At))) {
        size_t arg = next_code(tokens, cur + 1, tokens.size());
        if (arg == npos)
            return npos;
        if ((kind_is(tokens[arg], TK::OpenParenthesis) ||
             kind_is(tokens[arg], TK::OpenBrace)) &&
            tokens[arg].immutable.syntax.matching_token != npos) {
            cur = next_code(tokens, tokens[arg].immutable.syntax.matching_token + 1,
                            tokens.size());
        } else {
            cur = next_code(tokens, arg + 1, tokens.size());
        }
    }
    return cur;
}

inline size_t simple_statement_end_from(const TokenStream& tokens, size_t body) {
    if (body == npos || body >= tokens.size())
        return npos;

    if (kind_is(tokens[body], TK::BeginKeyword))
        return begin_end_statement_end_from(tokens, body);

    if (kind_is(tokens[body], TK::IfKeyword)) {
        size_t cond_open = next_code(tokens, body + 1, tokens.size());
        if (cond_open == npos || !kind_is(tokens[cond_open], TK::OpenParenthesis) ||
            tokens[cond_open].immutable.syntax.matching_token == npos)
            return npos;
        size_t then_body = next_code(tokens, tokens[cond_open].immutable.syntax.matching_token + 1,
                                     tokens.size());
        size_t then_end = simple_statement_end_from(tokens, then_body);
        if (then_end == npos)
            return npos;
        size_t maybe_else = next_code(tokens, then_end + 1, tokens.size());
        if (maybe_else != npos && kind_is(tokens[maybe_else], TK::ElseKeyword)) {
            size_t else_body = next_code(tokens, maybe_else + 1, tokens.size());
            size_t else_end = simple_statement_end_from(tokens, else_body);
            return else_end == npos ? then_end : else_end;
        }
        return then_end;
    }

    if (is_single_stmt_control(tokens[body].lex.kind)) {
        // SystemVerilog uses the same `statement_or_null` body shape for
        // if/for/foreach/while/repeat/forever.  Find that body syntactically
        // and recurse so nested single-statement controls are treated as one
        // statement by callers such as the `else` and procedural-block indent
        // logic.
        //
        // Examples:
        //   else forever #5 clk = ~clk;        ends at the assignment ';'
        //   else for (...) begin a = b; end    ends at the matching `end`
        //   initial if (a) b = c; else d = e;  ends after the else body
        size_t nested = single_statement_control_body_start(tokens, body);
        if (kind_is(tokens[body], TK::ForeverKeyword))
            nested = skip_leading_timing_control(tokens, nested);
        return nested == npos ? npos : simple_statement_end_from(tokens, nested);
    }

    int pd = 0, bd = 0, brd = 0;
    for (size_t i = body; i < tokens.size(); ++i) {
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::OpenParenthesis)) ++pd;
        else if (kind_is(tokens[i], TK::CloseParenthesis) && pd > 0) --pd;
        else if (kind_is(tokens[i], TK::OpenBracket)) ++bd;
        else if (kind_is(tokens[i], TK::CloseBracket) && bd > 0) --bd;
        else if (kind_is(tokens[i], TK::OpenBrace) || kind_is(tokens[i], TK::ApostropheOpenBrace)) ++brd;
        else if (kind_is(tokens[i], TK::CloseBrace) && brd > 0) --brd;
        if (pd == 0 && bd == 0 && brd == 0 && kind_is(tokens[i], TK::Semicolon))
            return i;
    }
    return npos;
}

inline bool is_declaration_keyword(TK k) {
    return is_port_direction(k) || is_type_keyword(k) || k == TK::ParameterKeyword ||
           k == TK::LocalParamKeyword || k == TK::VarKeyword || k == TK::ConstKeyword;
}

inline bool starts_module_like_header(TK k) {
    return k == TK::ModuleKeyword || k == TK::InterfaceKeyword ||
           k == TK::MacromoduleKeyword || k == TK::ProgramKeyword;
}

inline int snap_to_grid(int value, int indent_size) {
    if (indent_size <= 0) return value;
    return ((value + indent_size - 1) / indent_size) * indent_size;
}

inline int option_width(int value, const FormatOptions& opts) {
    return opts.tab_align ? snap_to_grid(value, opts.indent_size) : value;
}

inline int compact_width(const TokenStream& tokens, size_t first, size_t end) {
    int w = 0;
    bool need_space = false;
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i) {
        const Tok& t = tokens[i];
        if (!is_code_token(t)) continue;
        if (need_space && !no_space_before(t.lex.kind))
            ++w;
        w += token_width(t);
        need_space = !no_space_after(t.lex.kind);
    }
    return w;
}

inline int token_text_width(const TokenStream& tokens, size_t first, size_t end) {
    int w = 0;
    for (size_t i = first; i < end && i < tokens.size(); ++i) {
        if (is_code_token(tokens[i]))
            w += token_width(tokens[i]);
    }
    return w;
}

inline int canonical_space_between(const Tok& left, const Tok& right) {
    if (no_space_before(right.lex.kind) || no_space_after(left.lex.kind))
        return 0;
    if (kind_is(left, TK::Dot) || kind_is(right, TK::Dot) ||
        kind_is(left, TK::DoubleColon) || kind_is(right, TK::DoubleColon))
        return 0;
    if (kind_is(right, TK::OpenBracket) &&
        (is_identifier_like(left) || kind_is(left, TK::CloseBracket) ||
         kind_is(left, TK::CloseParenthesis)))
        return 0;
    if (kind_is(right, TK::OpenParenthesis) &&
        (is_identifier_like(left) || is_control_keyword(left.lex.kind) ||
         kind_is(left, TK::CloseParenthesis) || kind_is(left, TK::Hash)))
        return 0;
    return 1;
}

inline int canonical_width(const TokenStream& tokens, size_t first, size_t end) {
    int w = 0;
    size_t prev = npos;
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i) {
        const Tok& t = tokens[i];
        if (!is_code_token(t)) continue;
        if (prev != npos)
            w += canonical_space_between(tokens[prev], t);
        w += token_width(t);
        prev = i;
    }
    return w;
}

inline int line_prefix_width(const TokenStream& tokens, size_t token_idx) {
    size_t line_start = 0;
    for (size_t i = token_idx; i > 0; --i) {
        if (tokens[i].mutable_.wrap.must_break_before || tokens[i - 1].mutable_.wrap.must_break_after) {
            line_start = i;
            break;
        }
    }
    return compact_width(tokens, line_start, token_idx);
}

struct ListItem {
    size_t first{npos};
    size_t last{npos};   // inclusive
    size_t comma{npos};
};

inline std::vector<ListItem> top_level_list_items(const TokenStream& tokens, size_t first, size_t close) {
    std::vector<ListItem> out;
    size_t start = next_code(tokens, first, close);
    size_t last = npos;
    int pd = 0, bd = 0, brd = 0;
    for (size_t i = first; i < close && i < tokens.size(); ++i) {
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::OpenParenthesis)) ++pd;
        else if (kind_is(tokens[i], TK::CloseParenthesis) && pd > 0) --pd;
        else if (kind_is(tokens[i], TK::OpenBracket)) ++bd;
        else if (kind_is(tokens[i], TK::CloseBracket) && bd > 0) --bd;
        else if (kind_is(tokens[i], TK::OpenBrace) || kind_is(tokens[i], TK::ApostropheOpenBrace)) ++brd;
        else if (kind_is(tokens[i], TK::CloseBrace) && brd > 0) --brd;

        if (kind_is(tokens[i], TK::Comma) && pd == 0 && bd == 0 && brd == 0) {
            if (start != npos && last != npos)
                out.push_back({start, last, i});
            start = next_code(tokens, i + 1, close);
            last = npos;
        } else {
            last = i;
        }
    }
    if (start != npos && last != npos)
        out.push_back({start, last, npos});
    return out;
}

inline bool is_module_header_import_semicolon(const TokenStream& tokens, size_t semi) {
    if (semi >= tokens.size() || !kind_is(tokens[semi], TK::Semicolon))
        return false;

    // SystemVerilog permits package imports in a module/interface/program
    // header before the parameter/port lists:
    //
    //   module m
    //       import p::*;
    //   #(parameter int W = 1) (...);
    //
    // The semicolon after `import p::*` is not the module header terminator.
    // Treat it as a header-internal separator when scanning backward for the
    // owning module keyword.  This deliberately uses TokenKind structure rather
    // than the raw text of the import.
    for (size_t n = semi; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::ImportKeyword))
            return true;
        if (kind_is(tokens[i], TK::Semicolon) ||
            is_outer_close(tokens[i].lex.kind) ||
            is_close_block(tokens[i].lex.kind) ||
            starts_module_like_header(tokens[i].lex.kind))
            break;
    }
    return false;
}

inline size_t find_header_keyword_before(const TokenStream& tokens, size_t open) {
    int pd = 0;
    size_t direct_prev = prev_code(tokens, open);
    bool open_after_parameter_list = false;
    if (direct_prev != npos && kind_is(tokens[direct_prev], TK::CloseParenthesis)) {
        size_t param_open = tokens[direct_prev].immutable.syntax.matching_token;
        open_after_parameter_list = param_open != npos &&
                                    tokens[param_open].immutable.topology.starts_parameter_list;
    }
    bool open_follows_header_import =
        (direct_prev != npos && kind_is(tokens[direct_prev], TK::Semicolon)) ||
        open_after_parameter_list;
    for (size_t n = open; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::CloseParenthesis)) ++pd;
        else if (kind_is(tokens[i], TK::OpenParenthesis) && pd > 0) --pd;
        if (pd == 0 && starts_module_like_header(tokens[i].lex.kind))
            return i;
        if (pd == 0 && kind_is(tokens[i], TK::Semicolon) &&
            !open_follows_header_import &&
            !is_module_header_import_semicolon(tokens, i))
            break;
        if (pd == 0 && (is_outer_close(tokens[i].lex.kind) || is_close_block(tokens[i].lex.kind)))
            break;
    }
    return npos;
}

inline bool is_function_task_declaration_open(const TokenStream& tokens, size_t open) {
    if (open >= tokens.size() || !kind_is(tokens[open], TK::OpenParenthesis))
        return false;

    // This option is about the declaration header:
    //
    //   function int add(
    //                   ^ this open paren
    //
    // not about nested calls/default expressions inside the port list.  Walking
    // backward until a declaration boundary keeps the test token-kind based and
    // idempotent.  Seeing another '(' first means the current '(' belongs to an
    // expression nested inside an already-open declaration header.
    for (size_t n = open; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::FunctionKeyword) || kind_is(tokens[i], TK::TaskKeyword))
            return true;
        if (kind_is(tokens[i], TK::OpenParenthesis) ||
            kind_is(tokens[i], TK::Semicolon) ||
            is_close_block(tokens[i].lex.kind) ||
            is_outer_close(tokens[i].lex.kind))
            return false;
    }
    return false;
}

inline bool is_var_declaration_trailing_dimension_open(const TokenStream& tokens, size_t open) {
    if (open >= tokens.size() || !kind_is(tokens[open], TK::OpenBracket))
        return false;
    if (!is_identifier_like(tokens[open == 0 ? open : open - 1]))
        return false;
    if (tokens[open].immutable.syntax.in_covergroup)
        return false;
    size_t close = tokens[open].immutable.syntax.matching_token;
    if (close == npos)
        return false;
    size_t after = next_code(tokens, close + 1, tokens.size());
    if (after == npos)
        return false;
    if (!(kind_is(tokens[after], TK::Semicolon) || kind_is(tokens[after], TK::Comma) ||
          is_assignment_op(tokens[after].lex.kind)))
        return false;

    size_t stmt_begin = tokens[open].immutable.syntax.stmt_begin;
    if (stmt_begin == npos || stmt_begin >= open)
        return false;

    if (is_var_decl_leading_keyword(tokens[stmt_begin].lex.kind))
        return true;

    // Declarations place trailing unpacked dimensions before any initializer,
    // so if the statement already contains an assignment operator before the
    // candidate bracket, this is much more likely to be an expression such as
    // `assign y = arr[3:0];` than a variable declaration.
    for (size_t i = stmt_begin; i < open; ++i) {
        if (is_assignment_op(tokens[i].lex.kind))
            return false;
    }

    // User-defined types can lead a declaration with an identifier-like token.
    // Accept the pattern only when the statement contains at least two
    // identifier-like tokens before the dimension and no member-access dot,
    // which keeps array indexing expressions such as `foo.bar[3:0]` from being
    // misclassified as declarations.
    int identifier_count = 0;
    for (size_t i = stmt_begin; i < open; ++i) {
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::Dot))
            return false;
        if (is_identifier_like(tokens[i]))
            ++identifier_count;
    }
    return identifier_count >= 2;
}

inline size_t module_header_import_owner(const TokenStream& tokens, size_t import_idx) {
    if (import_idx >= tokens.size() || !kind_is(tokens[import_idx], TK::ImportKeyword))
        return npos;

    bool saw_import_semicolon = false;
    for (size_t i = import_idx + 1; i < tokens.size(); ++i) {
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::Semicolon)) {
            saw_import_semicolon = true;
            break;
        }
        if (is_outer_close(tokens[i].lex.kind) || is_close_block(tokens[i].lex.kind))
            return npos;
    }
    if (!saw_import_semicolon)
        return npos;

    for (size_t n = import_idx; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i]))
            continue;
        if (starts_module_like_header(tokens[i].lex.kind))
            return i;
        if (kind_is(tokens[i], TK::Semicolon) &&
            !is_module_header_import_semicolon(tokens, i))
            break;
        if (is_outer_close(tokens[i].lex.kind) || is_close_block(tokens[i].lex.kind))
            break;
    }
    return npos;
}

inline bool parameter_list_contains_directive(const TokenStream& tokens, size_t open, size_t close) {
    for (size_t i = open + 1; i < close && i < tokens.size(); ++i) {
        if (tokens[i].lex.is_directive)
            return true;
    }
    return false;
}

inline bool is_conditional_preprocessor_directive(const Tok& t) {
    if (!t.lex.is_directive)
        return false;

    // Conditional preprocessor directives are compilation-structure markers,
    // not ordinary statements nested in the surrounding SystemVerilog syntax.
    // Keep them visually anchored at column 0 even when they appear inside a
    // begin/end block, a module header, or an instance port list:
    //
    //   always_comb begin
    //   `ifdef USE_FAST_PATH
    //     y = fast;
    //   `else
    //     y = slow;
    //   `endif
    //   end
    //
    // TokenKind is intentionally coarse here: every preprocessor directive has
    // TokenKind::Directive.  Slang still provides the precise directive syntax
    // through Token::directiveKind(), which the formatter lexer stores as an
    // immutable fact so later passes do not need spelling/string comparisons.
    switch (t.lex.directive_kind) {
    case slang::syntax::SyntaxKind::IfDefDirective:
    case slang::syntax::SyntaxKind::IfNDefDirective:
    case slang::syntax::SyntaxKind::ElsIfDirective:
    case slang::syntax::SyntaxKind::ElseDirective:
    case slang::syntax::SyntaxKind::EndIfDirective:
        return true;
    default:
        return false;
    }
}

inline size_t module_header_parameter_hash_owner(const TokenStream& tokens, size_t hash) {
    if (hash >= tokens.size() || !kind_is(tokens[hash], TK::Hash))
        return npos;
    size_t open = next_code(tokens, hash + 1, tokens.size());
    if (open == npos || !kind_is(tokens[open], TK::OpenParenthesis) ||
        !tokens[open].immutable.topology.starts_parameter_list)
        return npos;
    return find_header_keyword_before(tokens, open);
}

inline bool is_instance_port_open(const TokenStream& tokens, size_t open) {
    auto is_basic_sv_item_boundary = [&](size_t prev) {
        if (prev == npos)
            return true;

        return kind_is(tokens[prev], TK::Semicolon) ||
               kind_is(tokens[prev], TK::BeginKeyword) ||
               kind_is(tokens[prev], TK::EndKeyword) ||
               kind_is(tokens[prev], TK::GenerateKeyword) ||
               kind_is(tokens[prev], TK::EndGenerateKeyword) ||
               // Module items may appear immediately after a subroutine body.
               // For example, demo/memory_top.sv declares a module-local task
               // and then instantiates `memory u_mem2 (...)`.  `endtask` and
               // `endfunction` are therefore valid item boundaries just like a
               // declaration semicolon.  Without these explicit boundaries the
               // instance's top-level port list falls through to function-call
               // wrapping, which indents named ports relative to the instance
               // name column instead of using the configured instance-port
               // indentation.
               kind_is(tokens[prev], TK::EndTaskKeyword) ||
               kind_is(tokens[prev], TK::EndFunctionKeyword);
    };

    auto follows_named_generate_boundary = [&](size_t prev) {
        if (prev == npos)
            return false;

        if (is_identifier_like(tokens[prev])) {
            size_t colon = prev_code(tokens, prev);
            size_t opener = colon == npos ? npos : prev_code(tokens, colon);
            if (colon != npos && kind_is(tokens[colon], TK::Colon) &&
                opener != npos &&
                (kind_is(tokens[opener], TK::BeginKeyword) ||
                 kind_is(tokens[opener], TK::ForkKeyword)))
                return true;
        }

        if (kind_is(tokens[prev], TK::Colon)) {
            size_t label = prev_code(tokens, prev);
            size_t before_label = label == npos ? npos : prev_code(tokens, label);
            if (label != npos && is_identifier_like(tokens[label]) &&
                is_basic_sv_item_boundary(before_label))
                return true;
        }

        return false;
    };

    auto is_macro_item_boundary = [&](size_t prev) {
        if (prev == npos)
            return false;

        // Some project macros expand to complete module items but are invoked
        // without a trailing semicolon.  OpenTitan's DV alert helper is a
        // representative example:
        //
        //   `DV_ALERT_IF_CONNECT()
        //
        //   dma #(...) dut (...);
        //
        // The token before `dma` is the macro call's closing parenthesis, not a
        // semicolon.  If we reject that close parenthesis as an item boundary,
        // the DUT port list is misclassified as a function-call argument list
        // and gets hanging-call indentation.  Accept only a *completed macro
        // invocation* whose macro token itself starts where a module item could
        // start; this keeps ordinary expression calls such as `foo(`MACRO())`
        // from becoming instantiation boundaries.
        size_t macro = npos;
        if (kind_is(tokens[prev], TK::MacroUsage)) {
            macro = prev;
        } else if (kind_is(tokens[prev], TK::CloseParenthesis)) {
            size_t open = tokens[prev].immutable.syntax.matching_token;
            macro = open == npos ? npos : prev_code(tokens, open);
            if (macro == npos || !kind_is(tokens[macro], TK::MacroUsage))
                return false;
        } else {
            return false;
        }

        size_t before_macro = prev_code(tokens, macro);
        return is_basic_sv_item_boundary(before_macro) ||
               follows_named_generate_boundary(before_macro);
    };

    auto follows_sv_item_boundary = [&](size_t prev) {
        if (is_basic_sv_item_boundary(prev))
            return true;

        // A module/interface instantiation is a module item / generate item.
        // It can therefore appear after the usual item terminators and after a
        // generate block opener:
        //
        //   foo u_foo (...);
        //   begin
        //     foo u_foo (...);
        //   end
        //
        // It can also appear after a named generate block header.  In concrete
        // token form the item boundary before `foo u_foo` is the label name in
        //
        //   begin : gen_label
        //     foo u_foo (...);
        //
        // or the colon in a labeled generate item:
        //
        //   gen_label : foo u_foo (...);
        //
        // Accept those label forms as boundaries, but keep the check structural
        // (TokenKind only) so we do not mistake arbitrary identifier pairs for
        // instantiations.
        if (follows_named_generate_boundary(prev))
            return true;

        if (is_macro_item_boundary(prev))
            return true;

        return false;
    };

    size_t inst = prev_code(tokens, open);
    if (inst != npos && kind_is(tokens[inst], TK::CloseBracket)) {
        size_t br = tokens[inst].immutable.syntax.matching_token;
        if (br != npos)
            inst = prev_code(tokens, br);
    }
    if (inst == npos || !is_identifier_like(tokens[inst])) return false;
    size_t mod = prev_code(tokens, inst);
    if (mod == npos) return false;
    if (kind_is(tokens[mod], TK::CloseBracket)) {
        size_t br = tokens[mod].immutable.syntax.matching_token;
        if (br != npos) mod = prev_code(tokens, br);
    }
    if (mod != npos && kind_is(tokens[mod], TK::CloseParenthesis)) {
        size_t par = tokens[mod].immutable.syntax.matching_token;
        size_t hash = par == npos ? npos : prev_code(tokens, par);
        if (hash != npos && kind_is(tokens[hash], TK::Hash))
            mod = prev_code(tokens, hash);
    }
    if (mod == npos || !is_identifier_like(tokens[mod])) return false;
    size_t prev = prev_code(tokens, mod);
    return follows_sv_item_boundary(prev);
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
        bool in_function_decl = false;
        bool in_task_decl = false;
        bool in_class_decl = false;
        bool in_covergroup = false;
        bool in_modport = false;
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            t.immutable.syntax.paren_depth = pd;
            t.immutable.syntax.bracket_depth = bd;
            t.immutable.syntax.brace_depth = brd;
            t.immutable.syntax.in_function_decl = in_function_decl;
            t.immutable.syntax.in_task_decl = in_task_decl;
            t.immutable.syntax.in_class_decl = in_class_decl;
            t.immutable.syntax.in_covergroup = in_covergroup;
            t.immutable.syntax.in_modport = in_modport;
            t.immutable.topology.opens_indent_scope = opens_indent_scope_at(tokens, i) || is_outer_open(t.lex.kind);
            t.immutable.topology.closes_indent_scope = is_close_block(t.lex.kind) || is_outer_close(t.lex.kind);

            if (kind_is(t, TK::OpenParenthesis)) {
                size_t prev_i = prev_code(tokens, i);
                const Tok* prev = prev_i == npos ? nullptr : &tokens[prev_i];
                t.immutable.topology.starts_parameter_list = prev && kind_is(*prev, TK::Hash);
                t.immutable.topology.starts_argument_list = prev &&
                    (kind_is(*prev, TK::Identifier) || kind_is(*prev, TK::SystemIdentifier) ||
                     kind_is(*prev, TK::MacroUsage) || kind_is(*prev, TK::CloseParenthesis));
                t.immutable.topology.starts_port_list =
                    pd == 0 &&
                    find_header_keyword_before(tokens, i) != npos &&
                    !(prev && kind_is(*prev, TK::Hash));
            }
            // ends_argument_list is set later when matching token is known (see below)

            if (t.lex.comment_kind != CommentLexemeKind::None) {
                bool comma_interstitial_block = false;
                if (t.immutable.input_trivia.starts_original_line &&
                    t.lex.comment_kind == CommentLexemeKind::Block) {
                    size_t p = prev_code(tokens, i);
                    size_t nx = next_code(tokens, i + 1, tokens.size());
                    comma_interstitial_block =
                        p != npos && kind_is(tokens[p], TK::Comma) &&
                        nx != npos &&
                        tokens[nx].immutable.input_trivia.original_newlines_before == 0;
                }
                t.immutable.comment.role =
                    (t.immutable.input_trivia.starts_original_line && !comma_interstitial_block)
                    ? CommentRole::OwnLine : CommentRole::Trailing;
                t.immutable.comment.anchor_token = i == 0 ? npos : i - 1;
                t.immutable.comment.inside_expression = pd > 0 || bd > 0 || brd > 0;
                t.immutable.comment.inside_arg_list = pd > 0;
            }
            if (kind_is(t, TK::FunctionKeyword)) in_function_decl = true;
            if (kind_is(t, TK::TaskKeyword)) in_task_decl = true;
            if (kind_is(t, TK::ClassKeyword)) in_class_decl = true;
            if (kind_is(t, TK::CoverGroupKeyword)) in_covergroup = true;
            if (kind_is(t, TK::ModPortKeyword)) in_modport = true;
            if (kind_is(t, TK::OpenParenthesis)) { parens.push_back(i); ++pd; }
            else if (kind_is(t, TK::CloseParenthesis)) { if (!parens.empty()) { auto j = parens.back(); parens.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; t.immutable.topology.ends_argument_list = tokens[j].immutable.topology.starts_argument_list; } pd = std::max(0, pd - 1); }
            else if (kind_is(t, TK::OpenBracket)) { brackets.push_back(i); ++bd; }
            else if (kind_is(t, TK::CloseBracket)) { if (!brackets.empty()) { auto j = brackets.back(); brackets.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } bd = std::max(0, bd - 1); }
            else if (kind_is(t, TK::OpenBrace)) { braces.push_back(i); ++brd; }
            else if (kind_is(t, TK::CloseBrace)) { if (!braces.empty()) { auto j = braces.back(); braces.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } brd = std::max(0, brd - 1); }
            if (kind_is(t, TK::Semicolon)) {
                in_function_decl = false;
                in_task_decl = false;
                in_modport = false;
            }
            if (kind_is(t, TK::EndClassKeyword))
                in_class_decl = false;
            if (kind_is(t, TK::EndGroupKeyword))
                in_covergroup = false;
        }
        size_t stmt_start = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (kind_is(tokens[i], TK::Semicolon) || kind_is(tokens[i], TK::Comma)) {
                for (size_t j = stmt_start; j <= i && j < tokens.size(); ++j) { tokens[j].immutable.syntax.stmt_begin = stmt_start; tokens[j].immutable.syntax.stmt_end = i; }
                stmt_start = i + 1;
            }
        }
        // Precompute inside_argument_list: O(n).  A token is inside an
        // argument list when
        // depth > 0, with ( exclusive (depth increments after) and ) exclusive
        // (depth decrements before), matching the original backward-scan semantics.
        {
            int depth = 0;
            for (size_t i = 0; i < tokens.size(); ++i) {
                auto& tok = tokens[i];
                if (kind_is(tok, TK::CloseParenthesis) && tok.immutable.topology.ends_argument_list)
                    depth = std::max(0, depth - 1);
                tok.immutable.topology.inside_argument_list = depth > 0;
                if (kind_is(tok, TK::OpenParenthesis) && tok.immutable.topology.starts_argument_list)
                    ++depth;
            }
        }
    }
};

inline bool is_class_extends_parameter_list(const TokenStream& tokens, size_t open) {
    size_t hash = prev_code(tokens, open);
    if (hash == npos || !kind_is(tokens[hash], TK::Hash))
        return false;
    for (size_t n = hash; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::Semicolon))
            break;
        if (kind_is(tokens[i], TK::ExtendsKeyword) || kind_is(tokens[i], TK::ClassKeyword))
            return true;
    }
    return false;
}

inline bool is_expression_brace(const TokenStream& tokens, size_t brace) {
    if (brace >= tokens.size() || !kind_is(tokens[brace], TK::OpenBrace))
        return false;
    size_t p = prev_code(tokens, brace);
    return p != npos && (kind_is(tokens[p], TK::Equals) ||
                         kind_is(tokens[p], TK::Apostrophe) ||
                         kind_is(tokens[p], TK::OpenBracket));
}

inline bool is_multiline_brace_construct(const TokenStream& tokens, size_t brace) {
    if (brace >= tokens.size() || !kind_is(tokens[brace], TK::OpenBrace))
        return false;
    for (size_t n = brace; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::Semicolon) || kind_is(tokens[i], TK::OpenBrace))
            break;
        if (kind_is(tokens[i], TK::CoverPointKeyword) || kind_is(tokens[i], TK::DistKeyword) ||
            kind_is(tokens[i], TK::ConstraintKeyword))
            return true;
    }
    return false;
}

inline bool is_struct_or_union_body_brace(const TokenStream& tokens, size_t brace) {
    if (brace >= tokens.size() || !kind_is(tokens[brace], TK::OpenBrace))
        return false;

    // `struct packed {` and `union tagged packed {` have qualifiers between
    // the keyword and the body brace.  Looking only at prev_code(openBrace)
    // misses those cases and leaves the first field on the same line:
    //
    //     typedef struct packed {logic valid;
    //
    // Scan backward within the declaration header until a statement / prior
    // brace boundary.  This keeps expression braces and aggregate literals out
    // while treating all struct/union body forms consistently.
    for (size_t n = brace; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::Semicolon) || kind_is(tokens[i], TK::OpenBrace) ||
            kind_is(tokens[i], TK::CloseBrace))
            break;
        if (kind_is(tokens[i], TK::StructKeyword) || kind_is(tokens[i], TK::UnionKeyword))
            return true;
    }
    return false;
}

// MacroClassifier + MacroRole — used by MacroPass to categorise macro tokens.
enum class MacroRole { ObjectLikeExpr, FunctionLikeExpr, StatementLike, DeclarationLike, ControlFlowLike, BlockBeginLike, BlockEndLike };

struct MacroClassifier {
    std::unordered_set<std::string> object_like_expr;
    std::unordered_set<std::string> function_like_expr;
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
        add(object_like_expr,  m.object_like_expr);
        add(function_like_expr, m.function_like_expr);
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
        if (function_like_expr.count(name)) return MacroRole::FunctionLikeExpr;
        if (object_like_expr.count(name))   return MacroRole::ObjectLikeExpr;
        if (block_end_like.count(name))    return MacroRole::BlockEndLike;
        if (block_begin_like.count(name))  return MacroRole::BlockBeginLike;
        if (control_flow_like.count(name)) return MacroRole::ControlFlowLike;
        if (declaration_like.count(name))  return MacroRole::DeclarationLike;
        if (statement_like.count(name))    return MacroRole::StatementLike;

        // Unknown macros are safest as expression-like leaves.  That default
        // keeps the formatter conservative: it avoids inventing statement,
        // declaration, or block-boundary structure for a macro whose expansion
        // is not known to the lexer.  Users can opt into stronger roles with
        // [format.macros] when a project macro intentionally behaves like a
        // block opener/closer, declaration, statement, or control-flow token.
        return MacroRole::ObjectLikeExpr;
    }

    bool is_whitespace_sensitive(const std::string& raw_text) const {
        return whitespace_sensitive.count(extract_name(raw_text)) > 0;
    }
};

inline std::string extract_define_name(const std::string& raw_text) {
    constexpr std::string_view define_keyword = "define";
    size_t pos = 0;
    if (pos >= raw_text.size() || raw_text[pos] != '`')
        return {};
    ++pos;
    for (char expected : define_keyword) {
        if (pos >= raw_text.size() || raw_text[pos] != expected)
            return {};
        ++pos;
    }
    while (pos < raw_text.size() && std::isspace(static_cast<unsigned char>(raw_text[pos])))
        ++pos;
    if (pos >= raw_text.size())
        return {};
    if (raw_text[pos] == '`')
        ++pos;
    size_t begin = pos;
    while (pos < raw_text.size() &&
           (std::isalnum(static_cast<unsigned char>(raw_text[pos])) ||
            raw_text[pos] == '_' || raw_text[pos] == '$'))
        ++pos;
    return raw_text.substr(begin, pos - begin);
}

// MacroPass owns MacroMetadata.  It never changes wrapping or spacing directly;
// it only marks regions/macros that downstream passes must treat conservatively.
class MacroPass final : public IFormatPass {
public:
    explicit MacroPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "macro"; }
    void run(TokenStream& tokens) override {
        MacroClassifier mc(opts_.macros);
        std::unordered_set<std::string> local_whitespace_sensitive;
        for (const auto& t : tokens) {
            if (!t.lex.is_directive)
                continue;
            // Multiline `define bodies are frozen by the lexer as one
            // whitespace-sensitive directive token.  A macro whose replacement
            // text spans physical lines often relies on argument text being
            // substituted exactly as written; remember its name so nested
            // function calls inside later invocations are not independently
            // exploded by the function-call wrapping pass.
            if (t.lex.is_whitespace_sensitive) {
                std::string name = extract_define_name(t.lex.text);
                if (!name.empty())
                    local_whitespace_sensitive.insert(std::move(name));
            }
        }
        for (auto& t : tokens) {
            if (t.lex.is_whitespace_sensitive) {
                t.mutable_.macro.passthrough = true;
                t.mutable_.macro.suppress_alignment = true;
                t.mutable_.macro.suppress_wrapping = true;
            } else if (t.lex.is_directive) {
                t.mutable_.macro.suppress_alignment = true;
            } else if (t.lex.kind == TK::MacroUsage) {
                const std::string macro_name = MacroClassifier::extract_name(t.lex.text);
                const bool whitespace_sensitive =
                    mc.is_whitespace_sensitive(t.lex.text) ||
                    local_whitespace_sensitive.count(macro_name) > 0;
                if (whitespace_sensitive) {
                    t.mutable_.macro.suppress_alignment = true;
                    size_t open = next_code(tokens, &t - tokens.data() + 1, tokens.size());
                    if (open != npos && kind_is(tokens[open], TK::OpenParenthesis)) {
                        size_t close = tokens[open].immutable.syntax.matching_token;
                        if (close != npos) {
                            for (size_t k = open + 1; k < close && k < tokens.size(); ++k) {
                                tokens[k].mutable_.macro.suppress_alignment = true;
                                tokens[k].mutable_.macro.suppress_wrapping = true;
                            }
                        }
                    }
                } else {
                    t.mutable_.macro.suppress_alignment = true;
                    MacroRole role = mc.classify(t.lex.text);
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
            if (i == 0)
                t.mutable_.wrap.must_break_before = true;
            if (t.lex.comment_kind != CommentLexemeKind::None && t.immutable.comment.role == CommentRole::OwnLine) {
                t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = true;
            } else if (t.immutable.comment.role == CommentRole::Trailing &&
                       t.lex.comment_kind == CommentLexemeKind::Line) {
                // A trailing line comment consumes the rest of the physical
                // line.  Treat it as a hard line boundary in the wrap layer so
                // later packing/alignment decisions cannot place the following
                // token into the comment text on the next formatting pass.
                t.mutable_.wrap.must_break_after = true;
            }
            // PP directives (ifdef/endif/else/define/…) are always on their own line.
            if (t.lex.is_directive) {
                t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = true;
            }
            if (module_header_import_owner(tokens, i) != npos) {
                // Header imports are visually separate clauses in the module
                // declaration, not ordinary same-line statements.  Break
                // before the `import` keyword so the semicolon cannot be read
                // as terminating the whole module header.
                t.mutable_.wrap.must_break_before = true;
            }
            if (t.mutable_.macro.suppress_wrapping) continue;
            t.mutable_.wrap.wrap_group = group;
            // Force line break after statement-/declaration-/block-like macro invocations.
            // If the macro is followed by '(args)', break after the matching ')'; otherwise
            // break after the macro token itself.
            if (kind_is(t, TK::MacroUsage) && t.mutable_.macro.force_line_break) {
                size_t break_at = i;
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (tokens[j].lex.comment_kind != CommentLexemeKind::None) continue;
                    if (kind_is(tokens[j], TK::OpenParenthesis) &&
                        tokens[j].immutable.syntax.matching_token != npos)
                        break_at = tokens[j].immutable.syntax.matching_token;
                    break;
                }
                tokens[break_at].mutable_.wrap.must_break_after = true;
            }
            if (kind_is(t, TK::MacroUsage) && !t.mutable_.macro.force_line_break) {
                size_t prev = prev_code(tokens, i);
                bool statement_position =
                    prev == npos || kind_is(tokens[prev], TK::Semicolon) ||
                    kind_is(tokens[prev], TK::BeginKeyword) || is_close_block(tokens[prev].lex.kind) ||
                    is_outer_close(tokens[prev].lex.kind);
                size_t open = next_code(tokens, i + 1, tokens.size());
                if (statement_position && open != npos && kind_is(tokens[open], TK::OpenParenthesis)) {
                    size_t close = tokens[open].immutable.syntax.matching_token;
                    if (close != npos && close < tokens.size())
                        tokens[close].mutable_.wrap.must_break_after = true;
                }
            }
            if (i > 0 &&
                tokens[i - 1].immutable.comment.role == CommentRole::Trailing &&
                tokens[i - 1].lex.comment_kind == CommentLexemeKind::Line)
                t.mutable_.wrap.must_break_before = true;
            if (kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth == 0) {
                size_t next = next_code(tokens, i + 1, tokens.size());
                t.mutable_.wrap.must_break_after =
                    is_module_header_import_semicolon(tokens, i) ||
                    !(next != npos && kind_is(tokens[next], TK::Hash));
                ++group;
                // If a trailing comment immediately follows on the same line,
                // defer the line break to after the comment so it renders inline.
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (is_passthrough(tokens[j])) continue;
                    if (tokens[j].lex.comment_kind != CommentLexemeKind::None &&
                        tokens[j].immutable.comment.role == CommentRole::Trailing) {
                        t.mutable_.wrap.must_break_after = false;
                        if (j > i + 1 && tokens[j - 1].lex.comment_kind != CommentLexemeKind::None)
                            tokens[j - 1].mutable_.wrap.must_break_after = false;
                        tokens[j].mutable_.wrap.must_break_after = true;
                        continue;
                    }
                    break;
                }
            }
            if (kind_is(t, TK::Semicolon)) {
                size_t p = prev_code(tokens, i);
                size_t open = (p != npos && kind_is(tokens[p], TK::CloseParenthesis))
                    ? tokens[p].immutable.syntax.matching_token : npos;
                size_t control = open == npos ? npos : prev_code(tokens, open);
                size_t before_control = control == npos ? npos : prev_code(tokens, control);
                if (control != npos && before_control != npos &&
                    kind_is(tokens[control], TK::WhileKeyword) &&
                    kind_is(tokens[before_control], TK::EndKeyword))
                    t.mutable_.wrap.must_break_before = false;
            }
            if (kind_is(t, TK::CloseParenthesis) && t.immutable.syntax.matching_token != npos) {
                size_t before_open = prev_code(tokens, t.immutable.syntax.matching_token);
                if (before_open != npos &&
                    (kind_is(tokens[before_open], TK::CaseKeyword) ||
                     kind_is(tokens[before_open], TK::CaseXKeyword) ||
                     kind_is(tokens[before_open], TK::CaseZKeyword)))
                    t.mutable_.wrap.must_break_after = true;
            }
            if (kind_is(t, TK::Comma)) t.mutable_.wrap.can_break_after = true;
            // Close-block keywords always start a new line; CloseBrace only when
            // not inside a parenthesised expression (e.g. `inside {A, B}`).
            if (is_outer_close(t.lex.kind) ||
                (is_close_block(t.lex.kind) &&
                 !(kind_is(t, TK::CloseBrace) &&
                   (t.immutable.syntax.paren_depth > 0 ||
                    (t.immutable.syntax.matching_token != npos &&
                     is_expression_brace(tokens, t.immutable.syntax.matching_token))))))
                t.mutable_.wrap.must_break_before = true;
            size_t next_i = next_code(tokens, i + 1, tokens.size());
            bool followed_by_label_colon = next_i != npos && kind_is(tokens[next_i], TK::Colon);
            bool close_brace_before_decl_name =
                kind_is(t, TK::CloseBrace) && next_i != npos &&
                (kind_is(tokens[next_i], TK::Identifier) || kind_is(tokens[next_i], TK::SystemIdentifier));
            bool close_brace_before_semicolon =
                kind_is(t, TK::CloseBrace) && next_i != npos && kind_is(tokens[next_i], TK::Semicolon);
            bool close_before_inline_else =
                kind_is(t, TK::CloseBrace) && next_i != npos && kind_is(tokens[next_i], TK::ElseKeyword) &&
                !opts_.statement.wrap_end_else_clauses;
            bool end_before_do_while =
                kind_is(t, TK::EndKeyword) && next_i != npos && kind_is(tokens[next_i], TK::WhileKeyword);
            if (kind_is(t, TK::BeginKeyword) ||
                is_fork_block_open(tokens, i) ||
                (is_outer_close(t.lex.kind) && !followed_by_label_colon) ||
                (is_close_block(t.lex.kind) && !followed_by_label_colon &&
                 !end_before_do_while &&
                 !close_brace_before_decl_name &&
                 !close_brace_before_semicolon &&
                 !close_before_inline_else &&
                 !(kind_is(t, TK::CloseBrace) &&
                   (t.immutable.syntax.paren_depth > 0 ||
                    (t.immutable.syntax.matching_token != npos &&
                     is_expression_brace(tokens, t.immutable.syntax.matching_token)))))) {
                t.mutable_.wrap.must_break_after = true;
            }
            if (is_identifier_like(t)) {
                size_t p = prev_code(tokens, i);
                size_t pp = p == npos ? npos : prev_code(tokens, p);
                if (p != npos && pp != npos && kind_is(tokens[p], TK::Colon) &&
                    (is_close_block(tokens[pp].lex.kind) || is_outer_close(tokens[pp].lex.kind)))
                    t.mutable_.wrap.must_break_after = true;
            }
            if (opts_.statement.begin_newline &&
                (kind_is(t, TK::BeginKeyword) ||
                 is_fork_block_open(tokens, i) ||
                 (kind_is(t, TK::OpenBrace) && !is_expression_brace(tokens, i))))
                t.mutable_.wrap.must_break_before = true;
            if (kind_is(t, TK::OpenBrace) && is_multiline_brace_construct(tokens, i)) {
                if (opts_.statement.begin_newline)
                    t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = true;
                if (t.immutable.syntax.matching_token != npos) {
                    size_t close = t.immutable.syntax.matching_token;
                    tokens[close].mutable_.wrap.must_break_before = true;
                    size_t after_close = next_code(tokens, close + 1, tokens.size());
                    tokens[close].mutable_.wrap.must_break_after =
                        !(after_close != npos && kind_is(tokens[after_close], TK::Semicolon));
                }
            }
            if (kind_is(t, TK::OpenBrace)) {
                if (is_struct_or_union_body_brace(tokens, i))
                    t.mutable_.wrap.must_break_after = true;
            }
            if (opts_.statement.wrap_end_else_clauses && kind_is(t, TK::ElseKeyword) && i > 0 && (kind_is(tokens[i - 1], TK::EndKeyword) || kind_is(tokens[i - 1], TK::CloseBrace))) t.mutable_.wrap.must_break_before = true;
            // else always breaks unless wrap_end_else_clauses handled it above
            if (kind_is(t, TK::ElseKeyword)) {
                bool prev_is_end_or_brace = (i > 0 && (kind_is(tokens[i-1], TK::EndKeyword) || kind_is(tokens[i-1], TK::CloseBrace)));
                if (!prev_is_end_or_brace)
                    t.mutable_.wrap.must_break_before = true;
            }

            // Any statement/block terminator that is followed by a same-line
            // trailing comment should keep that comment attached and move the
            // physical line break after the comment.  The semicolon path above
            // handles ordinary statements, but EOF comments after final
            // `endmodule` / `endclass` labels need the same treatment.
            if (t.mutable_.wrap.must_break_after && t.lex.comment_kind == CommentLexemeKind::None) {
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (is_passthrough(tokens[j])) continue;
                    if (tokens[j].lex.comment_kind != CommentLexemeKind::None &&
                        tokens[j].immutable.comment.role == CommentRole::Trailing) {
                        t.mutable_.wrap.must_break_after = false;
                        tokens[j].mutable_.wrap.must_break_after = true;
                    }
                    break;
                }
            }
        }

        auto apply_list = [&](size_t open, WrapListKind kind, bool break_after_open,
                              bool break_before_close, bool break_first_item) {
            if (open >= tokens.size()) return;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos || close >= tokens.size()) return;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty()) return;
            tokens[open].mutable_.wrap.list_kind = kind;
            tokens[open].mutable_.wrap.list_open = open;
            if (break_after_open)
                tokens[open].mutable_.wrap.must_break_after = true;
            else
                tokens[open].mutable_.wrap.must_break_after = false;
            for (size_t n = 0; n < items.size(); ++n) {
                const auto& item = items[n];
                size_t leading_block_comment = npos;
                if (n > 0 && kind != WrapListKind::InstancePorts) {
                    for (size_t c = item.first; c > open + 1; --c) {
                        size_t p = c - 1;
                        if (tokens[p].lex.comment_kind == CommentLexemeKind::Block) {
                            if (!(p + 1 < tokens.size() && tokens[p + 1].lex.comment_kind != CommentLexemeKind::None &&
                                  tokens[p + 1].immutable.comment.role == CommentRole::OwnLine))
                                leading_block_comment = p;
                            break;
                        }
                        if (is_code_token(tokens[p]) && !kind_is(tokens[p], TK::Comma))
                            break;
                        if (kind_is(tokens[p], TK::Comma))
                            break;
                    }
                }
                tokens[item.first].mutable_.wrap.list_kind = kind;
                tokens[item.first].mutable_.wrap.list_open = open;
                if (leading_block_comment != npos) {
                    tokens[leading_block_comment].mutable_.wrap.list_kind = kind;
                    tokens[leading_block_comment].mutable_.wrap.list_open = open;
                    tokens[leading_block_comment].mutable_.wrap.must_break_before = true;
                    tokens[item.first].mutable_.wrap.must_break_before = false;
                } else if (break_first_item || n > 0)
                    tokens[item.first].mutable_.wrap.must_break_before = true;
                else {
                    bool preceded_by_own_line_comment = false;
                    for (size_t p = item.first; p > open + 1; --p) {
                        size_t c = p - 1;
                        if (tokens[c].lex.comment_kind != CommentLexemeKind::None &&
                            tokens[c].immutable.comment.role == CommentRole::OwnLine) {
                            preceded_by_own_line_comment = true;
                            break;
                        }
                        if (is_code_token(tokens[c]))
                            break;
                    }
                    if (!preceded_by_own_line_comment)
                        tokens[item.first].mutable_.wrap.must_break_before = false;
                }
                if (item.comma != npos) {
                    size_t break_token = item.comma;
                    for (size_t c = item.comma + 1; c < tokens.size(); ++c) {
                        if (tokens[c].lex.comment_kind != CommentLexemeKind::None &&
                            tokens[c].immutable.comment.role == CommentRole::Trailing &&
                            (kind == WrapListKind::InstancePorts ||
                             tokens[c].lex.comment_kind == CommentLexemeKind::Line ||
                             (c + 1 < tokens.size() && tokens[c + 1].lex.comment_kind != CommentLexemeKind::None &&
                              tokens[c + 1].immutable.comment.role == CommentRole::OwnLine))) {
                            break_token = c;
                            break;
                        }
                        if (is_code_token(tokens[c]))
                            break;
                    }
                    tokens[break_token].mutable_.wrap.must_break_after = true;
                    tokens[item.comma].mutable_.wrap.list_kind = kind;
                    tokens[item.comma].mutable_.wrap.list_open = open;
                }
            }
            tokens[close].mutable_.wrap.list_kind = kind;
            tokens[close].mutable_.wrap.list_open = open;
            if (break_before_close)
                tokens[close].mutable_.wrap.must_break_before = true;
            else
                tokens[close].mutable_.wrap.must_break_before = false;

            size_t after_open = open + 1;
            if (after_open < close && tokens[after_open].lex.comment_kind != CommentLexemeKind::None &&
                tokens[after_open].immutable.comment.role == CommentRole::Trailing) {
                tokens[open].mutable_.wrap.must_break_after = false;
                tokens[after_open].mutable_.wrap.must_break_after = true;
            }

            if (kind == WrapListKind::ModulePorts && !items.empty() &&
                !is_declaration_keyword(tokens[items.front().first].lex.kind)) {
                int per_line = 1;
                if (opts_.module.non_ansi_port_per_line_enabled)
                    per_line = std::max(1, opts_.module.non_ansi_port_per_line);
                else if (opts_.module.non_ansi_port_max_line_length_enabled)
                    per_line = static_cast<int>(items.size());
                int line_count = 0;
                int line_width = opts_.indent_size;
                std::vector<bool> keep_with_prev(items.size(), false);
                for (size_t n = 0; n < items.size(); ++n) {
                    int item_width = compact_width(tokens, items[n].first, items[n].last + 1);
                    bool keep = n > 0 && line_count < per_line;
                    if (keep && opts_.module.non_ansi_port_max_line_length_enabled &&
                        !opts_.module.non_ansi_port_per_line_enabled) {
                        int projected = line_width + 2 + item_width +
                                        (items[n].comma != npos ? 1 : 0);
                        keep = projected <= opts_.module.non_ansi_port_max_line_length;
                    }
                    if (keep) {
                        keep_with_prev[n] = true;
                        tokens[items[n].first].mutable_.wrap.must_break_before = false;
                        line_width += 2 + item_width + (items[n].comma != npos ? 1 : 0);
                        ++line_count;
                    } else {
                        line_width = opts_.indent_size + item_width + (items[n].comma != npos ? 1 : 0);
                        line_count = 1;
                    }
                }
                for (size_t n = 0; n + 1 < items.size(); ++n) {
                    if (items[n].comma != npos && keep_with_prev[n + 1]) {
                            tokens[items[n].comma].mutable_.wrap.must_break_after = false;
                            for (size_t c = items[n].comma + 1; c < tokens.size(); ++c) {
                                if (tokens[c].lex.comment_kind != CommentLexemeKind::None &&
                                    tokens[c].immutable.comment.role == CommentRole::Trailing)
                                    tokens[c].mutable_.wrap.must_break_after = false;
                                if (is_code_token(tokens[c]))
                                    break;
                            }
                    }
                }
            }
        };

        auto contains_kind = [&](size_t first, size_t end, TK kind) {
            for (size_t j = first; j < end && j < tokens.size(); ++j)
                if (kind_is(tokens[j], kind))
                    return true;
            return false;
        };

        // Precompute whether an opening parenthesis is nested inside another
        // argument-list parenthesis.  Keeping this as a vector avoids the old
        // per-open backward scan, which was quadratic on generated files with
        // thousands of calls.
        std::vector<bool> nested_argument_open(tokens.size(), false);
        std::vector<size_t> argument_stack;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_code_token(tokens[i]))
                continue;
            if (kind_is(tokens[i], TK::OpenParenthesis)) {
                nested_argument_open[i] = !argument_stack.empty();
                if (tokens[i].immutable.topology.starts_argument_list)
                    argument_stack.push_back(i);
            } else if (kind_is(tokens[i], TK::CloseParenthesis) &&
                       tokens[i].immutable.syntax.matching_token != npos) {
                size_t open = tokens[i].immutable.syntax.matching_token;
                if (!argument_stack.empty() && argument_stack.back() == open)
                    argument_stack.pop_back();
            }
        }

        for (size_t open = 0; open < tokens.size(); ++open) {
            if (!kind_is(tokens[open], TK::OpenParenthesis) && !kind_is(tokens[open], TK::OpenBrace))
                continue;
            if (tokens[open].mutable_.macro.suppress_wrapping)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos || close >= tokens.size())
                continue;

            if (kind_is(tokens[open], TK::OpenBrace)) {
                bool enum_body = false;
                for (size_t j = open; j > 0; --j) {
                    size_t k = j - 1;
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::EnumKeyword)) { enum_body = true; break; }
                    if (kind_is(tokens[k], TK::Semicolon)) break;
                }
                if (enum_body) {
                    apply_list(open, WrapListKind::EnumBody, true, true, true);
                } else if (is_multiline_brace_construct(tokens, open)) {
                    apply_list(open, WrapListKind::BraceBlock, true, true, true);
                }
                continue;
            }

            if (tokens[open].immutable.topology.starts_parameter_list) {
                if (find_header_keyword_before(tokens, open) == npos &&
                    !is_class_extends_parameter_list(tokens, open))
                    continue;
                auto items = top_level_list_items(tokens, open + 1, close);
                if (items.empty())
                    continue;
                bool block = opts_.module.parameter_layout != "hanging";
                bool expand = block || items.size() > 1 ||
                              parameter_list_contains_directive(tokens, open, close) ||
                              (line_prefix_width(tokens, open) + 1 +
                               compact_width(tokens, open + 1, close) + 1 >
                               opts_.function_declaration.line_length);
                if (expand) {
                    apply_list(open, block ? WrapListKind::ModuleParametersBlock
                                           : WrapListKind::ModuleParametersHanging,
                               block, block, block);
                }
                continue;
            }

            if (tokens[open].immutable.topology.starts_port_list) {
                apply_list(open, WrapListKind::ModulePorts, true, true, true);
                continue;
            }

            size_t prev = prev_code(tokens, open);
            if (prev != npos && kind_is(tokens[prev], TK::Identifier)) {
                size_t before_name = prev_code(tokens, prev);
                if ((before_name != npos && kind_is(tokens[before_name], TK::ModPortKeyword)) ||
                    tokens[open].immutable.syntax.in_modport) {
                    apply_list(open, WrapListKind::ModportBody, true, true, true);
                    size_t close = tokens[open].immutable.syntax.matching_token;
                    size_t comma = close == npos ? npos : next_code(tokens, close + 1, tokens.size());
                    if (comma != npos && kind_is(tokens[comma], TK::Comma))
                        tokens[comma].mutable_.wrap.must_break_after = true;
                    continue;
                }
            }

            if (is_instance_port_open(tokens, open)) {
                apply_list(open, WrapListKind::InstancePorts, true, true, true);
                continue;
            }

            bool is_decl = false;
            for (size_t j = open; j > 0; --j) {
                size_t k = j - 1;
                if (!is_code_token(tokens[k])) continue;
                if (kind_is(tokens[k], TK::FunctionKeyword) || kind_is(tokens[k], TK::TaskKeyword)) {
                    is_decl = true;
                    break;
                }
                if (kind_is(tokens[k], TK::Semicolon)) break;
            }
            if (is_decl) {
                int approx = line_prefix_width(tokens, open) + 1 + compact_width(tokens, open + 1, close) + 1;
                if (approx > opts_.function_declaration.line_length) {
                    bool hanging = opts_.function_declaration.layout == "hanging";
                    apply_list(open, hanging ? WrapListKind::FunctionDeclHanging
                                             : WrapListKind::FunctionDeclBlock,
                               !hanging, !hanging, !hanging);
                }
                continue;
            }

            if (tokens[open].immutable.topology.starts_argument_list) {
                if (open < nested_argument_open.size() && nested_argument_open[open])
                    continue;
                auto items = top_level_list_items(tokens, open + 1, close);
                if (items.empty())
                    continue;
                bool do_break = false;
                if (opts_.function_call.break_policy == "always")
                    do_break = items.size() > 1;
                else if (opts_.function_call.break_policy == "auto") {
                    do_break = (opts_.function_call.arg_count >= 0 &&
                                static_cast<int>(items.size()) >= opts_.function_call.arg_count);
                    int approx = line_prefix_width(tokens, open) + 1 + compact_width(tokens, open + 1, close) + 1;
                    if (approx > opts_.function_call.line_length)
                        do_break = true;
                }
                if (do_break && opts_.function_call.break_policy != "never") {
                    bool hanging = opts_.function_call.layout == "hanging";
                    apply_list(open, hanging ? WrapListKind::FunctionHanging
                                             : WrapListKind::FunctionBlock,
                               !hanging, !hanging, !hanging);
                }
            }
        }

        apply_procedural_block_wrap(tokens);
        apply_single_statement_control_wrap(tokens);

        // Final comment line-boundary normalization belongs in WrapPass, not
        // CommentPass: it writes only WrapMetadata and runs after all list
        // packing helpers that may have cleared comma/comment breaks to keep
        // short port lists on one physical line.  A `//` comment is a lexical
        // line terminator in SystemVerilog; allowing any later token to render
        // after it changes that token into comment text on the next pass.
        for (auto& t : tokens) {
            if (t.lex.comment_kind == CommentLexemeKind::None)
                continue;
            if (t.immutable.comment.role == CommentRole::OwnLine) {
                t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = true;
            } else if (t.immutable.comment.role == CommentRole::Trailing &&
                       t.lex.comment_kind == CommentLexemeKind::Line) {
                t.mutable_.wrap.must_break_after = true;
            }
        }
    }
private:
    static void apply_procedural_block_wrap(TokenStream& tokens) {
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_procedural_block_keyword(tokens[i].lex.kind))
                continue;
            size_t body = procedural_body_start(tokens, i);
            if (body == npos || body >= tokens.size())
                continue;
            // Blocks that already use begin/fork can keep the existing project
            // style (`always_comb begin`).  Single-statement procedural blocks
            // and procedural blocks whose body is an if/forever/etc. get a
            // mandatory line break so the controlled statement is visually
            // nested under the always/initial/final header.
            if (!kind_is(tokens[body], TK::BeginKeyword) &&
                !kind_is(tokens[body], TK::ForkKeyword)) {
                tokens[body].mutable_.wrap.must_break_before = true;
            }
        }
    }

    static void apply_single_statement_control_wrap(TokenStream& tokens) {
        bool ctrl_expr_pending = false;
        int ctrl_paren_open = 0;
        bool single_stmt_pending = false;
        int paren_depth = 0;
        bool ctrl_just_closed = false;

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (is_passthrough(t))
                continue;

            if (kind_is(t, TK::OpenParenthesis))
                ++paren_depth;
            else if (kind_is(t, TK::CloseParenthesis) && paren_depth > 0)
                --paren_depth;

            if (kind_is(t, TK::ForeverKeyword)) {
                size_t body = next_code(tokens, i + 1, tokens.size());
                if (body != npos &&
                    !kind_is(tokens[body], TK::BeginKeyword) &&
                    !kind_is(tokens[body], TK::ForkKeyword) &&
                    !kind_is(tokens[body], TK::OpenBrace))
                    tokens[body].mutable_.wrap.must_break_before = true;
                continue;
            }

            if (kind_is(t, TK::ElseKeyword)) {
                size_t body = next_code(tokens, i + 1, tokens.size());
                if (body != npos &&
                    !kind_is(tokens[body], TK::BeginKeyword) &&
                    !kind_is(tokens[body], TK::ForkKeyword) &&
                    !kind_is(tokens[body], TK::OpenBrace))
                    tokens[body].mutable_.wrap.must_break_before = true;
                continue;
            }

            if (is_single_stmt_control(t.lex.kind))
                ctrl_expr_pending = true;
            if (ctrl_expr_pending && kind_is(t, TK::OpenParenthesis)) {
                ctrl_expr_pending = false;
                ctrl_paren_open = paren_depth;
            }

            if (kind_is(t, TK::CloseParenthesis) &&
                ctrl_paren_open > 0 &&
                paren_depth == ctrl_paren_open - 1) {
                ctrl_paren_open = 0;
                bool do_while_tail = false;
                if (t.immutable.syntax.matching_token != npos) {
                    size_t control = prev_code(tokens, t.immutable.syntax.matching_token);
                    size_t before_control = control == npos ? npos : prev_code(tokens, control);
                    do_while_tail = control != npos && before_control != npos &&
                        kind_is(tokens[control], TK::WhileKeyword) &&
                        kind_is(tokens[before_control], TK::EndKeyword);
                }
                single_stmt_pending = !do_while_tail;
                ctrl_just_closed = true;
            }

            if (single_stmt_pending && !ctrl_just_closed && t.lex.comment_kind == CommentLexemeKind::None) {
                single_stmt_pending = false;
                const bool is_block = kind_is(t, TK::BeginKeyword) ||
                                      is_fork_block_open(tokens, i) ||
                                      kind_is(t, TK::OpenBrace);
                const bool closes = is_close_block(t.lex.kind) || is_outer_close(t.lex.kind) ||
                                    t.mutable_.macro.closes_indent_scope;
                if (!is_block && !closes)
                    t.mutable_.wrap.must_break_before = true;
            }
            ctrl_just_closed = false;
        }
    }

    const FormatOptions& opts_;
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
        std::unordered_map<size_t, size_t> procedural_body_end;
        std::unordered_map<size_t, size_t> else_body_end;
        std::unordered_map<size_t, size_t> forever_body_end;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_procedural_block_keyword(tokens[i].lex.kind))
                continue;
            size_t body = procedural_body_start(tokens, i);
            if (body == npos || body >= tokens.size())
                continue;
            if (kind_is(tokens[body], TK::BeginKeyword) ||
                kind_is(tokens[body], TK::ForkKeyword) ||
                kind_is(tokens[body], TK::OpenBrace))
                continue;
            size_t body_end = simple_statement_end_from(tokens, body);
            procedural_body_end[body] = body_end == npos ? tokens[i].immutable.syntax.stmt_end : body_end;
        }
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::ElseKeyword))
                continue;
            size_t body = next_code(tokens, i + 1, tokens.size());
            if (body == npos || body >= tokens.size())
                continue;
            if (kind_is(tokens[body], TK::BeginKeyword) ||
                kind_is(tokens[body], TK::ForkKeyword) ||
                kind_is(tokens[body], TK::OpenBrace))
                continue;
            size_t body_end = simple_statement_end_from(tokens, body);
            if (body_end != npos)
                else_body_end[body] = body_end;
        }
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::ForeverKeyword))
                continue;
            size_t body = next_code(tokens, i + 1, tokens.size());
            if (body == npos || body >= tokens.size())
                continue;
            if (kind_is(tokens[body], TK::BeginKeyword) || kind_is(tokens[body], TK::ForkKeyword))
                continue;
            size_t body_end = simple_statement_end_from(tokens, i);
            if (body_end != npos)
                forever_body_end[body] = body_end;
        }
        bool procedural_body_active = false;
        size_t procedural_body_stmt_end = npos;
        bool else_body_active = false;
        size_t else_body_stmt_end = npos;
        bool forever_body_active = false;
        size_t forever_body_stmt_end = npos;

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (is_passthrough(t)) continue;

            // Track parens for single-stmt detection
            if (kind_is(t, TK::OpenParenthesis)) ++paren_depth;
            else if (kind_is(t, TK::CloseParenthesis) && paren_depth > 0) --paren_depth;

            // Detect control expression start
            if (is_single_stmt_control(t.lex.kind) && !kind_is(t, TK::ForeverKeyword)) {
                ctrl_expr_pending = true;
            }
            if (ctrl_expr_pending && kind_is(t, TK::OpenParenthesis)) {
                ctrl_expr_pending = false;
                ctrl_paren_open = paren_depth; // depth after increment above
            }
            // Detect control expression close — set pending but don't resolve yet (defer to next token)
            if (kind_is(t, TK::CloseParenthesis) && ctrl_paren_open > 0 && paren_depth == ctrl_paren_open - 1) {
                ctrl_paren_open = 0;
                bool do_while_tail = false;
                if (t.immutable.syntax.matching_token != npos) {
                    size_t control = prev_code(tokens, t.immutable.syntax.matching_token);
                    size_t before_control = control == npos ? npos : prev_code(tokens, control);
                    do_while_tail = control != npos && before_control != npos &&
                        kind_is(tokens[control], TK::WhileKeyword) &&
                        kind_is(tokens[before_control], TK::EndKeyword);
                }
                single_stmt_pending = !do_while_tail;
                ctrl_just_closed = true;
            }

            // Track declaration-qualifier context
            if (kind_is(t, TK::ImportKeyword))  in_import  = true;
            if (kind_is(t, TK::ExternKeyword))  in_extern  = true;
            if (kind_is(t, TK::TypedefKeyword)) in_typedef = true;
            if (kind_is(t, TK::Semicolon))      { in_import = false; in_extern = false; in_typedef = false; }

            // Compute indent — close tokens first so they dedent before assignment
            bool closes = is_close_block(t.lex.kind) || is_outer_close(t.lex.kind) ||
                          t.mutable_.macro.closes_indent_scope;
            if (closes) level = std::max(0, level - 1);

            // Resolve single-stmt pending on first non-passthrough, non-comment token AFTER control expr close
            if (single_stmt_pending && !ctrl_just_closed && t.lex.comment_kind == CommentLexemeKind::None) {
                single_stmt_pending = false;
                bool is_block = kind_is(t, TK::BeginKeyword) ||
                                is_fork_block_open(tokens, i) ||
                                kind_is(t, TK::OpenBrace);
                if (!is_block && !closes) {
                    level++;
                    single_stmt_active = true;
                    single_stmt_paren_depth = paren_depth;
                }
            }
            ctrl_just_closed = false;

            if (auto it = procedural_body_end.find(i); it != procedural_body_end.end()) {
                ++level;
                procedural_body_active = true;
                procedural_body_stmt_end = it->second;
            }
            if (auto it = else_body_end.find(i); it != else_body_end.end()) {
                ++level;
                else_body_active = true;
                else_body_stmt_end = it->second;
            }
            if (auto it = forever_body_end.find(i); it != forever_body_end.end()) {
                ++level;
                forever_body_active = true;
                forever_body_stmt_end = it->second;
            }

            t.mutable_.indent.base_indent = level * opts_.indent_size;
            if (size_t hdr = module_header_import_owner(tokens, i); hdr != npos)
                t.mutable_.indent.base_indent =
                    tokens[hdr].mutable_.indent.base_indent + opts_.indent_size;
            if (size_t hdr = module_header_parameter_hash_owner(tokens, i); hdr != npos)
                t.mutable_.indent.base_indent = tokens[hdr].mutable_.indent.base_indent;
            if (is_outer_close(t.lex.kind))
                t.mutable_.indent.base_indent = 0;
            else if (is_outer_open(t.lex.kind) && opts_.default_indent_level_inside_outmost_block == 0)
                t.mutable_.indent.base_indent = 0; // OuterOpen itself is at outer level
            if (is_conditional_preprocessor_directive(t))
                t.mutable_.indent.base_indent = 0;
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
                    (opens_indent_scope_at(tokens, i) ||
                     (is_outer_open(t.lex.kind) && opts_.default_indent_level_inside_outmost_block > 0) ||
                     t.mutable_.macro.opens_indent_scope))
                    ++level;
            }

            // Close single-stmt at semicolon at the right depth
            if (single_stmt_active && kind_is(t, TK::Semicolon) && paren_depth == single_stmt_paren_depth) {
                level = std::max(0, level - 1);
                single_stmt_active = false;
            }
            if (procedural_body_active && i == procedural_body_stmt_end) {
                level = std::max(0, level - 1);
                procedural_body_active = false;
                procedural_body_stmt_end = npos;
            }
            if (else_body_active && i == else_body_stmt_end) {
                level = std::max(0, level - 1);
                else_body_active = false;
                else_body_stmt_end = npos;
            }
            if (forever_body_active && i == forever_body_stmt_end) {
                level = std::max(0, level - 1);
                forever_body_active = false;
                forever_body_stmt_end = npos;
            }
        }

        auto line_start_of = [&](size_t idx) {
            size_t s = 0;
            for (size_t n = idx; n > 0; --n) {
                size_t i = n - 1;
                if (tokens[n].mutable_.wrap.must_break_before || tokens[i].mutable_.wrap.must_break_after) {
                    s = n;
                    break;
                }
            }
            return s;
        };
        auto column_before = [&](size_t idx) {
            size_t s = line_start_of(idx);
            int base = tokens[s].mutable_.indent.base_indent;
            int col = base + compact_width(tokens, s, idx);
            size_t prev = prev_code(tokens, idx);
            bool call_or_hash_paren =
                kind_is(tokens[idx], TK::OpenParenthesis) && prev != npos &&
                (is_identifier_like(tokens[prev]) || kind_is(tokens[prev], TK::Hash));
            if (idx != s && prev != npos && prev >= s &&
                !call_or_hash_paren &&
                !no_space_before(tokens[idx].lex.kind) && !no_space_after(tokens[prev].lex.kind))
                ++col;
            return col;
        };
        auto set_item_indent = [&](size_t first, size_t last, int indent) {
            if (first >= tokens.size()) return;
            tokens[first].mutable_.indent.base_indent = std::max(0, indent);
            for (size_t k = first + 1; k <= last && k < tokens.size(); ++k) {
                if (tokens[k].mutable_.wrap.must_break_before ||
                    (tokens[k].lex.comment_kind != CommentLexemeKind::None && tokens[k].immutable.comment.role == CommentRole::OwnLine))
                    tokens[k].mutable_.indent.base_indent = std::max(0, indent);
            }
        };

        for (size_t open = 0; open < tokens.size(); ++open) {
            WrapListKind kind = tokens[open].mutable_.wrap.list_kind;
            if (kind == WrapListKind::None || tokens[open].mutable_.wrap.list_open != open)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos || close >= tokens.size())
                continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty())
                continue;

            int base = tokens[line_start_of(open)].mutable_.indent.base_indent;
            int item_indent = base + opts_.indent_size;
            int close_indent = base;

            size_t name = prev_code(tokens, open);
            int name_col = (name == npos) ? base : column_before(name);
            int after_open_col = column_before(open) + token_width(tokens[open]);

            switch (kind) {
            case WrapListKind::FunctionBlock:
                item_indent = name_col + opts_.indent_size;
                close_indent = name_col;
                break;
            case WrapListKind::FunctionHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                break;
            case WrapListKind::ModuleParametersBlock:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::ModuleParametersHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                break;
            case WrapListKind::FunctionDeclBlock:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::FunctionDeclHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                break;
            case WrapListKind::InstancePorts:
                item_indent = base + std::max(0, opts_.instance.port_indent_level) * opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::ModulePorts:
                if (size_t hdr = find_header_keyword_before(tokens, open); hdr != npos)
                    base = tokens[hdr].mutable_.indent.base_indent;
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::EnumBody:
            case WrapListKind::BraceBlock:
            case WrapListKind::ModportBody:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::None:
                break;
            }

            for (const auto& item : items)
                set_item_indent(item.first, item.last, item_indent);
            for (size_t k = open + 1; k < close; ++k) {
                if (tokens[k].lex.comment_kind != CommentLexemeKind::None &&
                    (tokens[k].immutable.comment.role == CommentRole::OwnLine ||
                     tokens[k].mutable_.wrap.must_break_before))
                    tokens[k].mutable_.indent.base_indent = std::max(0, item_indent);
                if ((kind == WrapListKind::ModuleParametersBlock ||
                     kind == WrapListKind::ModuleParametersHanging) &&
                    tokens[k].lex.is_directive &&
                    !is_conditional_preprocessor_directive(tokens[k]) &&
                    tokens[k].mutable_.wrap.must_break_before)
                    tokens[k].mutable_.indent.base_indent = std::max(0, item_indent);
            }
            if (tokens[close].mutable_.wrap.must_break_before)
                tokens[close].mutable_.indent.base_indent = std::max(0, close_indent);
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
        struct Line {
            size_t first{npos};
            size_t end{npos};
            size_t assign_idx{npos};
            size_t lhs_first{npos};
            int    lhs_width{0};
            int    lhs_prefix_width{0};
            int    indent{0};
            bool   disabled{false};
        };

        std::vector<Line> lines;
        size_t cur_first = npos;
        size_t cur_start = 0;

        // Assignment alignment is intentionally disabled inside single-stmt
        // control blocks such as `for (...) begin ... end` because generated
        // loop bodies often contain repeated assignments where local vertical
        // alignment is noisier than useful.  Older code rediscovered this fact
        // by scanning backward from every rendered line to find a controlling
        // `begin`, which made large generated register files quadratic.
        //
        // Compute the same lexical containment once with a lightweight
        // begin/end stack.  This is approximate in the same direction as the
        // previous heuristic: it only tracks procedural `begin`/`end`, and it
        // only treats a begin as control-owned when it follows a parenthesized
        // for/foreach/while/repeat header.
        std::vector<bool> inside_control_begin(tokens.size(), false);
        std::vector<bool> begin_stack;
        int control_begin_depth = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (is_passthrough(tokens[i]))
                continue;
            inside_control_begin[i] = control_begin_depth > 0;
            if (kind_is(tokens[i], TK::BeginKeyword)) {
                bool control_owned = false;
                size_t close_paren = prev_code(tokens, i);
                size_t open_paren = close_paren == npos ? npos : tokens[close_paren].immutable.syntax.matching_token;
                size_t control = open_paren == npos ? npos : prev_code(tokens, open_paren);
                if (control != npos &&
                    (kind_is(tokens[control], TK::ForKeyword) ||
                     kind_is(tokens[control], TK::ForeachKeyword) ||
                     kind_is(tokens[control], TK::WhileKeyword) ||
                     kind_is(tokens[control], TK::RepeatKeyword))) {
                    control_owned = true;
                }
                begin_stack.push_back(control_owned);
                if (control_owned)
                    ++control_begin_depth;
            } else if (kind_is(tokens[i], TK::EndKeyword) && !begin_stack.empty()) {
                if (begin_stack.back())
                    control_begin_depth = std::max(0, control_begin_depth - 1);
                begin_stack.pop_back();
            }
        }
        auto push_line = [&](size_t end_idx) {
            Line ln;
            ln.first = cur_first;
            ln.end = end_idx;
            if (cur_first != npos)
                ln.indent = tokens[cur_first].mutable_.indent.base_indent;
            if (cur_first != npos &&
                (is_type_keyword(tokens[cur_first].lex.kind) ||
                 is_port_direction(tokens[cur_first].lex.kind) ||
                 ((kind_is(tokens[cur_first], TK::ParameterKeyword) ||
                   kind_is(tokens[cur_first], TK::LocalParamKeyword)) &&
                  tokens[cur_first].immutable.syntax.paren_depth > 0)))
                ln.disabled = true;
            if (cur_first != npos && cur_first < inside_control_begin.size() &&
                inside_control_begin[cur_first])
                ln.disabled = true;
            // Find assignment op at depth 0
            int pd = 0, bd = 0, brd = 0;
            size_t scan_start = (cur_first != npos ? cur_first : cur_start);
            if (scan_start < end_idx && kind_is(tokens[scan_start], TK::AssignKeyword)) {
                size_t after_assign = next_code(tokens, scan_start + 1, end_idx);
                if (after_assign != npos) {
                    // Continuous assignments have a fixed `assign ` prefix.
                    // Align the net LHS after that prefix to the same
                    // lhs_min_width rule used by procedural assignments:
                    //
                    //   lhs_min_width = 10, spacing = both
                    //   assign d          = a;
                    //          ^^^^^^^^^^^ 10-column LHS field + one
                    //                       pre-operator space
                    ln.lhs_prefix_width = canonical_width(tokens, scan_start, after_assign) + 1;
                    scan_start = after_assign;
                }
            }
            ln.lhs_first = scan_start;
            for (size_t k = scan_start; k < end_idx; ++k) {
                auto& tok = tokens[k];
                if (is_passthrough(tok)) { ln.disabled = true; continue; }
                if (tok.lex.comment_kind != CommentLexemeKind::None) continue;
                if (kind_is(tok, TK::OpenParenthesis))  ++pd;
                else if (kind_is(tok, TK::CloseParenthesis) && pd > 0)  --pd;
                else if (kind_is(tok, TK::OpenBracket))   ++bd;
                else if (kind_is(tok, TK::CloseBracket)  && bd > 0)   --bd;
                else if (kind_is(tok, TK::OpenBrace))    ++brd;
                else if (kind_is(tok, TK::CloseBrace)   && brd > 0)   --brd;
                if (pd == 0 && bd == 0 && brd == 0 &&
                    !tok.mutable_.macro.suppress_alignment &&
                    is_assignment_op(tok.lex.kind)) {
                    ln.assign_idx = k;
                    break;
                }
            }
            // Compute LHS width
            if (ln.assign_idx != npos) {
                ln.lhs_width = canonical_width(tokens, scan_start, ln.assign_idx);
                int identifiers_before_assign = 0;
                for (size_t k = scan_start; k < ln.assign_idx; ++k) {
                    if (is_code_token(tokens[k]) && is_identifier_like(tokens[k]))
                        ++identifiers_before_assign;
                }
                if (identifiers_before_assign >= 2)
                    ln.disabled = true;
            }
            lines.push_back(ln);
            cur_first = npos;
            cur_start = end_idx;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& tok = tokens[i];
            if (is_passthrough(tok)) continue;
            if (i > 0 && (tok.mutable_.wrap.must_break_before ||
                          tokens[i - 1].mutable_.wrap.must_break_after)) {
                push_line(i);
            }
            if (cur_first == npos) cur_first = i;
        }
        if (cur_first != npos) push_line(tokens.size());

        if (opts_.statement.align) {
            // Group consecutive alignable lines and align
            const bool space_before = wants_before(opts_.spacing.assignment_operator_spacing);
            const int op_gap = space_before ? 1 : 0;

            size_t li = 0;
            while (li < lines.size()) {
                if (lines[li].disabled || lines[li].assign_idx == npos) { ++li; continue; }
                int base_indent = lines[li].indent;
                // Collect group of consecutive lines with same indent that have assign ops
                size_t j = li;
                while (j < lines.size() && !lines[j].disabled && lines[j].assign_idx != npos && lines[j].indent == base_indent)
                    ++j;
                if (j - li >= 2) {
                    int group_lhs = opts_.statement.lhs_min_width;
                    for (size_t k = li; k < j; ++k)
                        group_lhs = std::max(group_lhs, lines[k].lhs_width);
                    for (size_t k = li; k < j; ++k) {
                        const int lhs_field = opts_.statement.align_adaptive
                            ? std::max(opts_.statement.lhs_min_width, lines[k].lhs_width)
                            : group_lhs;
                        const int target = opts_.tab_align
                            ? snap_to_grid(lines[k].lhs_prefix_width + lhs_field + op_gap, opts_.indent_size)
                            : lines[k].lhs_prefix_width + lhs_field + op_gap;
                        tokens[lines[k].assign_idx].mutable_.align.enabled = true;
                        tokens[lines[k].assign_idx].mutable_.align.target_column =
                            lines[k].indent + target;
                    }
                } else if (j - li == 1 && opts_.statement.lhs_min_width > 0) {
                    const int lhs_field = std::max(opts_.statement.lhs_min_width, lines[li].lhs_width);
                    int target = opts_.tab_align
                        ? snap_to_grid(lines[li].lhs_prefix_width + lhs_field + op_gap, opts_.indent_size)
                        : lines[li].lhs_prefix_width + lhs_field + op_gap;
                    tokens[lines[li].assign_idx].mutable_.align.enabled = true;
                    tokens[lines[li].assign_idx].mutable_.align.target_column =
                        lines[li].indent + target;
                }
                li = j;
            }
        }

        if (opts_.var_declaration.align) {
            int declaration_line_count = 0;
            int declaration_keyword_width = 0;
            for (const auto& ln : lines) {
                if (ln.first != npos &&
                    (is_type_keyword(tokens[ln.first].lex.kind) ||
                     is_port_direction(tokens[ln.first].lex.kind))) {
                    ++declaration_line_count;
                    declaration_keyword_width = std::max(declaration_keyword_width,
                                                         token_width(tokens[ln.first]));
                }
            }
            for (const auto& ln : lines) {
                if (ln.first == npos || ln.end <= ln.first)
                    continue;
                size_t first = ln.first;
                if (!is_type_keyword(tokens[first].lex.kind) &&
                    !is_port_direction(tokens[first].lex.kind))
                    continue;

                size_t semi = npos;
                size_t eq = npos;
                int pd = 0, bd = 0, brd = 0;
                for (size_t k = first; k < ln.end; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenParenthesis)) ++pd;
                    else if (kind_is(tokens[k], TK::CloseParenthesis) && pd > 0) --pd;
                    else if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (kind_is(tokens[k], TK::OpenBrace) || kind_is(tokens[k], TK::ApostropheOpenBrace)) ++brd;
                    else if (kind_is(tokens[k], TK::CloseBrace) && brd > 0) --brd;
                    if (pd == 0 && bd == 0 && brd == 0) {
                        if (eq == npos && kind_is(tokens[k], TK::Equals))
                            eq = k;
                        if (kind_is(tokens[k], TK::Semicolon)) {
                            semi = k;
                            break;
                        }
                    }
                }
                if (semi == npos)
                    continue;

                size_t name = npos;
                size_t name_limit = eq == npos ? semi : eq;
                for (size_t n = name_limit; n > first + 1; --n) {
                    size_t k = n - 1;
                    if (is_code_token(tokens[k]) && is_identifier_like(tokens[k])) {
                        name = k;
                        break;
                    }
                }
                if (name == npos)
                    continue;

                size_t dim = npos;
                for (size_t k = first + 1; k < name; ++k) {
                    if (kind_is(tokens[k], TK::OpenBracket)) {
                        dim = k;
                        break;
                    }
                }

                const int base = tokens[first].mutable_.indent.base_indent;
                const bool port_decl = is_port_direction(tokens[first].lex.kind);
                const int section1 = option_width(port_decl ? opts_.port_declaration.section1_min_width
                                                            : opts_.var_declaration.section1_min_width, opts_);
                const int section2 = option_width(port_decl ? opts_.port_declaration.section2_min_width
                                                            : opts_.var_declaration.section2_min_width, opts_);
                const int section3 = option_width(port_decl ? opts_.port_declaration.section3_min_width
                                                            : opts_.var_declaration.section3_min_width, opts_);
                const int section4 = option_width(port_decl ? opts_.port_declaration.section4_min_width
                                                            : opts_.var_declaration.section4_min_width, opts_);

                int name_target = base + token_width(tokens[first]) + section2 + 1;
                if (port_decl) {
                    size_t type_first = next_code(tokens, first + 1, name);
                    if (type_first != npos && type_first < name) {
                        tokens[type_first].mutable_.align.enabled = true;
                        tokens[type_first].mutable_.align.target_column =
                            base + option_width(std::max(opts_.port_declaration.section1_min_width,
                                                         token_width(tokens[first]) + 1), opts_);
                        int type_width = canonical_width(tokens, type_first, name);
                        name_target = tokens[type_first].mutable_.align.target_column +
                                      snap_to_grid(type_width + 1, opts_.indent_size);
                    }
                } else if (dim != npos && section1 > 0) {
                    tokens[dim].mutable_.align.enabled = true;
                    tokens[dim].mutable_.align.target_column = base + section1;
                    name_target = base + section1 + section2;
                } else if (dim != npos && declaration_line_count >= 2) {
                    tokens[dim].mutable_.align.enabled = true;
                    tokens[dim].mutable_.align.target_column = base + declaration_keyword_width + 1;
                    name_target = tokens[dim].mutable_.align.target_column + section2;
                } else if (dim == npos && section1 > 0) {
                    name_target = base + section1 + section2;
                }

                const bool align_name = declaration_line_count >= 2 || dim != npos ||
                                        opts_.var_declaration.section1_min_width > 0;
                if (align_name) {
                    tokens[name].mutable_.align.enabled = true;
                    tokens[name].mutable_.align.target_column = name_target;
                }

                if (eq != npos) {
                    tokens[eq].mutable_.align.enabled = true;
                    tokens[eq].mutable_.align.target_column = name_target + section3;
                    if (semi != npos) {
                        int rhs_width = canonical_width(tokens, eq + 1, semi);
                        if (!opts_.var_declaration.align_adaptive || rhs_width < section4) {
                            tokens[semi].mutable_.align.enabled = true;
                            tokens[semi].mutable_.align.target_column =
                                tokens[eq].mutable_.align.target_column + section4;
                        }
                    }
                } else if (semi != npos) {
                    tokens[semi].mutable_.align.enabled = true;
                    tokens[semi].mutable_.align.target_column =
                        align_name ? (name_target + section3 + section4)
                                   : (base + canonical_width(tokens, first, semi) + section3 - 1);
                }
            }
        }

        if (opts_.var_declaration.align && opts_.var_declaration.section1_min_width > 0) {
            struct VarLine {
                size_t first{npos};
                size_t end{npos};
                size_t semi{npos};
                size_t eq{npos};
                size_t first_name{npos};
                size_t first_delim{npos};
                size_t packed_dim{npos};
                bool has_dim{false};
                int indent{0};
            };

            auto find_statement_semicolon = [&](const Line& ln, size_t& eq_out) {
                eq_out = npos;
                int pd = 0, bd = 0, brd = 0;
                for (size_t k = ln.first; k < ln.end; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenParenthesis)) ++pd;
                    else if (kind_is(tokens[k], TK::CloseParenthesis) && pd > 0) --pd;
                    else if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (kind_is(tokens[k], TK::OpenBrace) || kind_is(tokens[k], TK::ApostropheOpenBrace)) ++brd;
                    else if (kind_is(tokens[k], TK::CloseBrace) && brd > 0) --brd;
                    if (pd == 0 && bd == 0 && brd == 0) {
                        if (eq_out == npos && kind_is(tokens[k], TK::Equals))
                            eq_out = k;
                        if (kind_is(tokens[k], TK::Semicolon))
                            return k;
                    }
                }
                return npos;
            };

            auto previous_decl_name = [&](size_t begin, size_t end) {
                int local_pd = 0, local_brd = 0;
                for (size_t n = end; n > begin; --n) {
                    size_t k = n - 1;
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::CloseParenthesis)) ++local_pd;
                    else if (kind_is(tokens[k], TK::OpenParenthesis) && local_pd > 0) --local_pd;
                    else if (kind_is(tokens[k], TK::CloseBrace)) ++local_brd;
                    else if ((kind_is(tokens[k], TK::OpenBrace) ||
                              kind_is(tokens[k], TK::ApostropheOpenBrace)) && local_brd > 0) --local_brd;
                    if (local_pd != 0 || local_brd != 0)
                        continue;
                    if (kind_is(tokens[k], TK::CloseBracket)) {
                        size_t open = tokens[k].immutable.syntax.matching_token;
                        if (open != npos && open > begin && open < k)
                            n = open + 1;
                        continue;
                    }
                    if (is_identifier_like(tokens[k]))
                        return k;
                }
                return npos;
            };

            auto first_top_level_delim = [&](size_t begin, size_t end) {
                int bd = 0;
                for (size_t k = begin; k < end; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (bd == 0 && kind_is(tokens[k], TK::Comma))
                        return k;
                }
                return end;
            };

            auto var_trailing_render_width = [&](size_t first, size_t end) {
                int width = token_text_width(tokens, first, end);
                for (size_t k = first; k < end && k < tokens.size(); ++k) {
                    if (!is_code_token(tokens[k]) || !kind_is(tokens[k], TK::Equals))
                        continue;

                    // `token_text_width` intentionally keeps bracketed
                    // dimensions compact (`[1:0]`), which is what section4
                    // needs.  Add only the spaces that the renderer will place
                    // around an assignment operator.  The space before `=` is
                    // part of the trailing field only when the trailing field
                    // started earlier, e.g. at an unpacked dimension:
                    //
                    //   [1:0] = value
                    //
                    // If the trailing field itself starts at `=`, only the
                    // after-space belongs to that field:
                    //
                    //   = value
                    if (k != first && wants_before(opts_.spacing.assignment_operator_spacing))
                        ++width;
                    if (next_code(tokens, k + 1, end) != npos &&
                        wants_after(opts_.spacing.assignment_operator_spacing))
                        ++width;
                }
                return width;
            };

            auto is_var_decl_line = [&](const Line& ln, VarLine& out) {
                if (ln.first == npos || ln.end <= ln.first)
                    return false;
                if (is_port_direction(tokens[ln.first].lex.kind))
                    return false;
                size_t eq = npos;
                size_t semi = find_statement_semicolon(ln, eq);
                if (semi == npos)
                    return false;

                bool plausible_start = is_var_decl_leading_keyword(tokens[ln.first].lex.kind);
                if (!plausible_start && is_identifier_like(tokens[ln.first])) {
                    size_t nx = next_code(tokens, ln.first + 1, semi);
                    plausible_start = nx != npos &&
                        (is_identifier_like(tokens[nx]) || kind_is(tokens[nx], TK::OpenBracket));
                }
                if (!plausible_start)
                    return false;

                size_t first_delim = first_top_level_delim(ln.first + 1, eq == npos ? semi : eq);
                size_t first_name = previous_decl_name(ln.first + 1, first_delim);
                if (first_name == npos || first_name <= ln.first)
                    return false;
                // A user-defined type declaration and a module/interface instance can
                // both begin with two identifier-like tokens:
                //
                //   packet_t value;    // declaration
                //   memory   u_mem();  // zero-port instance
                //
                // Treat an immediate top-level parenthesized tail after the candidate
                // name as an instantiation/call, not as a variable declaration.  This
                // keeps the declaration aligner from rewriting `memory u_mem();` into
                // a fake declaration with a padded semicolon.
                size_t after_name = next_code(tokens, first_name + 1, semi);
                if (after_name != npos && kind_is(tokens[after_name], TK::OpenParenthesis))
                    return false;

                size_t packed_dim = npos;
                for (size_t k = ln.first + 1; k < first_name; ++k) {
                    if (kind_is(tokens[k], TK::OpenBracket)) {
                        packed_dim = k;
                        break;
                    }
                }
                out = {ln.first, ln.end, semi, eq, first_name, first_delim,
                       packed_dim, packed_dim != npos, ln.indent};
                return true;
            };

            std::vector<VarLine> vlines(lines.size());
            std::vector<bool> is_vline(lines.size(), false);
            for (size_t i = 0; i < lines.size(); ++i)
                is_vline[i] = is_var_decl_line(lines[i], vlines[i]);

            for (size_t i = 0; i < lines.size();) {
                if (!is_vline[i]) {
                    ++i;
                    continue;
                }
                size_t j = i;
                bool group_has_dim = false;
                int indent = vlines[i].indent;
                int group_type_width = 0;
                int group_packed_width = 0;
                int group_name_width = 0;
                int group_trailing_width = 0;
                while (j < lines.size() && is_vline[j] && vlines[j].indent == indent) {
                    group_has_dim = group_has_dim || vlines[j].has_dim;
                    group_type_width = std::max(
                        group_type_width,
                        canonical_width(tokens, vlines[j].first,
                                        vlines[j].packed_dim != npos ? vlines[j].packed_dim
                                                                     : vlines[j].first_name));
                    if (vlines[j].packed_dim != npos) {
                        size_t close = tokens[vlines[j].packed_dim].immutable.syntax.matching_token;
                        if (close != npos && close < vlines[j].first_name)
                            group_packed_width = std::max(
                                group_packed_width,
                                token_text_width(tokens, vlines[j].packed_dim, close + 1));
                    }
                    group_name_width = std::max(group_name_width,
                                                token_width(tokens[vlines[j].first_name]));

                    // Section4 is the trailing field after the declaration
                    // name section.  For non-adaptive alignment, semicolons
                    // should line up with the longest trailing field in the
                    // group, including unpacked dimensions and initializers:
                    //
                    //   a                              ;
                    //   b        [1:0] = very_long_rhs;
                    //
                    // Measure from the first token that belongs to that
                    // trailing field through the token before `;`.  Empty
                    // trailing fields keep width 0 and are widened later by
                    // `section4_min_width`.
                    size_t trailing_first = vlines[j].eq != npos ? vlines[j].eq : npos;
                    for (size_t k = vlines[j].first_name + 1; k < vlines[j].first_delim; ++k) {
                        if (kind_is(tokens[k], TK::OpenBracket)) {
                            trailing_first = k;
                            break;
                        }
                    }
                    if (trailing_first != npos)
                        group_trailing_width = std::max(
                            group_trailing_width,
                            var_trailing_render_width(trailing_first, vlines[j].semi));
                    ++j;
                }

                const int section1 = option_width(opts_.var_declaration.section1_min_width, opts_);
                const int section2 = option_width(opts_.var_declaration.section2_min_width, opts_);
                const int section3 = option_width(opts_.var_declaration.section3_min_width, opts_);
                const int section4 = option_width(opts_.var_declaration.section4_min_width, opts_);
                const int effective_section1 =
                    opts_.var_declaration.align_adaptive
                        ? section1
                        : option_width(std::max(opts_.var_declaration.section1_min_width,
                                                group_type_width + 1), opts_);
                const int effective_section2 =
                    opts_.var_declaration.align_adaptive
                        ? section2
                        : option_width(std::max(opts_.var_declaration.section2_min_width,
                                                group_packed_width + 1), opts_);
                const int effective_section3 =
                    opts_.var_declaration.align_adaptive
                        ? section3
                        : option_width(std::max(opts_.var_declaration.section3_min_width,
                                                group_name_width + 1), opts_);
                const int effective_section4 =
                    opts_.var_declaration.align_adaptive
                        ? section4
                        : option_width(std::max(opts_.var_declaration.section4_min_width,
                                                group_trailing_width), opts_);

                for (size_t li = i; li < j; ++li) {
                    const auto& vl = vlines[li];
                    for (size_t k = vl.first; k <= vl.semi && k < tokens.size(); ++k) {
                        tokens[k].mutable_.align.enabled = false;
                        tokens[k].mutable_.align.target_column = -1;
                    }

                    int dim_col = vl.indent + effective_section1;
                    if (vl.packed_dim != npos) {
                        if (opts_.var_declaration.align_adaptive)
                            dim_col = std::max(dim_col,
                                               vl.indent + canonical_width(tokens, vl.first, vl.packed_dim) + 1);
                        tokens[vl.packed_dim].mutable_.align.enabled = true;
                        tokens[vl.packed_dim].mutable_.align.target_column = dim_col;
                    }

                    int name_col = group_has_dim ? vl.indent + effective_section1 + effective_section2
                                                 : vl.indent + effective_section1;
                    if (opts_.var_declaration.align_adaptive) {
                        name_col = std::max(
                            name_col,
                            vl.indent + option_width(canonical_width(tokens, vl.first, vl.first_name) + 1,
                                                     opts_));
                    }
                    if (vl.packed_dim != npos && opts_.var_declaration.align_adaptive) {
                        size_t close = tokens[vl.packed_dim].immutable.syntax.matching_token;
                        if (close != npos && close < vl.first_name)
                            name_col = std::max(name_col,
                                                dim_col + token_text_width(tokens, vl.packed_dim, close + 1) + 1);
                    }
                    size_t unpacked_dim = npos;
                    for (size_t k = vl.first_name + 1; k < vl.first_delim; ++k) {
                        if (kind_is(tokens[k], TK::OpenBracket)) {
                            unpacked_dim = k;
                            break;
                        }
                    }
                    tokens[vl.first_name].mutable_.align.enabled = true;
                    tokens[vl.first_name].mutable_.align.target_column = name_col;

                    auto align_unpacked_trailing = [&](size_t open_bracket, size_t after_target) {
                        if (open_bracket == npos)
                            return;
                        size_t close_bracket = tokens[open_bracket].immutable.syntax.matching_token;
                        if (close_bracket == npos)
                            return;

                        // Section 3 reserves the signal-name column width.
                        // The trailing unpacked dimension should begin after
                        // that reserved column, not after the raw token width
                        // of the name.  Adaptive mode uses the configured
                        // section width directly.  Non-adaptive mode widens
                        // the section for the whole group when one declaration
                        // has a longer name, so every trailing dimension starts
                        // in the same column.
                        int trailing_start = name_col + effective_section3;
                        tokens[open_bracket].mutable_.align.enabled = true;
                        tokens[open_bracket].mutable_.align.target_column = trailing_start;

                        if (after_target != npos) {
                            tokens[after_target].mutable_.align.enabled = true;
                            tokens[after_target].mutable_.align.target_column =
                                opts_.var_declaration.align_adaptive
                                    ? trailing_start + token_text_width(tokens, open_bracket, close_bracket + 1) + section4
                                    : trailing_start + effective_section4;
                        }
                    };

                    auto align_delim = [&](size_t name, size_t delim) {
                        if (delim != npos && delim < tokens.size()) {
                            tokens[delim].mutable_.align.enabled = true;
                            tokens[delim].mutable_.align.target_column = tokens[name].mutable_.align.target_column +
                                                                         effective_section3 + effective_section4;
                        }
                    };

                    if (vl.eq != npos) {
                        if (unpacked_dim != npos)
                            align_unpacked_trailing(unpacked_dim, vl.eq);
                        tokens[vl.eq].mutable_.align.enabled = true;
                        tokens[vl.eq].mutable_.align.target_column = name_col + effective_section3;
                        tokens[vl.semi].mutable_.align.enabled = true;
                        tokens[vl.semi].mutable_.align.target_column =
                            opts_.var_declaration.align_adaptive
                                ? tokens[vl.eq].mutable_.align.target_column + section4
                                : name_col + effective_section3 + effective_section4;
                    } else {
                        if (unpacked_dim != npos)
                            align_unpacked_trailing(unpacked_dim, vl.first_delim);
                        align_delim(vl.first_name, vl.first_delim);
                    }

                    if (vl.eq == npos) {
                        size_t delim = vl.first_delim;
                        size_t begin = delim + 1;
                        while (delim != vl.semi) {
                            size_t next_delim = first_top_level_delim(begin, vl.semi);
                            size_t name = previous_decl_name(begin, next_delim);
                            if (name != npos) {
                                tokens[name].mutable_.align.enabled = true;
                                tokens[name].mutable_.align.target_column =
                                    tokens[delim].mutable_.align.target_column + 2;
                                align_delim(name, next_delim);
                            }
                            delim = next_delim;
                            begin = delim + 1;
                        }
                    }
                }
                i = j;
            }
        }

        if (opts_.port_declaration.align) {
            // Non-ANSI port declarations live as ordinary statements after the
            // module header, for example:
            //
            //   input logic [1:0] i_data [7:0];
            //   output packet_t [0:0] test, VSS;
            //
            // They are not list items inside the module-header parentheses, so
            // the ModulePorts list aligner below cannot own them.  Treat each
            // declaration line as a five-section record:
            //
            //   direction | type/signing | packed dim | first name | unpacked dim / separator
            //
            // The target columns are derived only from TokenKind structure and
            // formatter options.  We deliberately clear earlier declaration
            // alignment for the line because the generic var-declaration path
            // identifies the last identifier before ';' as the name, which is
            // wrong for comma-separated ports (`a, b`) and creates the compact
            // misalignment seen in memory_top.sv.
            struct NonAnsiPortDecl {
                bool valid{false};
                size_t line_index{npos};
                size_t first{npos};
                size_t semi{npos};
                size_t first_delim{npos};
                size_t first_name{npos};
                size_t type_first{npos};
                size_t packed_dim{npos};
                size_t unpacked_dim{npos};
                int indent{0};
                int direction_width{0};
                int type_width{0};
                int packed_width{0};
                int name_width{0};
            };

            auto previous_port_declarator_name = [&](size_t begin, size_t end) {
                int local_pd = 0, local_brd = 0;
                for (size_t n = end; n > begin; --n) {
                    size_t k = n - 1;
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::CloseParenthesis)) ++local_pd;
                    else if (kind_is(tokens[k], TK::OpenParenthesis) && local_pd > 0) --local_pd;
                    else if (kind_is(tokens[k], TK::CloseBrace)) ++local_brd;
                    else if ((kind_is(tokens[k], TK::OpenBrace) ||
                              kind_is(tokens[k], TK::ApostropheOpenBrace)) && local_brd > 0) --local_brd;
                    if (local_pd != 0 || local_brd != 0)
                        continue;

                    // Skip unpacked dimensions that belong to the declarator
                    // name rather than the type.  This lets `i_clk [1:0]` be
                    // parsed with `i_clk` as section 4 and `[1:0]` as section
                    // 5 instead of accidentally selecting an identifier inside
                    // an expression-sized dimension.
                    if (kind_is(tokens[k], TK::CloseBracket)) {
                        size_t open = tokens[k].immutable.syntax.matching_token;
                        if (open != npos && open > begin && open < k)
                            n = open + 1;
                        continue;
                    }
                    if (is_identifier_like(tokens[k]))
                        return k;
                }
                return npos;
            };

            auto parse_non_ansi_port_line = [&](size_t line_index) {
                NonAnsiPortDecl out;
                const auto& ln = lines[line_index];
                if (ln.first == npos || !is_port_direction(tokens[ln.first].lex.kind) ||
                    tokens[ln.first].immutable.syntax.paren_depth != 0)
                    return out;

                size_t semi = npos;
                int pd = 0, bd = 0, brd = 0;
                for (size_t k = ln.first + 1; k < ln.end; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenParenthesis)) ++pd;
                    else if (kind_is(tokens[k], TK::CloseParenthesis) && pd > 0) --pd;
                    else if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (kind_is(tokens[k], TK::OpenBrace) || kind_is(tokens[k], TK::ApostropheOpenBrace)) ++brd;
                    else if (kind_is(tokens[k], TK::CloseBrace) && brd > 0) --brd;
                    if (pd == 0 && bd == 0 && brd == 0 && kind_is(tokens[k], TK::Semicolon)) {
                        semi = k;
                        break;
                    }
                }
                if (semi == npos)
                    return out;

                size_t first_delim = semi;
                bd = 0;
                for (size_t k = ln.first + 1; k < semi; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (bd == 0 && kind_is(tokens[k], TK::Comma)) {
                        first_delim = k;
                        break;
                    }
                }

                size_t first_name = previous_port_declarator_name(ln.first + 1, first_delim);
                if (first_name == npos)
                    return out;

                size_t type_first = next_code(tokens, ln.first + 1, first_name);
                size_t packed_dim = npos;
                for (size_t k = ln.first + 1; k < first_name; ++k) {
                    if (kind_is(tokens[k], TK::OpenBracket)) {
                        packed_dim = k;
                        break;
                    }
                }
                if (type_first != npos && packed_dim != npos && type_first >= packed_dim)
                    type_first = npos;
                if (type_first == first_name)
                    type_first = npos;

                size_t unpacked_dim = npos;
                for (size_t k = first_name + 1; k < first_delim; ++k) {
                    if (kind_is(tokens[k], TK::OpenBracket)) {
                        unpacked_dim = k;
                        break;
                    }
                }

                out.valid = true;
                out.line_index = line_index;
                out.first = ln.first;
                out.semi = semi;
                out.first_delim = first_delim;
                out.first_name = first_name;
                out.type_first = type_first;
                out.packed_dim = packed_dim;
                out.unpacked_dim = unpacked_dim;
                out.indent = tokens[ln.first].mutable_.indent.base_indent;
                out.direction_width = token_width(tokens[ln.first]);
                out.type_width = type_first == npos
                                     ? 0
                                     : canonical_width(tokens, type_first,
                                                       packed_dim != npos ? packed_dim : first_name);
                if (packed_dim != npos) {
                    size_t packed_close = tokens[packed_dim].immutable.syntax.matching_token;
                    if (packed_close != npos && packed_close < first_name)
                        out.packed_width = token_text_width(tokens, packed_dim, packed_close + 1);
                }
                out.name_width = token_width(tokens[first_name]);
                return out;
            };

            std::vector<NonAnsiPortDecl> port_lines(lines.size());
            for (size_t li = 0; li < lines.size(); ++li)
                port_lines[li] = parse_non_ansi_port_line(li);

            for (size_t li = 0; li < lines.size();) {
                if (!port_lines[li].valid) {
                    ++li;
                    continue;
                }

                size_t j = li;
                int indent = port_lines[li].indent;
                bool group_has_packed = false;
                int group_direction_width = 0;
                int group_type_width = 0;
                int group_packed_width = 0;
                int group_name_width = 0;
                while (j < lines.size() && port_lines[j].valid && port_lines[j].indent == indent) {
                    const auto& pl = port_lines[j];
                    group_has_packed = group_has_packed || pl.packed_dim != npos;
                    group_direction_width = std::max(group_direction_width, pl.direction_width);
                    group_type_width = std::max(group_type_width, pl.type_width);
                    group_packed_width = std::max(group_packed_width, pl.packed_width);
                    group_name_width = std::max(group_name_width, pl.name_width);
                    ++j;
                }

                const int s1 = option_width(opts_.port_declaration.section1_min_width, opts_);
                const int s2 = option_width(opts_.port_declaration.section2_min_width, opts_);
                const int s3 = option_width(opts_.port_declaration.section3_min_width, opts_);
                const int s4 = option_width(opts_.port_declaration.section4_min_width, opts_);
                const int s5 = option_width(opts_.port_declaration.section5_min_width, opts_);

                const int effective_s1 = opts_.port_declaration.align_adaptive
                                             ? s1
                                             : option_width(std::max(opts_.port_declaration.section1_min_width,
                                                                     group_direction_width + 1), opts_);
                const int effective_s2 = opts_.port_declaration.align_adaptive
                                             ? s2
                                             : option_width(std::max(opts_.port_declaration.section2_min_width,
                                                                     group_type_width + (group_has_packed ? 1 : 3)), opts_);
                const int effective_s3 = opts_.port_declaration.align_adaptive
                                             ? s3
                                             : option_width(std::max(opts_.port_declaration.section3_min_width,
                                                                     group_packed_width + 1), opts_);
                const int effective_s4 = opts_.port_declaration.align_adaptive
                                             ? s4
                                             : option_width(std::max(opts_.port_declaration.section4_min_width,
                                                                     group_name_width + 1), opts_);

                for (size_t gi = li; gi < j; ++gi) {
                    const auto& pl = port_lines[gi];
                    for (size_t k = pl.first; k <= pl.semi && k < tokens.size(); ++k) {
                        tokens[k].mutable_.align.enabled = false;
                        tokens[k].mutable_.align.target_column = -1;
                    }

                    const int base = pl.indent;
                    const int type_col = opts_.port_declaration.align_adaptive
                                             ? base + option_width(std::max(opts_.port_declaration.section1_min_width,
                                                                           pl.direction_width + 1), opts_)
                                             : base + effective_s1;
                    const int packed_min_col = type_col + effective_s2;
                    const int name_min_col = group_has_packed ? packed_min_col + effective_s3
                                                              : type_col + effective_s2;
                    const int trailing_gap = opts_.tab_align || !opts_.port_declaration.align_adaptive ? 0 : 5;
                    const int unpacked_col = name_min_col + effective_s4 + trailing_gap;
                    const int first_sep_col = unpacked_col + s5;

                    if (pl.type_first != npos) {
                        tokens[pl.type_first].mutable_.align.enabled = true;
                        tokens[pl.type_first].mutable_.align.target_column = type_col;
                    }

                    int packed_col = packed_min_col;
                    if (pl.packed_dim != npos) {
                        if (opts_.port_declaration.align_adaptive && pl.type_first != npos) {
                            int type_end_col = type_col + pl.type_width;
                            if (is_identifier_like(tokens[pl.type_first]) && pl.type_width < s2)
                                packed_col = type_end_col + 3;
                            else
                                packed_col = std::max(packed_col, type_end_col + 1);
                        }
                        tokens[pl.packed_dim].mutable_.align.enabled = true;
                        tokens[pl.packed_dim].mutable_.align.target_column = packed_col;
                    }

                    int name_col = name_min_col;
                    if (opts_.port_declaration.align_adaptive && pl.packed_dim != npos) {
                        size_t packed_close = tokens[pl.packed_dim].immutable.syntax.matching_token;
                        if (packed_close != npos && packed_close < pl.first_name)
                            name_col = std::max(name_col,
                                                packed_col + token_text_width(tokens, pl.packed_dim,
                                                                              packed_close + 1) + 1);
                    }
                    tokens[pl.first_name].mutable_.align.enabled = true;
                    tokens[pl.first_name].mutable_.align.target_column = name_col;

                    if (pl.unpacked_dim != npos) {
                        tokens[pl.unpacked_dim].mutable_.align.enabled = true;
                        tokens[pl.unpacked_dim].mutable_.align.target_column = unpacked_col;
                    }
                    tokens[pl.first_delim].mutable_.align.enabled = true;
                    tokens[pl.first_delim].mutable_.align.target_column =
                        (opts_.port_declaration.align_adaptive && pl.first_delim == pl.semi &&
                         pl.unpacked_dim == npos && pl.packed_dim != npos)
                            ? (name_col + effective_s4 + trailing_gap + s5)
                            : first_sep_col;

                    // Subsequent comma declarators on the same non-ANSI line
                    // are still aligned relative to the previous separator.
                    // This keeps `input logic a, b;` readable without trying
                    // to fold secondary names into the cross-line section
                    // model, which only describes the first declarator.
                    int next_name_col = first_sep_col + 2;
                    size_t delim = pl.first_delim;
                    size_t item_begin = delim + 1;
                    while (delim != pl.semi) {
                        size_t next_delim = pl.semi;
                        int bd2 = 0;
                        for (size_t k = item_begin; k < pl.semi; ++k) {
                            if (!is_code_token(tokens[k])) continue;
                            if (kind_is(tokens[k], TK::OpenBracket)) ++bd2;
                            else if (kind_is(tokens[k], TK::CloseBracket) && bd2 > 0) --bd2;
                            else if (bd2 == 0 && kind_is(tokens[k], TK::Comma)) {
                                next_delim = k;
                                break;
                            }
                        }
                        size_t name = previous_port_declarator_name(item_begin, next_delim);
                        if (name != npos) {
                            tokens[name].mutable_.align.enabled = true;
                            tokens[name].mutable_.align.target_column = next_name_col;
                            tokens[next_delim].mutable_.align.enabled = true;
                            tokens[next_delim].mutable_.align.target_column = next_name_col + effective_s4 + s5;
                        }
                        delim = next_delim;
                        item_begin = delim + 1;
                        next_name_col = next_name_col + effective_s4 + s5 + 2;
                    }
                }

                li = j;
            }

            for (const auto& ln : lines) {
                if (ln.first == npos || !is_port_direction(tokens[ln.first].lex.kind) ||
                    tokens[ln.first].immutable.syntax.paren_depth == 0)
                    continue;
                size_t dim = npos;
                for (size_t k = ln.first + 1; k < ln.end; ++k) {
                    if (kind_is(tokens[k], TK::Semicolon) || kind_is(tokens[k], TK::Comma))
                        break;
                    if (kind_is(tokens[k], TK::OpenBracket)) {
                        dim = k;
                        break;
                    }
                }
                if (dim == npos)
                    continue;
                size_t type_first = next_code(tokens, ln.first + 1, dim);
                if (type_first == npos || type_first >= dim)
                    continue;
                int base = tokens[ln.first].mutable_.indent.base_indent;
                int type_col = base + token_width(tokens[ln.first]) + 1;
                tokens[type_first].mutable_.align.enabled = true;
                tokens[type_first].mutable_.align.target_column = type_col;
                int type_width = canonical_width(tokens, type_first, dim);
                int dim_target = type_col + type_width + 3;
                if (!opts_.port_declaration.align_adaptive)
                    dim_target = std::max(dim_target, base + opts_.port_declaration.section1_min_width +
                                                      opts_.port_declaration.section2_min_width);
                tokens[dim].mutable_.align.enabled = true;
                tokens[dim].mutable_.align.target_column = dim_target;
            }
        }

        auto align_declaration_items = [&](WrapListKind list_kind) {
            for (size_t open = 0; open < tokens.size(); ++open) {
                if (tokens[open].mutable_.wrap.list_kind != list_kind ||
                    tokens[open].mutable_.wrap.list_open != open)
                    continue;
                size_t close = tokens[open].immutable.syntax.matching_token;
                if (close == npos) continue;
                auto items = top_level_list_items(tokens, open + 1, close);
                struct Decl {
                    size_t first;
                    size_t type_first;
                    size_t packed_dim;
                    size_t name;
                    size_t unpacked_dim;
                    size_t comma;
                    int typew;
                };
                std::vector<Decl> decls;
                int type_width = opts_.port_declaration.section2_min_width;
                for (const auto& item : items) {
                    if (!is_port_direction(tokens[item.first].lex.kind))
                        continue;
                    size_t name = npos;
                    int pd = 0, bd = 0, brd = 0;
                    for (size_t n = item.last + 1; n > item.first + 1; --n) {
                        size_t k = n - 1;
                        if (!is_code_token(tokens[k])) continue;
                        if (kind_is(tokens[k], TK::CloseParenthesis)) ++pd;
                        else if (kind_is(tokens[k], TK::OpenParenthesis) && pd > 0) --pd;
                        else if (kind_is(tokens[k], TK::CloseBracket)) ++bd;
                        else if (kind_is(tokens[k], TK::OpenBracket) && bd > 0) --bd;
                        else if (kind_is(tokens[k], TK::CloseBrace)) ++brd;
                        else if ((kind_is(tokens[k], TK::OpenBrace) ||
                                  kind_is(tokens[k], TK::ApostropheOpenBrace)) && brd > 0) --brd;
                        if (pd == 0 && bd == 0 && brd == 0 && is_identifier_like(tokens[k])) {
                            name = k;
                            break;
                        }
                    }
                    if (name == npos || name <= item.first)
                        continue;
                    size_t type_first = next_code(tokens, item.first + 1, name);
                    size_t packed_dim = npos;
                    for (size_t k = item.first + 1; k < name; ++k) {
                        if (kind_is(tokens[k], TK::OpenBracket)) {
                            packed_dim = k;
                            break;
                        }
                    }
                    int tw = type_first == npos
                                 ? 0
                                 : canonical_width(tokens, type_first, packed_dim != npos ? packed_dim : name);
                    type_width = std::max(type_width, tw);
                    size_t unpacked_dim = npos;
                    for (size_t k = name + 1; k < item.last; ++k) {
                        if (kind_is(tokens[k], TK::OpenBracket)) {
                            unpacked_dim = k;
                            break;
                        }
                    }
                    decls.push_back({item.first, type_first, packed_dim, name, unpacked_dim, item.comma, tw});
                }
                int base = decls.empty() ? 0 : tokens[decls.front().first].mutable_.indent.base_indent;
                const int s1 = option_width(opts_.port_declaration.section1_min_width, opts_);
                const int s2 = option_width(opts_.port_declaration.section2_min_width, opts_);
                const int s3 = option_width(opts_.port_declaration.section3_min_width, opts_);
                const int s4 = option_width(opts_.port_declaration.section4_min_width, opts_);
                const int s5 = option_width(opts_.port_declaration.section5_min_width, opts_);
                // ANSI headers only need section-5 comma alignment when the
                // group actually contains trailing content after the port
                // name.  If every item is just `dir type name,` then pushing
                // the comma out to the section-5 column creates the
                // over-indented `name               ,` shape that breaks the
                // compact ANSI style tests.
                //
                // When at least one item has an unpacked dimension, comma
                // alignment becomes useful because otherwise the comma on a
                // shorter item can visually stop before the group reaches its
                // trailing section.  However, the configured section widths
                // are still per-declaration widths:
                //
                //   name column --section4--> section-5 / unpacked dimension
                //   section-5 start --section5--> comma
                //
                // In other words, `[1:0]` is content inside section 5; it is
                // not extra text before section 5.  Adding its width to the
                // comma target makes `section5_min_width = 14` behave like
                // `19` for a five-column unpacked dimension.  We only need the
                // boolean "does this group have a trailing section at all?" to
                // decide whether bare `dir type name,` items should reserve
                // section4+section5 space.
                bool group_has_trailing = false;
                bool group_has_packed = false;
                int group_direction_width = 0;
                int group_type_width = 0;
                int group_packed_width = 0;
                int group_name_width = 0;
                for (const auto& d : decls) {
                    group_direction_width =
                        std::max(group_direction_width, token_width(tokens[d.first]));
                    group_type_width = std::max(group_type_width, d.typew);
                    if (d.packed_dim != npos) {
                        group_has_packed = true;
                        size_t packed_close = tokens[d.packed_dim].immutable.syntax.matching_token;
                        if (packed_close != npos && packed_close < d.name)
                            group_packed_width =
                                std::max(group_packed_width,
                                         token_text_width(tokens, d.packed_dim, packed_close + 1));
                    }
                    group_name_width = std::max(group_name_width, token_width(tokens[d.name]));
                    if (d.unpacked_dim != npos) {
                        group_has_trailing = true;
                    }
                }
                // `align_adaptive=true` preserves the historical behavior:
                // each later boundary may adapt to the current declaration's
                // actual text, while still sharing the common base columns.
                //
                // `align_adaptive=false` is stricter.  Each configured section
                // width is widened once for the whole group based on the
                // longest content in that section, then every declaration uses
                // those same section starts.  This prevents a long packed
                // dimension on one port from shifting only that port's name
                // column while shorter packed dimensions keep the old column.
                const int effective_s1 =
                    opts_.port_declaration.align_adaptive
                        ? s1
                        : option_width(std::max(opts_.port_declaration.section1_min_width,
                                                group_direction_width + 1), opts_);
                const int effective_s2 =
                    opts_.port_declaration.align_adaptive
                        ? s2
                        // Historical ANSI alignment keeps a wider visual gap
                        // between a long type and the name when there is no
                        // packed-dimension section.  Preserve that behavior
                        // for no-packed groups, but use the ordinary single
                        // separating space when section2 is followed by an
                        // explicit packed-dimension section.
                        : option_width(std::max(opts_.port_declaration.section2_min_width,
                                                group_type_width + (group_has_packed ? 1 : 3)), opts_);
                const int effective_s3 =
                    opts_.port_declaration.align_adaptive
                        ? s3
                        : option_width(std::max(opts_.port_declaration.section3_min_width,
                                                group_packed_width + 1), opts_);

                int dir_target = base + effective_s1;
                int type_target = base + effective_s1;
                int packed_min_col = type_target + effective_s2;
                int name_target = opts_.port_declaration.align_adaptive
                                      ? dir_target + type_width
                                      : (group_has_packed ? packed_min_col + effective_s3
                                                          : type_target + effective_s2);
                for (const auto& d : decls) {
                    if (d.type_first != npos) {
                        tokens[d.type_first].mutable_.align.enabled = true;
                        tokens[d.type_first].mutable_.align.target_column = type_target;
                        if (opts_.port_declaration.align_adaptive)
                            name_target = std::max(name_target, dir_target + d.typew + 3);
                    }
                }
                for (const auto& d : decls) {
                    int decl_name_target = name_target;
                    int packed_col = packed_min_col;
                    if (d.packed_dim != npos && d.type_first != npos) {
                        int packed_width = canonical_width(tokens, d.type_first, d.packed_dim);
                        if (opts_.port_declaration.align_adaptive)
                            packed_col = std::max(packed_col, type_target + packed_width + 1);
                        tokens[d.packed_dim].mutable_.align.enabled = true;
                        tokens[d.packed_dim].mutable_.align.target_column = packed_col;
                    }

                    if (d.packed_dim != npos && opts_.port_declaration.align_adaptive) {
                        size_t packed_close = tokens[d.packed_dim].immutable.syntax.matching_token;
                        if (packed_close != npos && packed_close < d.name) {
                            decl_name_target = std::max(
                                decl_name_target,
                                packed_col + token_text_width(tokens, d.packed_dim, packed_close + 1) + 1);
                        }
                        decl_name_target = std::max(decl_name_target, packed_col + effective_s3);
                    }
                    tokens[d.name].mutable_.align.enabled = true;
                    tokens[d.name].mutable_.align.target_column = decl_name_target;

                    // Adaptive alignment keeps each declaration local, but it
                    // still must never let two sections overlap.  If the
                    // current name is longer than section4, widen only this
                    // declaration's name section enough to keep one separating
                    // space before the trailing unpacked dimensions.
                    //
                    // Non-adaptive alignment instead makes the section
                    // boundary group-wide: if one name is longer than
                    // section4, every other declaration's trailing section
                    // starts after that longest name as well.
                    //
                    // Example with section4=13 and longest name
                    // `i_diveeeeeeeee` (14 columns):
                    //
                    //   i_clk          [1:0]
                    //   i_diveeeeeeeee [1:0]
                    //
                    // The `+ 1` reserves the normal separating space between
                    // the section-4 content and section-5 content.
                    const int effective_s4 =
                        opts_.port_declaration.align_adaptive
                            ? option_width(std::max(opts_.port_declaration.section4_min_width,
                                                    token_width(tokens[d.name]) + 1), opts_)
                            : option_width(std::max(opts_.port_declaration.section4_min_width,
                                                    group_name_width + 1), opts_);

                    if (d.unpacked_dim != npos) {
                        size_t close = tokens[d.unpacked_dim].immutable.syntax.matching_token;
                        if (close == npos)
                            continue;
                        int trailing_start = decl_name_target + effective_s4;
                        tokens[d.unpacked_dim].mutable_.align.enabled = true;
                        tokens[d.unpacked_dim].mutable_.align.target_column = trailing_start;
                        if (d.comma != npos && group_has_trailing) {
                            bool skip_comma_align = false;
                            for (size_t k = d.comma + 1; k < tokens.size(); ++k) {
                                if (tokens[k].lex.comment_kind != CommentLexemeKind::None ||
                                    tokens[k].lex.is_directive) {
                                    skip_comma_align = true;
                                    break;
                                }
                                if (is_code_token(tokens[k]))
                                    break;
                            }
                            if (skip_comma_align)
                            {
                                tokens[d.comma].mutable_.align.enabled = false;
                                tokens[d.comma].mutable_.align.target_column = -1;
                                continue;
                            }
                            tokens[d.comma].mutable_.align.enabled = true;
                            tokens[d.comma].mutable_.align.target_column = trailing_start + s5;
                        }
                    } else if (d.comma != npos) {
                        if (!group_has_trailing) {
                            tokens[d.comma].mutable_.align.enabled = false;
                            tokens[d.comma].mutable_.align.target_column = -1;
                            continue;
                        }
                        bool skip_comma_align = false;
                        for (size_t k = d.comma + 1; k < tokens.size(); ++k) {
                            if (tokens[k].lex.comment_kind != CommentLexemeKind::None ||
                                tokens[k].lex.is_directive) {
                                skip_comma_align = true;
                                break;
                            }
                            if (is_code_token(tokens[k]))
                                break;
                        }
                        if (skip_comma_align)
                        {
                            tokens[d.comma].mutable_.align.enabled = false;
                            tokens[d.comma].mutable_.align.target_column = -1;
                            continue;
                        }
                        tokens[d.comma].mutable_.align.enabled = true;
                        tokens[d.comma].mutable_.align.target_column =
                            decl_name_target + effective_s4 + s5;
                    }
                }
            }
        };

        if (opts_.port_declaration.align)
            align_declaration_items(WrapListKind::ModulePorts);

        // Instance named-port alignment.  WrapPass owns the decision to expand
        // the list; AlignPass only assigns target columns for the connection
        // parens and optional inside padding.
        int group = 1000;
        for (size_t open = 0; open < tokens.size(); ++open) {
            if (!opts_.instance.align)
                break;
            if (tokens[open].mutable_.wrap.list_kind != WrapListKind::InstancePorts ||
                tokens[open].mutable_.wrap.list_open != open)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos) continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            int max_port = option_width(opts_.instance.instance_port_name_width, opts_);
            int max_sig = opts_.instance.instance_port_between_paren_width;
            struct Conn { size_t name, op, cl; int namew, sigw; };
            std::vector<Conn> conns;
            for (const auto& item : items) {
                size_t dot = item.first;
                size_t name = next_code(tokens, dot + 1, item.last + 1);
                size_t op = name == npos ? npos : next_code(tokens, name + 1, item.last + 1);
                if (name == npos || op == npos || !kind_is(tokens[dot], TK::Dot) ||
                    !kind_is(tokens[op], TK::OpenParenthesis))
                    continue;
                size_t cl = tokens[op].immutable.syntax.matching_token;
                if (cl == npos || cl > item.last) continue;
                int nw = token_width(tokens[name]);
                int sw = compact_width(tokens, op + 1, cl);
                max_port = std::max(max_port, nw);
                max_sig = std::max(max_sig, sw);
                conns.push_back({name, op, cl, nw, sw});
            }
            for (const auto& c : conns) {
                int item_indent = tokens[prev_code(tokens, c.name)].mutable_.indent.base_indent;
                int port_width = opts_.instance.align_adaptive
                    ? std::max(option_width(opts_.instance.instance_port_name_width, opts_), c.namew)
                    : max_port;
                int sig_width = opts_.instance.align_adaptive
                    ? std::max(opts_.instance.instance_port_between_paren_width, c.sigw)
                    : max_sig;
                tokens[c.op].mutable_.align.enabled = true;
                tokens[c.op].mutable_.align.alignment_group = group;
                int configured_port_width = option_width(opts_.instance.instance_port_name_width, opts_);
                tokens[c.op].mutable_.align.target_column = item_indent +
                    (c.namew >= configured_port_width ? c.namew + 2 : port_width);
                tokens[c.cl].mutable_.align.enabled = true;
                tokens[c.cl].mutable_.align.alignment_group = group;
                tokens[c.cl].mutable_.align.target_column =
                    tokens[c.op].mutable_.align.target_column + 1 + sig_width;
            }
            ++group;
        }

        for (size_t open = 0; open < tokens.size(); ++open) {
            if (tokens[open].mutable_.wrap.list_open != open)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos) continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty()) continue;

            if (tokens[open].mutable_.wrap.list_kind == WrapListKind::EnumBody &&
                opts_.enum_declaration.align) {
                struct E { size_t first, eq, comma; int namew, valw; };
                std::vector<E> es;
                int namew = option_width(opts_.enum_declaration.enum_name_min_width, opts_);
                int valw = option_width(opts_.enum_declaration.enum_value_min_width, opts_);
                for (const auto& item : items) {
                    size_t eq = npos;
                    for (size_t k = item.first; k <= item.last; ++k)
                        if (kind_is(tokens[k], TK::Equals)) { eq = k; break; }
                    int nw = eq == npos ? compact_width(tokens, item.first, item.last + 1)
                                         : compact_width(tokens, item.first, eq);
                    int vw = eq == npos ? 0 : compact_width(tokens, eq + 1, item.last + 1);
                    namew = std::max(namew, nw);
                    valw = std::max(valw, vw);
                    es.push_back({item.first, eq, item.comma, nw, vw});
                }
                int base = tokens[items.front().first].mutable_.indent.base_indent;
                for (const auto& e : es) {
                    int local_namew = opts_.enum_declaration.align_adaptive
                        ? std::max(option_width(opts_.enum_declaration.enum_name_min_width, opts_), e.namew)
                        : namew;
                    int local_valw = opts_.enum_declaration.align_adaptive
                        ? std::max(option_width(opts_.enum_declaration.enum_value_min_width, opts_), e.valw)
                        : valw;
                    int eq_target = base + (opts_.tab_align ? snap_to_grid(local_namew + 1, opts_.indent_size)
                                                            : local_namew + 1);
                    if (e.eq != npos) {
                        tokens[e.eq].mutable_.align.enabled = true;
                        tokens[e.eq].mutable_.align.target_column = eq_target;
                    }
                    if (e.comma != npos) {
                        tokens[e.comma].mutable_.align.enabled = true;
                        tokens[e.comma].mutable_.align.target_column =
                            opts_.tab_align ? (eq_target + snap_to_grid(std::max(1, local_valw) + opts_.indent_size,
                                                                         opts_.indent_size) - 1) :
                            (e.eq == npos) ? (local_valw > 0 ? (eq_target + 2 + local_valw)
                                                             : eq_target)
                                           : (eq_target + 2 + local_valw);
                    }
                }
            }

            if (tokens[open].mutable_.wrap.list_kind == WrapListKind::ModportBody &&
                opts_.modport.align) {
                struct M { size_t dir, sig, comma; int dirw, sigw; };
                std::vector<M> ms;
                int dirw = option_width(opts_.modport.direction_min_width, opts_);
                int sigw = option_width(opts_.modport.signal_min_width, opts_);
                for (const auto& item : items) {
                    size_t sig = next_code(tokens, item.first + 1, item.last + 1);
                    if (sig == npos) continue;
                    int dw = token_width(tokens[item.first]);
                    int sw = compact_width(tokens, sig, item.last + 1);
                    dirw = std::max(dirw, dw + 1);
                    sigw = std::max(sigw, sw);
                    ms.push_back({item.first, sig, item.comma, dw, sw});
                }
                int base = tokens[items.front().first].mutable_.indent.base_indent;
                dirw = option_width(dirw, opts_);
                sigw = option_width(sigw, opts_);
                for (const auto& m : ms) {
                    int local_sigw = opts_.modport.align_adaptive
                        ? std::max(option_width(opts_.modport.signal_min_width, opts_), m.sigw)
                        : sigw;
                    tokens[m.sig].mutable_.align.enabled = true;
                    tokens[m.sig].mutable_.align.target_column = base + dirw;
                    if (m.comma != npos) {
                        tokens[m.comma].mutable_.align.enabled = true;
                        tokens[m.comma].mutable_.align.target_column = base + dirw + local_sigw;
                    }
                }
            }
        }
    }
private: const FormatOptions& opts_;
};

// CommentPass owns CommentMetadata and reads stable syntax comment roles.
class CommentPass final : public IFormatPass {
public:
    const char* name() const override { return "comment"; }
    void run(TokenStream& tokens) override {
        for (auto& t : tokens) {
            if (t.lex.comment_kind == CommentLexemeKind::None)
                continue;
            if (t.immutable.comment.role == CommentRole::OwnLine)
                t.mutable_.comment.force_own_line = true;
        }
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
            if (no_space_before(t.lex.kind) || no_space_after(L.lex.kind)) spaces = 0;
            if (t.lex.comment_kind != CommentLexemeKind::None && kind_is(L, TK::OpenParenthesis))
                spaces = 1;
            // Empty positional argument: `, ,` — keep one space so the slot is visible
            if (kind_is(t, TK::Comma) && kind_is(L, TK::Comma)) spaces = 1;

            // Slang represents based literals as multiple adjacent tokens.
            // Preserve adjacency around the base marker and all following
            // body chunks:
            //
            //   12 'h 7 c4  ->  12'h7c4
            //   4  'b 10 xz ->  4'b10xz
            //
            // This rule is purely TokenKind-driven and therefore remains
            // idempotent after the first formatting pass.
            if ((kind_is(t, TK::IntegerBase) && kind_is(L, TK::IntegerLiteral)) ||
                is_based_literal_continuation(tokens, i))
                spaces = 0;

            // Unary ops: no space after.
            // Exception: `~ &a`, `~ |a`, `~ ^a` — tilde followed by a
            // reduction operator must keep a space so it isn't read as
            // the compound `~&` / `~|` / `~^` operator.
            if (is_unary_op(L.lex.kind)) {
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
            if ((kind_is(t, TK::Dot) && kind_is(L, TK::Comma))) {
                spaces = 1;
            } else if (kind_is(t, TK::Dot) || kind_is(t, TK::DoubleColon)) {
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
            if (kind_is(t, TK::OpenBracket) && is_identifier_like(L) &&
                t.immutable.syntax.matching_token != npos) {
                size_t after_dim = next_code(tokens, t.immutable.syntax.matching_token + 1, tokens.size());
                if (after_dim != npos && is_identifier_like(tokens[after_dim]))
                    spaces = 1;
                else if (is_var_declaration_trailing_dimension_open(tokens, i))
                    spaces = 1;
            }

            // Function/task declaration spacing has its own option because many
            // codebases prefer `foo (...)` for calls but `function foo(...)` for
            // declarations (or vice versa).  Check it before the generic
            // call-like rule below; wrapped and unwrapped declarations both
            // pass through this spacing pass.
            if (kind_is(t, TK::OpenParenthesis) && is_function_task_declaration_open(tokens, i))
                spaces = opts_.function_declaration.space_before_paren ? 1 : 0;
            // Function/task call spacing
            else if (kind_is(t, TK::OpenParenthesis) && (kind_is(L, TK::Identifier) || kind_is(L, TK::SystemIdentifier) || kind_is(L, TK::MacroUsage)))
                spaces = opts_.function_call.space_before_paren ? 1 : 0;
            if (kind_is(t, TK::OpenParenthesis) && t.mutable_.wrap.list_kind == WrapListKind::InstancePorts &&
                opts_.instance.align)
                spaces = 1;
            if (kind_is(t, TK::OpenParenthesis) && t.mutable_.wrap.list_kind == WrapListKind::ModportBody)
                spaces = 1;
            if (kind_is(t, TK::OpenParenthesis) && t.immutable.topology.starts_port_list &&
                kind_is(L, TK::CloseParenthesis))
                spaces = 0;
            if (kind_is(t, TK::OpenParenthesis) && is_control_keyword(L.lex.kind))
                spaces = opts_.spacing.control_keyword_space ? 1 : 0;
            if ((kind_is(L, TK::OpenParenthesis) || kind_is(t, TK::CloseParenthesis)) && opts_.spacing.space_inside_parens) spaces = 1;
            // function.space_inside_paren: space inside argument-list parens only
            if (kind_is(L, TK::OpenParenthesis) && L.immutable.topology.starts_argument_list && opts_.function_call.space_inside_paren) spaces = 1;
            if (kind_is(t, TK::CloseParenthesis) && t.immutable.topology.ends_argument_list && opts_.function_call.space_inside_paren) spaces = 1;
            if ((kind_is(L, TK::OpenBracket) || kind_is(t, TK::CloseBracket)) && opts_.spacing.space_inside_dimension_brackets) spaces = 1;

            // } brace: 1 space after (unless followed by ; or ,)
            if (kind_is(L, TK::CloseBrace) && !kind_is(t, TK::Semicolon) && !kind_is(t, TK::Comma)) spaces = 1;
            if (kind_is(L, TK::CloseBrace) && kind_is(t, TK::CloseParenthesis)) spaces = 0;

            // Apostrophe / cast: no space
            if (kind_is(t, TK::Apostrophe) || kind_is(L, TK::Apostrophe)) spaces = 0;
            // Apostrophe-open-brace assignment patterns / casts.
            if (kind_is(t, TK::ApostropheOpenBrace)) spaces = 0;

            // Postfix ++ / --: no space before when attached to an identifier, ], or )
            if ((kind_is(t, TK::DoublePlus) || kind_is(t, TK::DoubleMinus)) &&
                (is_identifier_like(L) || kind_is(L, TK::CloseBracket) || kind_is(L, TK::CloseParenthesis)))
                spaces = 0;

            // Assignment operators.
            // LessThanEquals is context-sensitive: inside parens it's a comparison, not
            // non-blocking assignment.  Treat it as a regular binary op in that context.
            auto is_assign = [&](const Tok& tok) {
                if (tok.lex.kind == TK::LessThanEquals && tok.immutable.syntax.paren_depth > 0)
                    return false;
                return is_assignment_op(tok.lex.kind);
            };
            if (is_assign(t)) spaces = wants_before(opts_.spacing.assignment_operator_spacing) ? 1 : 0;
            if (is_assign(L)) spaces = wants_after(opts_.spacing.assignment_operator_spacing) ? 1 : 0;

            // Binary operators (non-assignment).
            // Closing brackets carry depth=1 (before decrement), so exclude them from the
            // L-side dim check to avoid treating tokens after ] as inside a dimension.
            bool in_dim = t.immutable.syntax.bracket_depth > 0 ||
                          (L.immutable.syntax.bracket_depth > 0 && !kind_is(L, TK::CloseBracket));
            const std::string& bop_mode = in_dim ? opts_.spacing.dimension_binary_operator_spacing : opts_.spacing.binary_operator_spacing;
            if (is_binary_op(t.lex.kind) && !is_assign(t))
                spaces = wants_before(bop_mode) ? 1 : 0;
            if (is_binary_op(L.lex.kind) && !is_assign(L))
                spaces = wants_after(bop_mode) ? 1 : 0;
            if (is_binary_op(L.lex.kind) && !is_assign(L) &&
                can_begin_unary_expression(t.lex.kind)) {
                // Do not concatenate a binary operator with the unary operator
                // that starts its right-hand operand.  SystemVerilog has many
                // multi-character operator tokens, so removing this separator
                // can change the token stream on the next pass:
                //
                //   a && &b  -> a&&&b  // lexes as the single &&& token
                //   a + +b   -> a++b   // prefix/postfix increment ambiguity
                //   a - -b   -> a--b   // decrement ambiguity
                //   a ^ ~b   -> a^~b   // xnor token
                //
                // Keep one syntactic separator independent of the configured
                // binary_operator_spacing style.
                spaces = std::max(spaces, 1);
            }
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
                if (is_covergroup_event_at(tokens, i - 1))
                    standalone = true;
                spaces = (!standalone && wants_after(opts_.spacing.procedural_event_control_at_spacing)) ? 1 : 0;
            }
            // space_inside_event_control_parens: add space inside ( ) of procedural event control.
            // Only applies when ( directly follows @ which is not a standalone delay control.
            if (opts_.spacing.space_inside_event_control_parens) {
                if (kind_is(L, TK::OpenParenthesis) && i >= 2 && kind_is(tokens[i-2], TK::At)) {
                    bool standalone = i >= 3 && kind_is(tokens[i-3], TK::Semicolon);
                    if (is_covergroup_event_at(tokens, i - 2))
                        standalone = true;
                    if (!standalone) spaces = 1;
                }
                if (kind_is(t, TK::CloseParenthesis) && t.immutable.syntax.matching_token != npos) {
                    size_t j = t.immutable.syntax.matching_token;
                    if (j >= 1 && j < tokens.size() && kind_is(tokens[j], TK::OpenParenthesis) &&
                        j >= 1 && kind_is(tokens[j-1], TK::At)) {
                        bool standalone = j >= 2 && kind_is(tokens[j-2], TK::Semicolon);
                        if (is_covergroup_event_at(tokens, j - 1))
                            standalone = true;
                        if (!standalone) spaces = 1;
                    }
                }
            }

            // End-label colon: `endclass: Foo`, `endfunction: bar`, etc. — no space before `:`
            if (kind_is(t, TK::Colon) && (is_close_block(L.lex.kind) || is_outer_close(L.lex.kind) ||
                                           kind_is(L, TK::BeginKeyword) || kind_is(L, TK::ForkKeyword)))
                spaces = 0;
            if (kind_is(t, TK::Colon) && is_numeric(L) && !in_dim)
                spaces = 0;
            if (kind_is(t, TK::Colon) && is_identifier_like(L)) {
                size_t nx = next_code(tokens, i + 1, tokens.size());
                if (nx != npos && kind_is(tokens[nx], TK::CoverPointKeyword))
                    spaces = 0;
            }

            // semicolon_spacing: controls space before/after `;` inside for-loop headers
            // (paren_depth > 0 identifies the for(;;) context vs statement-ending `;`)
            if (kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth > 0)
                spaces = wants_before(opts_.spacing.semicolon_spacing) ? 1 : 0;
            if (kind_is(L, TK::Semicolon) && L.immutable.syntax.paren_depth > 0)
                spaces = wants_after(opts_.spacing.semicolon_spacing) ? 1 : 0;

            if ((kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth == 0) ||
                (kind_is(t, TK::Comma) && !kind_is(L, TK::Comma)) ||
                (kind_is(t, TK::Dot) && !kind_is(L, TK::Comma)) || kind_is(t, TK::DoubleColon))
                spaces = 0;
            if (kind_is(t, TK::CloseParenthesis) &&
                !opts_.spacing.space_inside_parens &&
                !(t.immutable.topology.ends_argument_list && opts_.function_call.space_inside_paren)) {
                bool event_control_close = false;
                if (opts_.spacing.space_inside_event_control_parens &&
                    t.immutable.syntax.matching_token != npos) {
                    size_t open = t.immutable.syntax.matching_token;
                    size_t before_open = prev_code(tokens, open);
                    event_control_close = before_open != npos && kind_is(tokens[before_open], TK::At);
                }
                if (!event_control_close)
                spaces = 0;
            }

            t.mutable_.space.spaces_before = std::max(0, spaces);
            t.mutable_.space.suppress_space = spaces == 0;
        }
    }
private: const FormatOptions& opts_;
};

// BlankLinePass owns BlankLineMetadata.  It intentionally does not copy the
// original source's blank-line count: once a syntactic blank-line boundary is
// accepted, the rendered amount is canonical and config-driven.
class BlankLinePass final : public IFormatPass {
public:
    explicit BlankLinePass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "blank_line"; }
    void run(TokenStream& tokens) override {
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            int boundary_newlines = t.immutable.input_trivia.original_newlines_before;

            // Passthrough tokens are rendered verbatim, so a physical item
            // boundary can be split across the previous token's immutable text
            // and this token's ordinary leading trivia.  This happens for
            // multiline macro definitions frozen as one whitespace-sensitive
            // directive:
            //
            //   `define FOO \
            //     foo();\n
            //   \n
            //   `define BAR ...
            //
            // The first newline is part of the raw `define token; the second
            // is the gap before `define BAR.  Together they represent one
            // blank separator line.  Keep this policy in BlankLinePass rather
            // than inventing lexer-side pending trivia after a token has
            // already been emitted.
            if (i > 0 && passthrough_text_ends_with_newline(tokens[i - 1]))
                boundary_newlines += 1;

            if (boundary_newlines > 1 &&
                t.immutable.syntax.paren_depth == 0 &&
                t.immutable.syntax.brace_depth == 0)
                t.mutable_.blank.before = std::max(0, opts_.blank_lines_between_items);
        }
    }
private:
    static bool passthrough_text_ends_with_newline(const Tok& tok) {
        return is_passthrough(tok) &&
               !tok.lex.text.empty() &&
               tok.lex.text.back() == '\n';
    }

    const FormatOptions& opts_;
};

} // namespace svfmt
