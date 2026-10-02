#pragma once

#include "formatter_token.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace svfmt {

using TK = slang::parsing::TokenKind;

inline bool kind_is(const Tok& t, TK k) { return t.lex.kind == k; }
inline bool is_passthrough(const Tok& t) { return t.mutable_.macro.passthrough || t.lex.is_whitespace_sensitive; }
inline size_t last_newline_offset(std::string_view text) {
    for (size_t n = text.size(); n > 0; --n)
        if (text[n - 1] == '\n')
            return n - 1;
    return std::string_view::npos;
}

// Whether BlankLinePass puts blank lines before `tokens[i]`.  It reads only
// immutable facts, so a pass that runs earlier -- AlignPass, walking the lines
// the renderer will emit -- can ask it too: a blank line also breaks the line.
inline bool is_blank_line_boundary(const TokenStream& tokens, size_t i) {
    const Tok& t = tokens[i];
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
    // blank separator line.  Keep this policy here rather than
    // inventing lexer-side pending trivia after a token has already
    // been emitted.
    if (i > 0 && is_passthrough(tokens[i - 1]) && !tokens[i - 1].lex.text.empty() &&
        tokens[i - 1].lex.text.back() == '\n')
        boundary_newlines += 1;

    return boundary_newlines > 1 &&
           t.immutable.syntax.paren_depth == 0 &&
           t.immutable.syntax.brace_depth == 0;
}
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
           k == TK::SpecifyKeyword || k == TK::TableKeyword || k == TK::ForkKeyword ||
           // Closed by `endcase` / `endsequence` like their plain forms.
           k == TK::RandCaseKeyword || k == TK::RandSequenceKeyword;
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
           k == TK::TripleEquals || k == TK::ExclamationDoubleEquals ||
           k == TK::DoubleEqualsQuestion || k == TK::ExclamationEqualsQuestion ||
           k == TK::DoubleStar || k == TK::LessThanMinusArrow ||
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
// The token form also turns away an intra-assignment `repeat`, which
// controls no statement.
inline bool is_single_stmt_control(const Tok& t) {
    return is_single_stmt_control(t.lex.kind) && !t.immutable.topology.is_intra_assignment_repeat;
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

// Would `a` written directly before `b` lex differently -- some operator
// starting inside `a` running on into `b` (`&` `&b` -> `&&`, `^` `~b` -> `^~`,
// `-` `-b` -> `--`)?  Maximal munch is the only way two adjacent operators
// can merge, so this is the whole question.
inline bool operators_would_merge(std::string_view a, std::string_view b) {
    static constexpr std::string_view kOps[] = {
        "**", "==", "!=", "===", "!==", "==?", "!=?", "<=", ">=", "&&", "||",
        "~&", "~|", "~^", "^~", "<<", ">>", "<<<", ">>>", "->", "<->", "->>",
        "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=",
        ">>=", "<<<=", ">>>=", "+:", "-:", "::", "&&&", "|->", "|=>", "=>",
        "*>", "(*", "*)", "//", "/*", "@@", "##", "#-#", "#=#", ".*",
    };
    for (size_t p = 0; p < a.size(); ++p) {
        const size_t tail = a.size() - p;
        for (std::string_view op : kOps) {
            if (op.size() <= tail || op.substr(0, tail) != a.substr(p))
                continue;
            if (b.substr(0, op.size() - tail) == op.substr(tail))
                return true;
        }
    }
    return false;
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
// `rand`/`randc` qualify a class property the way `static` does.
inline bool is_var_decl_leading_keyword(TK k) {
    return is_type_keyword(k) || k == TK::AutomaticKeyword || k == TK::StaticKeyword ||
           k == TK::ConstKeyword || k == TK::RandKeyword || k == TK::RandCKeyword;
}
// The net types `wire` is not: `tri0 t;`, `supply0 gnd;`, `wand w;`.
inline bool is_net_type_keyword(TK k) {
    return k == TK::TriKeyword || k == TK::Tri0Keyword || k == TK::Tri1Keyword ||
           k == TK::TriAndKeyword || k == TK::TriOrKeyword || k == TK::TriRegKeyword ||
           k == TK::WAndKeyword || k == TK::WOrKeyword || k == TK::UWireKeyword ||
           k == TK::Supply0Keyword || k == TK::Supply1Keyword;
}
inline bool is_port_direction(TK k) {
    return k == TK::InputKeyword || k == TK::OutputKeyword ||
           k == TK::InOutKeyword || k == TK::RefKeyword;
}

inline bool is_identifier_like(const Tok& t) {
    return t.lex.kind == TK::Identifier || t.lex.kind == TK::SystemIdentifier ||
           t.lex.kind == TK::MacroUsage;
}

inline bool is_code_token(const Tok& t) {
    return t.lex.comment_kind == CommentLexemeKind::None && !t.lex.is_directive && !is_passthrough(t);
}

// `if`/`else`/`case`/`endcase` used as property operators
// (`assert property (@(posedge c) if (a) b else c);`).  They take none of the
// statement layout their procedural spellings get.
//
// A property `case` outside every parenthesis is the exception.  Its items
// each end in a `;` and so end their lines whatever is decided here; treated
// as an operator it kept its first item on the header's line and the rest
// unindented.  It is laid out as the block it is written as.
//
// A randsequence production's `if`/`else`/`repeat` choose between
// productions the same way (`first: if (a) x else y;`), and its `case` is a
// block like the property one.
inline bool is_property_operator_keyword(const Tok& t) {
    const TK k = t.lex.kind;
    if (t.immutable.syntax.in_production)
        return k == TK::IfKeyword || k == TK::ElseKeyword || k == TK::RepeatKeyword;
    if (!t.immutable.syntax.in_property_expr)
        return false;
    if (k == TK::CaseKeyword || k == TK::EndCaseKeyword)
        return t.immutable.syntax.paren_depth > 0;
    return k == TK::IfKeyword || k == TK::ElseKeyword;
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

inline size_t prev_code(const TokenStream& tokens, size_t before);

// `clocking cb @(posedge clk);` -- the `@` after the name a clocking block
// declares.  `default clocking @(e);` has no name and is not one.
inline bool is_named_clocking_event_at(const TokenStream& tokens, size_t at) {
    if (at >= tokens.size() || !kind_is(tokens[at], TK::At))
        return false;
    const size_t name = prev_code(tokens, at);
    const size_t kw = name == npos ? npos : prev_code(tokens, name);
    return kw != npos && kind_is(tokens[name], TK::Identifier) && kind_is(tokens[kw], TK::ClockingKeyword);
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

// Operator position, decided from the code token before the operator.  `-`,
// `+`, `&`, `|`, `^`, `~&`, `~|`, `~^`, `^~` are unary after anything that
// cannot end an operand (`=`, `(`, `,`, `?`, another operator, a keyword) and
// binary after one that can.  `++`/`--` are postfix after an operand.
inline bool in_prefix_position(const TokenStream& tokens, size_t idx);

inline bool ends_operand(const TokenStream& tokens, size_t idx) {
    const Tok& t = tokens[idx];
    const TK k = t.lex.kind;
    if (k == TK::DoublePlus || k == TK::DoubleMinus)
        return !in_prefix_position(tokens, idx);
    return is_identifier_like(t) || t.lex.continues_vector_literal ||
           k == TK::IntegerLiteral || k == TK::RealLiteral || k == TK::TimeLiteral ||
           k == TK::UnbasedUnsizedLiteral || k == TK::StringLiteral || k == TK::Dollar ||
           k == TK::CloseParenthesis || k == TK::CloseBracket || k == TK::CloseBrace ||
           k == TK::ThisKeyword || k == TK::SuperKeyword || k == TK::NullKeyword;
}

inline bool in_prefix_position(const TokenStream& tokens, size_t idx) {
    const size_t p = prev_code(tokens, idx);
    return p == npos || !ends_operand(tokens, p);
}

// A unary operator directly followed by the start of its operand, where
// closing the gap would re-lex the pair as another token: `~ &a` (`~&`),
// `- -b` (`--`), `^ ~a` (`^~`), `& &b` (`&&`).
inline bool unary_pair_merges(TK l, TK t) {
    switch (l) {
    case TK::Plus: case TK::DoublePlus:
        return t == TK::Plus || t == TK::DoublePlus;
    case TK::Minus: case TK::DoubleMinus:
        return t == TK::Minus || t == TK::DoubleMinus;
    case TK::And: case TK::TildeAnd:
        return t == TK::And;
    case TK::Or: case TK::TildeOr:
        return t == TK::Or;
    case TK::Xor: case TK::TildeXor: case TK::XorTilde:
        return t == TK::Tilde || t == TK::TildeAnd || t == TK::TildeOr || t == TK::TildeXor;
    case TK::Tilde:
        return t == TK::And || t == TK::Or || t == TK::Xor || t == TK::XorTilde;
    default:
        return false;
    }
}

// `initial`/`always*`/`final` opening a procedural block.  A deferred
// assertion's `final` (`assert final (c);`, also `assume`/`cover`) is not
// one: it has no body, only the assertion's condition.
inline bool is_procedural_block_at(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size() || !is_procedural_block_keyword(tokens[idx].lex.kind))
        return false;
    // `s_eventually always a`, `property p; always a; endproperty` -- inside a
    // property `always` is an operator (IEEE 1800 16.12.11), not a process.
    if (kind_is(tokens[idx], TK::AlwaysKeyword) && tokens[idx].immutable.syntax.in_property_expr)
        return false;
    if (!kind_is(tokens[idx], TK::FinalKeyword))
        return true;
    const size_t p = prev_code(tokens, idx);
    return !(p != npos && (kind_is(tokens[p], TK::AssertKeyword) || kind_is(tokens[p], TK::AssumeKeyword) ||
                           kind_is(tokens[p], TK::CoverKeyword)));
}

// Operators whose spelling is also a unary operator.
inline bool is_sign_or_reduction_op(TK k) {
    return k == TK::Plus || k == TK::Minus || k == TK::And || k == TK::Or || k == TK::Xor ||
           k == TK::TildeAnd || k == TK::TildeOr || k == TK::TildeXor || k == TK::XorTilde;
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

inline bool is_covergroup_sample_function_header(const TokenStream& tokens, size_t function_idx) {
    if (function_idx >= tokens.size() || !kind_is(tokens[function_idx], TK::FunctionKeyword))
        return false;

    // SystemVerilog has a contextual `function` use in covergroup headers:
    //
    //   covergroup cg with function sample(...);
    //   covergroup cg @(posedge clk) with function sample(...);
    //
    // That `function` declares the covergroup's sample method signature.  It is
    // not a function body and has no matching `endfunction`, so treating it as
    // the ordinary `function ... endfunction` block opener leaves the formatter
    // with one stale indent level until `endgroup`.
    //
    // Keep the recognition deliberately structural and narrow:
    //   * the immediate previous code token must be `with`;
    //   * scanning backward within the same semicolon-delimited header must
    //     reach `covergroup`.
    //
    // This avoids suppressing real function declarations elsewhere, and it also
    // avoids broad "inside covergroup" heuristics that could misclassify a
    // future/extension construct in the covergroup body.
    size_t with_idx = prev_code(tokens, function_idx);
    if (with_idx == npos || !kind_is(tokens[with_idx], TK::WithKeyword))
        return false;

    for (size_t n = with_idx; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::Semicolon) ||
            kind_is(tokens[i], TK::EndGroupKeyword) ||
            is_outer_close(tokens[i].lex.kind) ||
            is_close_block(tokens[i].lex.kind))
            return false;
        if (kind_is(tokens[i], TK::CoverGroupKeyword))
            return true;
    }
    return false;
}

inline size_t next_code(const TokenStream& tokens, size_t first, size_t end);

// A `.` that opens a named connection, port expression or pattern variable
// rather than a member select keeps a space after the token before it:
// `, .b(x)`, a modport's `input .a(addr)`, and `tagged Valid .n`.
inline bool dot_keeps_space_after(const TokenStream& tokens, size_t dot) {
    const size_t p = prev_code(tokens, dot);
    if (p == npos)
        return false;
    if (kind_is(tokens[p], TK::Comma) || is_port_direction(tokens[p].lex.kind))
        return true;
    const size_t pp = prev_code(tokens, p);
    return kind_is(tokens[p], TK::Identifier) && pp != npos && kind_is(tokens[pp], TK::TaggedKeyword);
}

// A `function`/`task` that is only a prototype, or a `typedef class`.  The
// qualifiers that make a prototype sit between the keyword and the previous
// item boundary: `extern`, `pure virtual`, `import "DPI-C" context c_name =`,
// `export "DPI-C"`, and a modport's `import`/`export`.  SyntaxPass freezes the
// answer as TopologyFacts::is_prototype.
inline bool is_prototype_at(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size())
        return false;
    const TK k = tokens[idx].lex.kind;
    if (k == TK::ClassKeyword) {
        size_t p = prev_code(tokens, idx);
        if (p != npos && kind_is(tokens[p], TK::InterfaceKeyword))
            p = prev_code(tokens, p);
        return p != npos && kind_is(tokens[p], TK::TypedefKeyword);
    }
    if (k != TK::FunctionKeyword && k != TK::TaskKeyword)
        return false;
    for (size_t p = prev_code(tokens, idx); p != npos; p = prev_code(tokens, p)) {
        switch (tokens[p].lex.kind) {
        case TK::ExternKeyword: case TK::PureKeyword: case TK::ImportKeyword:
        case TK::ExportKeyword:
            return true;
        case TK::VirtualKeyword: case TK::StaticKeyword: case TK::ProtectedKeyword:
        case TK::LocalKeyword: case TK::ContextKeyword: case TK::ForkJoinKeyword:
        case TK::StringLiteral: case TK::Identifier: case TK::Equals:
            continue;
        default:
            return false;
        }
    }
    return false;
}

inline bool opens_design_unit_at(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size() || !is_outer_open(tokens[idx].lex.kind))
        return false;
    const size_t p = prev_code(tokens, idx);
    if (p != npos && kind_is(tokens[p], TK::ExternKeyword))
        return false; // `extern module m (...);` has no body
    if (!kind_is(tokens[idx], TK::InterfaceKeyword))
        return true;
    if (p != npos && kind_is(tokens[p], TK::VirtualKeyword))
        return false;
    // A generic interface port, `module m (interface g, interface.mst h)`:
    // the keyword is the port's type and starts a list item, never a unit.
    if (p != npos && (kind_is(tokens[p], TK::OpenParenthesis) || kind_is(tokens[p], TK::Comma)))
        return false;
    const size_t n = next_code(tokens, idx + 1, tokens.size());
    return !(n != npos && (kind_is(tokens[n], TK::ClassKeyword) || kind_is(tokens[n], TK::Dot)));
}

inline bool opens_indent_scope_at(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size())
        return false;
    if (tokens[idx].immutable.topology.is_prototype || is_property_operator_keyword(tokens[idx]))
        return false;
    if (kind_is(tokens[idx], TK::ForkKeyword))
        return is_fork_block_open(tokens, idx);
    if (is_covergroup_sample_function_header(tokens, idx))
        return false;
    // is_open_block() lists OpenBrace unconditionally, but only a statement
    // block indents its contents -- a concatenation or assignment pattern does
    // not.  SyntaxPass froze which is which.
    if (kind_is(tokens[idx], TK::OpenBrace))
        return tokens[idx].immutable.topology.opens_brace_block;
    // `property`/`sequence` open a block only as a declaration.  After an
    // assertion keyword they introduce an expression, which no
    // `endproperty` ever closes:  `a_x: assert property (@(posedge c) a);`
    if (kind_is(tokens[idx], TK::PropertyKeyword) || kind_is(tokens[idx], TK::SequenceKeyword)) {
        const size_t p = prev_code(tokens, idx);
        const TK pk = p == npos ? TK::Unknown : tokens[p].lex.kind;
        return pk != TK::AssertKeyword && pk != TK::AssumeKeyword && pk != TK::CoverKeyword &&
               pk != TK::RestrictKeyword && pk != TK::ExpectKeyword;
    }
    // A clocking declaration names its event: `[default|global] clocking
    // [cb] @(...)`.  `default clocking cb;` and a modport's `clocking cb`
    // only refer to one and have no `endclocking`.
    if (kind_is(tokens[idx], TK::ClockingKeyword)) {
        size_t n = next_code(tokens, idx + 1, tokens.size());
        if (n != npos && kind_is(tokens[n], TK::Identifier))
            n = next_code(tokens, n + 1, tokens.size());
        return n != npos && kind_is(tokens[n], TK::At);
    }
    return is_open_block(tokens[idx].lex.kind);
}

// The dedent half of opens_indent_scope_at().  A closing brace dedents only
// when its own opening brace indented; pairing them through matching_token is
// what stops an expression brace from dropping a level it never added and
// shifting every following line of the file.
inline bool closes_indent_scope_at(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size() || is_property_operator_keyword(tokens[idx]))
        return false;
    if (kind_is(tokens[idx], TK::CloseBrace)) {
        const size_t open = tokens[idx].immutable.syntax.matching_token;
        return open != npos && kind_is(tokens[open], TK::OpenBrace) &&
               tokens[open].immutable.topology.opens_brace_block;
    }
    return is_close_block(tokens[idx].lex.kind);
}

inline size_t next_code(const TokenStream& tokens, size_t first, size_t end) {
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i)
        if (is_code_token(tokens[i]))
            return i;
    return npos;
}

inline size_t procedural_body_start(const TokenStream& tokens, size_t proc) {
    if (!is_procedural_block_at(tokens, proc))
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
        !is_single_stmt_control(tokens[control]))
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

    // A configured block_begin_like macro is a `begin`: the statement runs to
    // the invocation of its matching block_end_like macro.
    if (kind_is(tokens[body], TK::MacroUsage) && tokens[body].mutable_.macro.opens_indent_scope) {
        int depth = 0;
        for (size_t i = body; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::MacroUsage) || !is_code_token(tokens[i]))
                continue;
            if (tokens[i].mutable_.macro.opens_indent_scope)
                ++depth;
            else if (tokens[i].mutable_.macro.closes_indent_scope && --depth == 0) {
                size_t end = i;
                size_t open = next_code(tokens, i + 1, tokens.size());
                if (open != npos && kind_is(tokens[open], TK::OpenParenthesis) &&
                    tokens[open].immutable.syntax.matching_token != npos)
                    end = tokens[open].immutable.syntax.matching_token;
                return end;
            }
        }
        return npos;
    }

    // `initial randsequence (main) ... endsequence` -- one statement, ending
    // at its own `endsequence` and not at the `;` of its first production.
    if (kind_is(tokens[body], TK::RandSequenceKeyword)) {
        int depth = 0;
        for (size_t i = body; i < tokens.size(); ++i) {
            if (!is_code_token(tokens[i]))
                continue;
            if (kind_is(tokens[i], TK::RandSequenceKeyword))
                ++depth;
            else if (kind_is(tokens[i], TK::EndSequenceKeyword) && --depth == 0)
                return i;
        }
        return npos;
    }

    // `do stmt while (c);` is one statement ending at the `;` after `while`.
    if (kind_is(tokens[body], TK::DoKeyword)) {
        const size_t inner_end = simple_statement_end_from(tokens, next_code(tokens, body + 1, tokens.size()));
        const size_t w = inner_end == npos ? npos : next_code(tokens, inner_end + 1, tokens.size());
        if (w == npos || !kind_is(tokens[w], TK::WhileKeyword))
            return inner_end;
        const size_t open = next_code(tokens, w + 1, tokens.size());
        if (open == npos || !kind_is(tokens[open], TK::OpenParenthesis) ||
            tokens[open].immutable.syntax.matching_token == npos)
            return inner_end;
        const size_t semi = next_code(tokens, tokens[open].immutable.syntax.matching_token + 1, tokens.size());
        return semi != npos && kind_is(tokens[semi], TK::Semicolon) ? semi : inner_end;
    }

    // `unique case`, `priority if`: the qualifier belongs to the statement
    // after it.
    if ((kind_is(tokens[body], TK::UniqueKeyword) || kind_is(tokens[body], TK::Unique0Keyword) ||
         kind_is(tokens[body], TK::PriorityKeyword))) {
        const size_t qualified = next_code(tokens, body + 1, tokens.size());
        if (qualified != npos &&
            (kind_is(tokens[qualified], TK::IfKeyword) || kind_is(tokens[qualified], TK::CaseKeyword) ||
             kind_is(tokens[qualified], TK::CaseXKeyword) || kind_is(tokens[qualified], TK::CaseZKeyword)))
            return simple_statement_end_from(tokens, qualified);
    }

    // `initial case (s) ... endcase` -- a case statement ends at its own
    // `endcase`, not at the `;` of its first item.
    auto opens_case = [&](size_t k) {
        return (kind_is(tokens[k], TK::CaseKeyword) || kind_is(tokens[k], TK::CaseXKeyword) ||
                kind_is(tokens[k], TK::CaseZKeyword) || kind_is(tokens[k], TK::RandCaseKeyword)) &&
               !is_property_operator_keyword(tokens[k]);
    };
    if (opens_case(body)) {
        int depth = 0;
        for (size_t i = body; i < tokens.size(); ++i) {
            if (!is_code_token(tokens[i]))
                continue;
            if (opens_case(i))
                ++depth;
            else if (kind_is(tokens[i], TK::EndCaseKeyword) && !is_property_operator_keyword(tokens[i]) &&
                     --depth == 0)
                return i;
        }
        return npos;
    }

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

    if (is_single_stmt_control(tokens[body])) {
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
        // A semicolonless macro statement ends where its `;` would be.
        if (pd == 0 && bd == 0 && brd == 0 &&
            (kind_is(tokens[i], TK::Semicolon) || tokens[i].mutable_.macro.ends_statement))
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

// -----------------------------------------------------------------------------
// Semicolonless macro statements
// -----------------------------------------------------------------------------
// UVM and OpenTitan invoke statement and item macros with no `;`:
//
//   `uvm_info(`gfn, "msg", UVM_LOW)
//   `ASSERT(CntNoOverflow_A, cnt_q != '1)
//   always_comb ...
//
// Nothing in the token stream says the macro ends a statement, so every
// question "where does this statement end" used to fall through to the next
// `;` -- which belongs to a different statement.  These helpers answer it once,
// from TokenKinds, for SyntaxPass to freeze as
// TopologyFacts::may_end_macro_statement.

// A token that can only carry on the operand in front of it.  A macro followed
// by one of these is part of an expression -- a case label (`` `OP: ``), an
// assignment target (`` `REG = 1; ``), a member select (`` `FIELD(1).f ``).
inline bool continues_operand(TK k) {
    return k == TK::Semicolon || k == TK::Colon || k == TK::Comma || k == TK::Question ||
           is_binary_op(k) || is_assignment_op(k) ||
           k == TK::TripleEquals || k == TK::ExclamationDoubleEquals ||
           k == TK::DoubleEqualsQuestion || k == TK::ExclamationEqualsQuestion ||
           k == TK::Dot || k == TK::OpenBracket || k == TK::DoubleColon ||
           k == TK::Apostrophe || k == TK::PlusColon || k == TK::MinusColon ||
           k == TK::DoublePlus || k == TK::DoubleMinus ||
           k == TK::CloseParenthesis || k == TK::CloseBracket || k == TK::CloseBrace ||
           k == TK::Hash || k == TK::At || k == TK::OpenParenthesis;
}

// Keywords that begin a statement, a module item or a class item and can never
// continue an expression.
inline bool starts_new_statement_keyword(TK k) {
    if (is_close_block(k) && k != TK::CloseBrace)
        return true;
    if (is_outer_close(k) || is_outer_open(k) || is_procedural_block_keyword(k))
        return true;
    switch (k) {
    case TK::AssignKeyword: case TK::DeassignKeyword: case TK::ForceKeyword:
    case TK::ReleaseKeyword: case TK::IfKeyword: case TK::ElseKeyword:
    case TK::CaseKeyword: case TK::CaseXKeyword: case TK::CaseZKeyword:
    case TK::RandCaseKeyword: case TK::RandSequenceKeyword: case TK::ForKeyword:
    case TK::ForeachKeyword: case TK::WhileKeyword: case TK::DoKeyword:
    case TK::RepeatKeyword: case TK::ForeverKeyword: case TK::ForkKeyword:
    case TK::BeginKeyword: case TK::ReturnKeyword: case TK::BreakKeyword:
    case TK::ContinueKeyword: case TK::WaitKeyword: case TK::DisableKeyword:
    case TK::UniqueKeyword: case TK::Unique0Keyword: case TK::PriorityKeyword:
    case TK::AssertKeyword: case TK::AssumeKeyword: case TK::CoverKeyword:
    case TK::RestrictKeyword: case TK::ExpectKeyword: case TK::GenerateKeyword:
    case TK::GenVarKeyword: case TK::FunctionKeyword: case TK::TaskKeyword:
    case TK::TypedefKeyword: case TK::ImportKeyword: case TK::ExportKeyword:
    case TK::ParameterKeyword: case TK::LocalParamKeyword: case TK::DefParamKeyword:
    case TK::PropertyKeyword: case TK::SequenceKeyword: case TK::ClassKeyword:
    case TK::VirtualKeyword: case TK::ModPortKeyword: case TK::ClockingKeyword:
    case TK::DefaultKeyword: case TK::CoverGroupKeyword: case TK::ConstraintKeyword:
    case TK::BindKeyword: case TK::LetKeyword: case TK::GlobalKeyword:
        return true;
    default:
        return false;
    }
}

// Keywords that begin a data or net declaration.  Unlike the list above these
// also follow a prefix macro (`` `MY_ATTR logic a; ``), so they end a macro
// statement only after an invocation with arguments.
inline bool starts_declaration_keyword(TK k) {
    return is_var_decl_leading_keyword(k) || is_port_direction(k) ||
           k == TK::VarKeyword || k == TK::StructKeyword || k == TK::EnumKeyword ||
           k == TK::UnionKeyword ||
           k == TK::TriKeyword || k == TK::UWireKeyword || k == TK::NetTypeKeyword;
}

// Drive and charge strength keywords: `(strong0, weak1)`, `trireg (small)`.
inline bool is_strength_keyword(TK k) {
    switch (k) {
        case TK::Supply0Keyword: case TK::Supply1Keyword:
        case TK::Strong0Keyword: case TK::Strong1Keyword:
        case TK::Pull0Keyword: case TK::Pull1Keyword:
        case TK::Weak0Keyword: case TK::Weak1Keyword:
        case TK::HighZ0Keyword: case TK::HighZ1Keyword:
        case TK::SmallKeyword: case TK::MediumKeyword: case TK::LargeKeyword:
            return true;
        default:
            return false;
    }
}

// `)` closing a drive or charge strength -- `wire (pull1, pull0) [3:0] w`.
// What follows is a declaration's packed dimension, not a select.
inline bool closes_strength(const TokenStream& tokens, size_t close) {
    if (!kind_is(tokens[close], TK::CloseParenthesis)) return false;
    const size_t open = tokens[close].immutable.syntax.matching_token;
    if (open == npos) return false;
    const size_t first = next_code(tokens, open + 1, close);
    return first != npos && is_strength_keyword(tokens[first].lex.kind);
}

// The invocation's last token: the `)` closing its arguments, or the macro.
inline size_t macro_invocation_end(const TokenStream& tokens, size_t macro) {
    size_t open = next_code(tokens, macro + 1, tokens.size());
    if (open != npos && kind_is(tokens[open], TK::OpenParenthesis) &&
        tokens[open].immutable.syntax.matching_token != npos)
        return tokens[open].immutable.syntax.matching_token;
    return macro;
}

// First code token after `idx` and after any `[...]` dimensions that follow.
inline size_t next_code_past_dimensions(const TokenStream& tokens, size_t idx) {
    size_t n = next_code(tokens, idx + 1, tokens.size());
    while (n != npos && kind_is(tokens[n], TK::OpenBracket) &&
           tokens[n].immutable.syntax.matching_token != npos)
        n = next_code(tokens, tokens[n].immutable.syntax.matching_token + 1, tokens.size());
    return n;
}

// Given a macro that sits where a statement or item can start, decide whether
// its invocation is the whole statement: the token after it must begin a new
// one.  Returns the invocation's last token, or npos when it is not.
//
// `in_case_item` is true when the macro is the statement of a case item.  Its
// successor there can be the next item's label -- any expression -- so only a
// token that continues the operand keeps them together.
inline size_t macro_statement_end(const TokenStream& tokens, size_t macro, bool in_case_item) {
    const size_t end = macro_invocation_end(tokens, macro);
    const bool has_args = end != macro;
    const size_t next = next_code(tokens, end + 1, tokens.size());
    if (next == npos)
        return end;
    const Tok& n = tokens[next];
    const TK k = n.lex.kind;
    if (continues_operand(k))
        return npos;
    if (in_case_item)
        return end;
    if (k == TK::MacroUsage) {
        // `` `T_DATA `CAT(d, 2); `` declares a name spelled by a macro; the
        // first macro is its type.  `` `CHECK_A `CHECK_B(x) `` are two
        // statements.
        if (!has_args) {
            const size_t after = next_code_past_dimensions(tokens, macro_invocation_end(tokens, next));
            if (after != npos && (kind_is(tokens[after], TK::Semicolon) ||
                                  kind_is(tokens[after], TK::Comma) ||
                                  kind_is(tokens[after], TK::Equals)))
                return npos;
        }
        return end;
    }
    if (starts_new_statement_keyword(k) || k == TK::SystemIdentifier)
        return end;
    if (starts_declaration_keyword(k))
        return has_args ? end : npos;
    if (k == TK::Identifier) {
        const size_t after = next_code_past_dimensions(tokens, next);
        const TK a = after == npos ? TK::Unknown : tokens[after].lex.kind;
        if (has_args) {
            // `` `MY_T(8) sig; `` declares `sig`; `` `uvm_info(...) foo = 1; ``
            // and `` `ASSERT(...) prim_x u_x (...); `` are new statements.
            return (a == TK::Semicolon || a == TK::Comma) ? npos : end;
        }
        // A bare macro before a name is a type or prefix (`` `T_DATA d0; ``,
        // `` `MOD u (...) ``) unless that name is plainly assigned or stepped.
        const bool assigned = is_assignment_op(a) || a == TK::DoublePlus || a == TK::DoubleMinus;
        return assigned ? end : npos;
    }
    return npos;
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

// Width of [first, end) as the renderer will print it on one line: token text
// plus the gaps SpacingPass decided.  AlignPass runs after SpacingPass, so it
// measures what will actually be emitted.  It used to measure with a private
// copy of the spacing rules, which knew nothing of case-item colons or based
// literals (`4'h12: yy` measured 10 wide and rendered 9) and so misplaced the
// columns it was computing.
inline int rendered_width(const TokenStream& tokens, size_t first, size_t end) {
    int w = 0;
    bool first_code = true;
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i) {
        const Tok& t = tokens[i];
        if (!is_code_token(t)) continue;
        if (!first_code && !t.mutable_.space.suppress_space)
            w += t.mutable_.space.spaces_before;
        w += token_width(t);
        first_code = false;
    }
    return w;
}

// A format-on marker, a format-off marker and the frozen body between them
// are rendered verbatim and each ends its line.  They carry no wrap flags, so
// a walk back to "the start of this line" has to stop at them explicitly, or
// the first statement after a disabled region is measured -- and indented --
// from inside that region.
inline bool ends_verbatim_line(const Tok& t) {
    return t.lex.is_format_on_marker || t.lex.is_format_off_marker || t.lex.is_disabled_region_body;
}

// The width of the line in front of `token_idx`, indentation included.
// WrapPass runs before IndentPass, so the indent is taken from the scope depth
// SyntaxPass recorded for the line's first token rather than from
// IndentMetadata; a limit compared against the text alone let a line run past
// it by exactly its own indent.
inline int line_prefix_width(const TokenStream& tokens, size_t token_idx, const FormatOptions& opts) {
    size_t line_start = 0;
    for (size_t i = token_idx; i > 0; --i) {
        if (tokens[i].mutable_.wrap.must_break_before || tokens[i - 1].mutable_.wrap.must_break_after ||
            ends_verbatim_line(tokens[i - 1])) {
            line_start = i;
            break;
        }
    }
    int levels = 0;
    if (line_start < tokens.size()) {
        const auto& topo = tokens[line_start].immutable.topology;
        levels = topo.scope_depth;
        if (topo.in_outermost_unit && opts.default_indent_level_inside_outmost_block > 0)
            ++levels;
    }
    return levels * opts.indent_size + compact_width(tokens, line_start, token_idx);
}

struct ListItem {
    size_t first{npos};
    size_t last{npos};   // inclusive
    size_t comma{npos};
};

inline bool is_conditional_preprocessor_directive(const Tok& t);

inline std::vector<ListItem> top_level_list_items(const TokenStream& tokens, size_t first, size_t close) {
    std::vector<ListItem> out;
    size_t start = next_code(tokens, first, close);
    size_t last = npos;
    int pd = 0, bd = 0, brd = 0;
    for (size_t i = first; i < close && i < tokens.size(); ++i) {
        // `` `ifdef WIDE output [63:0] q `else output [31:0] q `endif `` --
        // the branches have no comma between them, but a conditional
        // directive at the list's own depth still separates two items.
        // Without this the second branch was part of the first item, so its
        // `` `else `` took the item indent and the two alignments disagreed.
        if (pd == 0 && bd == 0 && brd == 0 && is_conditional_preprocessor_directive(tokens[i])) {
            if (start != npos && last != npos)
                out.push_back({start, last, npos});
            start = next_code(tokens, i + 1, close);
            last = npos;
            continue;
        }
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
    for (size_t n = open; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::CloseParenthesis)) ++pd;
        else if (kind_is(tokens[i], TK::OpenParenthesis) && pd > 0) --pd;
        // `virtual interface bus_if #(16) vif;` -- the keyword is part of a
        // type, and the `#(` after it specializes rather than declares.
        if (pd == 0 && kind_is(tokens[i], TK::InterfaceKeyword)) {
            const size_t p = prev_code(tokens, i);
            if (p != npos && kind_is(tokens[p], TK::VirtualKeyword))
                break;
        }
        if (pd == 0 && starts_module_like_header(tokens[i].lex.kind))
            return i;
        // Only a header import's `;` (`module m import p::*; (...)`) sits
        // inside a header.  Any other one ends an item, and a `(` after it --
        // a specify path `(clk => q) = 1;` after `specparam ...;` -- is not
        // the header's port list.
        if (pd == 0 && kind_is(tokens[i], TK::Semicolon) &&
            !is_module_header_import_semicolon(tokens, i))
            break;
        if (pd == 0 && (is_outer_close(tokens[i].lex.kind) || is_close_block(tokens[i].lex.kind)))
            break;
        // A header holds no block: a `(` inside `specify`, `begin`, ...
        // belongs to that block.
        if (pd == 0 && is_open_block(tokens[i].lex.kind) && !kind_is(tokens[i], TK::OpenBrace))
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
    //
    // `function type(x) f (int a);` -- a `type(...)` return type has
    // parentheses of its own.  Its `(` is not the header's, and its balanced
    // pair is stepped over on the way back from the one that is.
    const size_t before_open = prev_code(tokens, open);
    if (before_open != npos && kind_is(tokens[before_open], TK::TypeKeyword))
        return false;
    for (size_t n = open; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::CloseParenthesis)) {
            const size_t match = tokens[i].immutable.syntax.matching_token;
            if (match != npos && match < i) {
                n = match + 1;
                continue;
            }
        }
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

// The `)` closing a control header or timing control -- `if (c)`,
// `foreach (m[i])`, `@(posedge clk)`, `#(D)` -- ends what comes before the
// statement it controls.
inline bool closes_control_header(const TokenStream& tokens, size_t close) {
    if (close >= tokens.size() || !kind_is(tokens[close], TK::CloseParenthesis))
        return false;
    const size_t open = tokens[close].immutable.syntax.matching_token;
    const size_t before = open == npos ? npos : prev_code(tokens, open);
    if (before == npos)
        return false;
    if (tokens[before].immutable.topology.is_intra_assignment_repeat)
        return false;
    const TK k = tokens[before].lex.kind;
    return is_single_stmt_control(k) || is_control_keyword(k) || k == TK::WaitKeyword ||
           k == TK::At || k == TK::Hash || k == TK::CaseKeyword || k == TK::CaseXKeyword ||
           k == TK::CaseZKeyword;
}

// A `<=` is a nonblocking assignment only as a statement's first assignment
// operator, outside every delimiter.  Inside one (`if (a <= b)`,
// `{a <= b}`), or after another (`y = a <= b;`, `assign z = a <= b;`,
// `q <= a <= b;`), it compares.
inline bool is_relational_less_equal(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size() || !kind_is(tokens[idx], TK::LessThanEquals))
        return false;
    const auto& sx = tokens[idx].immutable.syntax;
    if (sx.paren_depth > 0 || sx.bracket_depth > 0 || sx.brace_depth > 0)
        return true;
    for (size_t i = prev_code(tokens, idx); i != npos; i = prev_code(tokens, i)) {
        const Tok& t = tokens[i];
        if (kind_is(t, TK::Semicolon) || t.mutable_.macro.ends_statement ||
            closes_control_header(tokens, i) || is_close_block(t.lex.kind) ||
            kind_is(t, TK::BeginKeyword) || t.immutable.topology.is_case_item_colon)
            return false;
        const auto& ts = t.immutable.syntax;
        if (ts.paren_depth == 0 && ts.bracket_depth == 0 && ts.brace_depth == 0 &&
            is_assignment_op(t.lex.kind))
            return true;
    }
    return false;
}

// An operator that joins or negates operands.  A declaration's prefix holds
// only keywords, names, `::`, `#(...)` and dimensions, so one of these at the
// statement's own depth makes it an expression: `a |-> b[0];`,
// `!gnt ##1 d[0];`, `data[0] < data[1];`.
inline bool is_expression_operator(TK k) {
    return is_binary_op(k) || k == TK::OrMinusArrow || k == TK::OrEqualsArrow ||
           k == TK::DoubleHash || k == TK::MinusArrow || k == TK::Question ||
           k == TK::Exclamation || k == TK::Tilde;
}

// `my_if.mp name` -- exactly a name, a `.`, a name and the declarator,
// from `first` up to (not including) `last`.
inline bool is_modport_typed_declarator(const TokenStream& tokens, size_t first, size_t last) {
    size_t at = first;
    const TK shape[] = {TK::Identifier, TK::Dot, TK::Identifier, TK::Identifier};
    for (TK want : shape) {
        if (at == npos || at >= last || !kind_is(tokens[at], want))
            return false;
        at = next_code(tokens, at + 1, tokens.size());
    }
    return at != npos && at >= last;
}

inline bool is_var_declaration_trailing_dimension_open(const TokenStream& tokens, size_t open) {
    if (open >= tokens.size() || !kind_is(tokens[open], TK::OpenBracket))
        return false;
    if (!is_identifier_like(tokens[open == 0 ? open : open - 1]))
        return false;
    if (tokens[open].immutable.syntax.in_covergroup)
        return false;
    // `a ##1 d [=3];` -- an SVA repetition, not a declarator's dimension.
    if (tokens[open].immutable.topology.is_repetition_bracket)
        return false;
    size_t close = tokens[open].immutable.syntax.matching_token;
    if (close == npos)
        return false;
    // `logic b [2][3];` -- the declarator's later dimensions come first.
    size_t after = next_code_past_dimensions(tokens, close);
    if (after == npos)
        return false;
    // The last ANSI port or function argument is followed by the list's `)`.
    const bool ends_port_list =
        kind_is(tokens[after], TK::CloseParenthesis) &&
        tokens[after].immutable.syntax.matching_token != npos &&
        (tokens[tokens[after].immutable.syntax.matching_token].immutable.topology.starts_port_list ||
         is_function_task_declaration_open(tokens, tokens[after].immutable.syntax.matching_token));
    if (!(kind_is(tokens[after], TK::Semicolon) || kind_is(tokens[after], TK::Comma) ||
          is_assignment_op(tokens[after].lex.kind) || ends_port_list))
        return false;

    // Where the bracket's own list element starts: back to the `;` or `,` at
    // its depth, or to the `(`/`{` enclosing it.  SyntaxFacts::stmt_begin
    // splits at every comma whatever its depth, so for the `a[0]` in
    // `wire [7:0] w = {4{a[0], b[0]}};` it named `wire` and the bracket read
    // as a declaration's trailing dimension (`{4{a [0], ...`).  A
    // declaration never nests inside an expression's braces or parentheses,
    // so starting from the element keeps `{mem[g], ...}` and
    // `` `CHECK(m[k], 0) `` indexes while `module m(input logic a [4], ...`
    // still starts at `input`.
    //
    // Nor does a declarator follow a control header (`foreach (m[i]) m[i] =
    // 0;`, `@(posedge clk) m[0] <= x;`) or a colon at its depth (a case
    // item, a statement label, an assignment-pattern key `'{hi: a[3:0]}`),
    // so the walk stops there too.  With `stop_at_comma` false it runs on to
    // the start of the whole declaration, for `int a[], b[$];`.
    const int pd = tokens[open].immutable.syntax.paren_depth;
    const int bd = tokens[open].immutable.syntax.bracket_depth;
    const int brd = tokens[open].immutable.syntax.brace_depth;
    auto element_start = [&](bool stop_at_comma) {
        size_t begin = npos;
        for (size_t n = open; n > 0; --n) {
            const size_t i = n - 1;
            if (!is_code_token(tokens[i]))
                continue;
            const auto& sx = tokens[i].immutable.syntax;
            const bool enclosing = sx.paren_depth < pd || sx.brace_depth < brd;
            const bool here = sx.paren_depth == pd && sx.brace_depth == brd;
            const bool separator = here && (kind_is(tokens[i], TK::Semicolon) ||
                                            (stop_at_comma && kind_is(tokens[i], TK::Comma)));
            const bool colon = here && sx.bracket_depth == bd && kind_is(tokens[i], TK::Colon);
            // A semicolonless macro statement (`` `INIT_PROLOG ``) ends the
            // statement before, as a `;` would.
            // `mailbox #(int) mb [2];` -- a `#(...)` after a type name
            // overrides its parameters; it is not a delay control.
            auto closes_parameter_override = [&]() {
                if (!kind_is(tokens[i], TK::CloseParenthesis))
                    return false;
                const size_t po = tokens[i].immutable.syntax.matching_token;
                const size_t hash = po == npos ? npos : prev_code(tokens, po);
                const size_t type = hash == npos || !kind_is(tokens[hash], TK::Hash)
                    ? npos : prev_code(tokens, hash);
                return type != npos && kind_is(tokens[type], TK::Identifier);
            };
            if (enclosing || separator || colon ||
                (closes_control_header(tokens, i) && !closes_parameter_override()) ||
                tokens[i].mutable_.macro.ends_statement)
                break;
            // `endfunction` / `int q[$];` and `begin` / `int q[$];` -- a
            // block's closer or opener ends the statement before it without
            // a `;`.  A `}` does not: `struct { ... } s [2];` is one element.
            const TK ik = tokens[i].lex.kind;
            if ((is_close_block(ik) && ik != TK::CloseBrace) || is_outer_close(ik) ||
                ik == TK::BeginKeyword || ik == TK::ForkKeyword || ik == TK::GenerateKeyword)
                break;
            // `begin : gen` -- the block's name belongs to its opener, not to
            // the first item after it (`begin : gen assign wl[g] = 1;`).
            const size_t before = prev_code(tokens, i);
            if (before != npos && tokens[before].immutable.topology.is_block_name_colon)
                break;
            begin = i;
        }
        // `(* ram_style = "block" *) reg [7:0] mem [256];` -- an attribute
        // annotates the element; the declaration starts after it.
        while (begin != npos && begin < open && tokens[begin].lex.in_attribute_instance)
            begin = next_code(tokens, begin + 1, open);
        return begin;
    };

    // `logic a [4]`, `input logic [7:0] d [2]`, `my_t m [4]`, or
    // `localparam logic [7:0] LUT [4]` -- a parameter's unpacked dimension
    // is spaced like a variable's.
    auto declares = [&](size_t first, size_t last) {
        if (is_var_decl_leading_keyword(tokens[first].lex.kind) ||
            is_port_direction(tokens[first].lex.kind) ||
            kind_is(tokens[first], TK::ParameterKeyword) ||
            kind_is(tokens[first], TK::LocalParamKeyword) ||
            kind_is(tokens[first], TK::TypedefKeyword) ||
            // `tri t [2];`, `var v [2];`, `local int x [2];`, and
            // `struct { ... } s [4];` -- declarations all, whatever leads.
            is_net_type_keyword(tokens[first].lex.kind) ||
            kind_is(tokens[first], TK::VarKeyword) ||
            kind_is(tokens[first], TK::InterconnectKeyword) ||
            kind_is(tokens[first], TK::LocalKeyword) ||
            kind_is(tokens[first], TK::ProtectedKeyword) ||
            kind_is(tokens[first], TK::StructKeyword) ||
            kind_is(tokens[first], TK::UnionKeyword) ||
            kind_is(tokens[first], TK::EnumKeyword))
            return true;
        // `my_if.mp ifa [2];` -- an interface modport names the type.  The
        // declarator follows it directly, which no member select does.
        if (is_modport_typed_declarator(tokens, first, last))
            return true;
        // User-defined types can lead a declaration with an identifier-like
        // token.  Accept the pattern only when the element contains at least
        // two identifier-like tokens before the dimension and no
        // member-access dot, which keeps array indexing expressions such as
        // `foo.bar[3:0]` from being misclassified as declarations.
        int identifier_count = 0;
        for (size_t i = first; i < last; ++i) {
            if (!is_code_token(tokens[i]))
                continue;
            if (kind_is(tokens[i], TK::Dot))
                return false;
            // `a |-> b[0];`, `!gnt ##1 d[0];`, `x && y[1];` -- two names
            // joined by an operator are an expression.  A declaration's
            // prefix holds only keywords, names, `::`, `#(...)` and
            // dimensions.
            const auto& sx = tokens[i].immutable.syntax;
            const bool own_depth =
                sx.paren_depth == pd && sx.bracket_depth == bd && sx.brace_depth == brd;
            if (own_depth && is_expression_operator(tokens[i].lex.kind))
                return false;
            // `pkg::arr[i]` names one thing; a scoped name counts once.  So
            // does a selected one: in `f(w[i][j], a[i])` the first argument
            // holds one name of its own, and counting `i` and `j` with it
            // made `a[i]` a later declarator of a declaration `w ... j`.
            const size_t before = prev_code(tokens, i);
            if (own_depth && is_identifier_like(tokens[i]) &&
                !(before != npos && before >= first && kind_is(tokens[before], TK::DoubleColon)))
                ++identifier_count;
        }
        return identifier_count >= 2;
    };

    const size_t elem = element_start(true);
    if (elem == npos || elem >= open)
        return false;

    // Declarations place trailing unpacked dimensions before any initializer,
    // so if the element already contains an assignment operator before the
    // candidate bracket, this is an expression such as `assign y = arr[3:0];`
    // however the statement began.
    for (size_t i = elem; i < open; ++i) {
        if (is_assignment_op(tokens[i].lex.kind))
            return false;
    }
    // `` `MACRO m[0] = 1; `` -- a macro followed by an assigned select.  A
    // macro-named type declares with `;` after its dimensions
    // (`` `WORD_T m [4]; ``); one whose element is assigned is more often a
    // statement macro in front of an assignment, which no TokenKind can
    // tell apart, and reading it as a declaration spaces the select.
    if (kind_is(tokens[elem], TK::MacroUsage) && is_assignment_op(tokens[after].lex.kind))
        return false;
    if (declares(elem, open))
        return true;

    // A later declarator of a multi-name declaration is only a name
    // (`int da[], q[$];`); it is a declarator when the declaration it
    // continues is one.
    const size_t first = element_start(false);
    if (elem != open - 1 || first == npos || first >= elem)
        return false;
    size_t first_end = first;
    while (first_end < elem && !(kind_is(tokens[first_end], TK::Comma) &&
                                 tokens[first_end].immutable.syntax.paren_depth == pd &&
                                 tokens[first_end].immutable.syntax.brace_depth == brd))
        ++first_end;
    return declares(first, first_end);
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

// Can a module item (an instantiation) start right after `prev`?
inline bool follows_module_item_boundary(const TokenStream& tokens, size_t prev) {
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
               kind_is(tokens[prev], TK::EndFunctionKeyword) ||
               // The same holds for every other item that closes with a
               // keyword and no `;`: an instance right after `endclocking`
               // was laid out as a call.
               kind_is(tokens[prev], TK::EndClockingKeyword) ||
               kind_is(tokens[prev], TK::EndGroupKeyword) ||
               kind_is(tokens[prev], TK::EndPropertyKeyword) ||
               kind_is(tokens[prev], TK::EndSequenceKeyword) ||
               kind_is(tokens[prev], TK::EndCheckerKeyword) ||
               kind_is(tokens[prev], TK::EndSpecifyKeyword) ||
               kind_is(tokens[prev], TK::EndClassKeyword);
    };

    auto follows_named_generate_boundary = [&](size_t prev) {
        if (prev == npos)
            return false;

        // `begin : gen_a`, `end : gen_a` -- the name ends its keyword.
        if (const size_t colon = prev_code(tokens, prev);
            colon != npos && tokens[colon].immutable.topology.is_block_name_colon)
            return true;

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
        if (tokens[prev].mutable_.macro.ends_statement)
            return true;

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

    // A generate item is also the body of a generate `if`/`for` without
    // `begin` (`if (N > 2) sub u (...);`), the branch after `else`, or a
    // case-generate item.  In a procedural context none of those can be
    // followed by `type name (`, so the reading is unambiguous.
    auto follows_generate_control = [&](size_t prev) {
        if (prev == npos)
            return false;
        if (kind_is(tokens[prev], TK::ElseKeyword) || tokens[prev].immutable.topology.is_case_item_colon)
            return true;
        if (!kind_is(tokens[prev], TK::CloseParenthesis) || tokens[prev].immutable.syntax.matching_token == npos)
            return false;
        const size_t control = prev_code(tokens, tokens[prev].immutable.syntax.matching_token);
        return control != npos &&
               (kind_is(tokens[control], TK::IfKeyword) || kind_is(tokens[control], TK::ForKeyword));
    };

    auto follows_sv_item_boundary = [&](size_t prev) {
        // `(* keep *) sub u (...);` -- an attribute belongs to the item after it.
        while (prev != npos && tokens[prev].lex.in_attribute_instance)
            prev = prev_code(tokens, prev);
        if (is_basic_sv_item_boundary(prev))
            return true;
        if (follows_generate_control(prev))
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
    return follows_sv_item_boundary(prev);
}

inline bool is_gate_keyword(TK k) {
    switch (k) {
    case TK::AndKeyword: case TK::NandKeyword: case TK::OrKeyword: case TK::NorKeyword:
    case TK::XorKeyword: case TK::XnorKeyword: case TK::BufKeyword: case TK::NotKeyword:
    case TK::BufIf0Keyword: case TK::BufIf1Keyword: case TK::NotIf0Keyword: case TK::NotIf1Keyword:
    case TK::NmosKeyword: case TK::PmosKeyword: case TK::RnmosKeyword: case TK::RpmosKeyword:
    case TK::CmosKeyword: case TK::RcmosKeyword: case TK::TranKeyword: case TK::RtranKeyword:
    case TK::TranIf0Keyword: case TK::TranIf1Keyword: case TK::RtranIf0Keyword: case TK::RtranIf1Keyword:
    case TK::PullUpKeyword: case TK::PullDownKeyword:
        return true;
    default:
        return false;
    }
}

// The instance name before a port list, `u_x` or `u_x[3:0]`; npos when the
// token before `open` is not one.
inline size_t instance_name_before(const TokenStream& tokens, size_t open) {
    size_t inst = prev_code(tokens, open);
    if (inst != npos && kind_is(tokens[inst], TK::CloseBracket)) {
        size_t br = tokens[inst].immutable.syntax.matching_token;
        inst = br == npos ? npos : prev_code(tokens, br);
    }
    return inst != npos && kind_is(tokens[inst], TK::Identifier) ? inst : npos;
}

// `and #1 g1 (o1, i1, i2), g2 (o2, i3, i4);` and
// `bufif0 (strong0, weak1) #(1:2:3) b1 (o, i, en);` -- the terminal list of a
// named gate-primitive instance.  The gate keyword must stand where a module
// item starts, so a sequence instance in `a and s1(x)` is not one.
inline bool is_gate_terminal_open(const TokenStream& tokens, size_t open) {
    if (open >= tokens.size() || !kind_is(tokens[open], TK::OpenParenthesis))
        return false;
    const size_t inst = instance_name_before(tokens, open);
    if (inst == npos)
        return false;
    size_t k = prev_code(tokens, inst);
    // A later instance of the same gate statement.
    if (k != npos && kind_is(tokens[k], TK::Comma)) {
        const size_t close = prev_code(tokens, k);
        return close != npos && kind_is(tokens[close], TK::CloseParenthesis) &&
               tokens[close].immutable.syntax.matching_token != npos &&
               is_gate_terminal_open(tokens, tokens[close].immutable.syntax.matching_token);
    }
    // Delay: `#1`, `#d`, `#(1:2:3)`.
    if (k != npos && kind_is(tokens[k], TK::CloseParenthesis) &&
        tokens[k].immutable.syntax.matching_token != npos) {
        const size_t hash = prev_code(tokens, tokens[k].immutable.syntax.matching_token);
        if (hash != npos && kind_is(tokens[hash], TK::Hash))
            k = prev_code(tokens, hash);
    } else if (k != npos) {
        const size_t hash = prev_code(tokens, k);
        if (hash != npos && kind_is(tokens[hash], TK::Hash))
            k = prev_code(tokens, hash);
    }
    // Drive or pull strength: `(strong0, weak1)`.
    if (k != npos && kind_is(tokens[k], TK::CloseParenthesis) &&
        tokens[k].immutable.syntax.matching_token != npos) {
        const size_t kw = prev_code(tokens, tokens[k].immutable.syntax.matching_token);
        if (kw != npos && is_gate_keyword(tokens[kw].lex.kind))
            k = kw;
    }
    // `property p; not s1(a); endproperty` -- there `not` is a property
    // operator and `s1(a)` a sequence instance.
    return k != npos && is_gate_keyword(tokens[k].lex.kind) &&
           !tokens[k].immutable.syntax.in_property_expr &&
           follows_module_item_boundary(tokens, prev_code(tokens, k));
}

inline bool is_instance_port_open(const TokenStream& tokens, size_t open) {
    size_t inst = prev_code(tokens, open);
    if (inst != npos && kind_is(tokens[inst], TK::CloseBracket)) {
        size_t br = tokens[inst].immutable.syntax.matching_token;
        if (br != npos)
            inst = prev_code(tokens, br);
    }
    // An instance name is a plain identifier.  `$display(` is a system task
    // call, and the `(` after a macro (`` `T_DATA `CAT(d, 2); ``) is that
    // macro's own argument list -- neither is a port list, whatever precedes.
    if (inst == npos || !kind_is(tokens[inst], TK::Identifier)) return false;
    size_t mod = prev_code(tokens, inst);
    // `sub (* keep *) u (...)` -- an attribute on the instance name.
    while (mod != npos && tokens[mod].lex.in_attribute_instance)
        mod = prev_code(tokens, mod);
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
    // `sub u1 (...), u2 (...);` -- a later instance of the same statement.
    if (mod != npos && kind_is(tokens[mod], TK::Comma) && tokens[open].immutable.syntax.paren_depth == 0) {
        const size_t close = prev_code(tokens, mod);
        return close != npos && kind_is(tokens[close], TK::CloseParenthesis) &&
               tokens[close].immutable.syntax.matching_token != npos &&
               is_instance_port_open(tokens, tokens[close].immutable.syntax.matching_token);
    }
    if (mod == npos || !is_identifier_like(tokens[mod])) return false;
    size_t prev = prev_code(tokens, mod);
    if (follows_module_item_boundary(tokens, prev))
        return true;
    // `bind fifo chk u_chk (...)`, `bind top.u_a: u_b chk u_chk (...)` --
    // the bind target runs from `bind` to the instantiated module.
    for (size_t n = mod; n > 0; --n) {
        const size_t i = n - 1;
        if (!is_code_token(tokens[i]))
            continue;
        if (kind_is(tokens[i], TK::BindKeyword))
            return follows_module_item_boundary(tokens, prev_code(tokens, i));
        if (!(is_identifier_like(tokens[i]) || kind_is(tokens[i], TK::Dot) ||
              kind_is(tokens[i], TK::Colon) || kind_is(tokens[i], TK::Comma) ||
              kind_is(tokens[i], TK::OpenBracket) || kind_is(tokens[i], TK::CloseBracket) ||
              kind_is(tokens[i], TK::IntegerLiteral)))
            return false;
    }
    return false;
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
        // Paren/bracket depth at each open brace, so a `;` nested inside
        // parentheses is not mistaken for the brace's own statement separator.
        std::vector<std::pair<int, int>> brace_ctx;
        int pd = 0, bd = 0, brd = 0;
        bool in_function_decl = false;
        bool in_task_decl = false;
        bool in_class_decl = false;
        bool in_covergroup = false;
        bool in_modport = false;
        bool in_clocking_block = false;
        // Read by opens_indent_scope_at() in the walk below.
        for (size_t i = 0; i < tokens.size(); ++i) {
            tokens[i].immutable.topology.is_prototype = is_prototype_at(tokens, i);
            tokens[i].immutable.topology.opens_design_unit = opens_design_unit_at(tokens, i);
        }
        // Brace depth at each open `randsequence`, innermost last: a
        // production's `{ ... }` code block holds statements again.
        std::vector<int> production_brace_depths;
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (kind_is(t, TK::EndClockingKeyword))
                in_clocking_block = false;
            t.immutable.syntax.paren_depth = pd;
            t.immutable.syntax.bracket_depth = bd;
            t.immutable.syntax.brace_depth = brd;
            t.immutable.syntax.in_function_decl = in_function_decl;
            t.immutable.syntax.in_task_decl = in_task_decl;
            t.immutable.syntax.in_class_decl = in_class_decl;
            t.immutable.syntax.in_covergroup = in_covergroup;
            t.immutable.syntax.in_modport = in_modport;
            t.immutable.syntax.in_clocking_block = in_clocking_block;
            if (kind_is(t, TK::EndSequenceKeyword) && !production_brace_depths.empty())
                production_brace_depths.pop_back();
            t.immutable.syntax.in_production =
                !production_brace_depths.empty() && brd == production_brace_depths.back();
            if (kind_is(t, TK::RandSequenceKeyword))
                production_brace_depths.push_back(brd);
            t.immutable.topology.opens_indent_scope = opens_indent_scope_at(tokens, i) || t.immutable.topology.opens_design_unit;
            t.immutable.topology.closes_indent_scope = is_close_block(t.lex.kind) || is_outer_close(t.lex.kind);

            if (kind_is(t, TK::OpenParenthesis)) {
                size_t prev_i = prev_code(tokens, i);
                const Tok* prev = prev_i == npos ? nullptr : &tokens[prev_i];
                t.immutable.topology.starts_parameter_list = prev && kind_is(*prev, TK::Hash);
                // `f(a)(b)` chains a call onto a call.  The `)` of an event
                // control, a `disable iff`, a cycle delay or a control header
                // ends no callee: `@(posedge clk) (req, v = d) |-> gnt` and
                // `disable iff (rst) (a && b) |-> c` open a parenthesised
                // expression, which is not a list to break per argument.
                bool after_callee_paren = prev && kind_is(*prev, TK::CloseParenthesis);
                if (after_callee_paren) {
                    const size_t prev_open = prev->immutable.syntax.matching_token;
                    const size_t before = prev_open == npos ? npos : prev_code(tokens, prev_open);
                    if (before != npos &&
                        (kind_is(tokens[before], TK::IffKeyword) || kind_is(tokens[before], TK::DoubleHash) ||
                         (closes_control_header(tokens, prev_i) && !kind_is(tokens[before], TK::Hash))))
                        after_callee_paren = false;
                }
                t.immutable.topology.starts_argument_list = prev &&
                    (kind_is(*prev, TK::Identifier) || kind_is(*prev, TK::SystemIdentifier) ||
                     kind_is(*prev, TK::MacroUsage) || after_callee_paren);
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
                // `/* a */ /* b */ assign x = 1;` -- a comment that follows
                // only own-line comments on its line leads the code as they
                // do.  The first one is broken onto its own line, so calling
                // the next one trailing left it for the following run to
                // split, one comment per run.
                const bool continues_own_line_run =
                    i > 0 && tokens[i - 1].lex.comment_kind != CommentLexemeKind::None &&
                    tokens[i - 1].immutable.comment.role == CommentRole::OwnLine &&
                    t.immutable.input_trivia.original_newlines_before == 0;
                t.immutable.comment.role =
                    ((t.immutable.input_trivia.starts_original_line && !comma_interstitial_block) ||
                     continues_own_line_run)
                    ? CommentRole::OwnLine : CommentRole::Trailing;
                t.immutable.comment.anchor_token = i == 0 ? npos : i - 1;
                t.immutable.comment.ends_line =
                    i + 1 >= tokens.size() ||
                    tokens[i + 1].immutable.input_trivia.original_newlines_before > 0;
                if (t.lex.comment_kind == CommentLexemeKind::Block &&
                    t.immutable.comment.role == CommentRole::OwnLine &&
                    t.lex.text.find('\n') != std::string::npos)
                    t.immutable.comment.source_column = t.immutable.input_trivia.original_column;
                t.immutable.comment.inside_expression = pd > 0 || bd > 0 || brd > 0;
                t.immutable.comment.inside_arg_list = pd > 0;
            }
            if (kind_is(t, TK::FunctionKeyword)) in_function_decl = true;
            if (kind_is(t, TK::TaskKeyword)) in_task_decl = true;
            if (kind_is(t, TK::ClassKeyword)) in_class_decl = true;
            if (kind_is(t, TK::CoverGroupKeyword)) in_covergroup = true;
            if (kind_is(t, TK::ModPortKeyword)) in_modport = true;
            if (kind_is(t, TK::ClockingKeyword) && t.immutable.topology.opens_indent_scope)
                in_clocking_block = true;
            if (kind_is(t, TK::OpenParenthesis)) { parens.push_back(i); ++pd; }
            else if (kind_is(t, TK::CloseParenthesis)) { if (!parens.empty()) { auto j = parens.back(); parens.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; t.immutable.topology.ends_argument_list = tokens[j].immutable.topology.starts_argument_list; } pd = std::max(0, pd - 1); }
            else if (kind_is(t, TK::OpenBracket)) { brackets.push_back(i); ++bd; }
            else if (kind_is(t, TK::CloseBracket)) { if (!brackets.empty()) { auto j = brackets.back(); brackets.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } bd = std::max(0, bd - 1); }
            // ApostropheOpenBrace (`'{`) is a single token but it opens a brace
            // exactly like OpenBrace does.  Leaving it off the stack made every
            // assignment pattern's `}` pop the wrong entry -- or nothing at all,
            // leaving matching_token unset -- and left brace_depth short by one
            // for the rest of the file.
            else if (kind_is(t, TK::OpenBrace) || kind_is(t, TK::ApostropheOpenBrace)) { braces.push_back(i); brace_ctx.emplace_back(pd, bd); ++brd; }
            else if (kind_is(t, TK::CloseBrace)) {
                if (!braces.empty()) {
                    auto j = braces.back(); braces.pop_back(); brace_ctx.pop_back();
                    tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j;
                    // A brace that directly holds a statement block is one:
                    // `constraint c { foreach (q[i]) { q[i] > 0; } }` has no
                    // `;` of its own, and no expression brace holds a block.
                    if (tokens[j].immutable.topology.opens_brace_block && !braces.empty() &&
                        brace_ctx.back() == std::pair<int, int>(pd, bd))
                        tokens[braces.back()].immutable.topology.opens_brace_block = true;
                }
                brd = std::max(0, brd - 1);
            }
            if (kind_is(t, TK::Semicolon)) {
                // A `;` at the brace's own depth makes the brace a statement
                // block.  No expression brace can hold one.
                if (!braces.empty() && brace_ctx.back() == std::pair<int, int>(pd, bd)) {
                    tokens[braces.back()].immutable.topology.opens_brace_block = true;
                    t.immutable.topology.separates_brace_block_items = true;
                }
                in_function_decl = false;
                in_task_decl = false;
                in_modport = false;
            }
            if (kind_is(t, TK::EndClassKeyword))
                in_class_decl = false;
            if (kind_is(t, TK::EndGroupKeyword))
                in_covergroup = false;
        }
        mark_property_expressions(tokens);
        for (size_t i = 0; i < tokens.size(); ++i) {
            // `bins t = (1 [->2] => 0);` -- a transition list repeats a value the
            // way a sequence repeats an expression, and sits in parentheses.
            const bool in_transition =
                tokens[i].immutable.syntax.in_covergroup && tokens[i].immutable.syntax.paren_depth > 0;
            if (!kind_is(tokens[i], TK::OpenBracket) ||
                !(tokens[i].immutable.syntax.in_property_expr || in_transition))
                continue;
            const size_t op = next_code(tokens, i + 1, tokens.size());
            tokens[i].immutable.topology.is_repetition_bracket =
                op != npos && (kind_is(tokens[op], TK::Star) || kind_is(tokens[op], TK::Plus) ||
                               kind_is(tokens[op], TK::MinusArrow) || kind_is(tokens[op], TK::Equals));
        }

        size_t stmt_start = 0;
        int stmt_pd = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (kind_is(tokens[i], TK::OpenParenthesis)) ++stmt_pd;
            else if (kind_is(tokens[i], TK::CloseParenthesis)) stmt_pd = std::max(0, stmt_pd - 1);
            // `for (int i = 0; i < n; i++)` — the header separators live inside
            // the parens and do not end the enclosing statement.
            if (kind_is(tokens[i], TK::Semicolon) && stmt_pd > 0)
                continue;
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

        // A constraint or coverpoint body with no item in it -- `constraint c {}`,
        // or one holding only a comment -- has no `;` to say so, and is a block
        // all the same: its `}` closes a scope and a label may follow it.
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (!kind_is(t, TK::OpenBrace) || t.immutable.topology.opens_brace_block ||
                t.immutable.syntax.paren_depth > 0 || t.immutable.syntax.matching_token == npos ||
                next_code(tokens, i + 1, tokens.size()) != t.immutable.syntax.matching_token)
                continue;
            for (size_t n = i; n > 0; --n) {
                const auto& h = tokens[n - 1];
                if (!is_code_token(h))
                    continue;
                if (kind_is(h, TK::Semicolon) || kind_is(h, TK::OpenBrace) || kind_is(h, TK::CloseBrace))
                    break;
                if (kind_is(h, TK::CoverPointKeyword) || kind_is(h, TK::ConstraintKeyword)) {
                    t.immutable.topology.opens_brace_block = true;
                    break;
                }
            }
        }

        // Indent scopes for braces can only be settled once every brace has been
        // classified and matched, which is why this is a second pass rather than
        // part of the walk above.  An expression brace opens no scope, and its
        // closing brace must close none either, or the indent level drifts for
        // the rest of the file.
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (is_property_operator_keyword(t)) {
                t.immutable.topology.opens_indent_scope = false;
                t.immutable.topology.closes_indent_scope = false;
            } else if (kind_is(t, TK::OpenBrace)) {
                t.immutable.topology.opens_indent_scope = t.immutable.topology.opens_brace_block;
            } else if (kind_is(t, TK::CloseBrace)) {
                const size_t open = t.immutable.syntax.matching_token;
                t.immutable.topology.closes_indent_scope =
                    open != npos && kind_is(tokens[open], TK::OpenBrace) &&
                    tokens[open].immutable.topology.opens_brace_block;
            }
        }

        // `end : blk`, `begin : blk`, `endmodule : m` -- a colon that names
        // the block its keyword opens or closes.  Only keywords take a label;
        // a `}` never does, so `{a, b}: y = 0;` is a case label and
        // `c ? {a} : {b}` a conditional.
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::Colon))
                continue;
            const size_t p = prev_code(tokens, i);
            if (p == npos)
                continue;
            const TK k = tokens[p].lex.kind;
            tokens[i].immutable.topology.is_block_name_colon =
                k == TK::BeginKeyword || is_fork_block_open(tokens, p) ||
                (is_close_block(k) && k != TK::CloseBrace) || is_outer_close(k);
        }

        // `{4{a}}`, `{(N){a}}`, `{2'd2{a}}` -- the inner brace of a
        // replication follows its multiplier directly after the outer `{`,
        // or after the `'{` of a replicated assignment pattern (`'{4{a}}`).
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::OpenBrace) || tokens[i].immutable.topology.opens_brace_block)
                continue;
            size_t start = prev_code(tokens, i);
            if (start == npos)
                continue;
            const TK k = tokens[start].lex.kind;
            if (k == TK::CloseParenthesis) {
                start = tokens[start].immutable.syntax.matching_token;
            } else if (k == TK::IntegerLiteral || k == TK::Identifier || k == TK::MacroUsage ||
                       k == TK::Question || k == TK::RealLiteral) {
                // Walk back over a based literal's pieces to its size.
                while (start != npos && tokens[start].lex.continues_vector_literal)
                    start = prev_code(tokens, start);
                if (start != npos && kind_is(tokens[start], TK::IntegerBase)) {
                    const size_t size = prev_code(tokens, start);
                    if (size != npos && kind_is(tokens[size], TK::IntegerLiteral))
                        start = size;
                }
            } else {
                continue;
            }
            const size_t outer = start == npos ? npos : prev_code(tokens, start);
            tokens[i].immutable.topology.is_replication_brace =
                outer != npos &&
                ((kind_is(tokens[outer], TK::OpenBrace) &&
                  !tokens[outer].immutable.topology.opens_brace_block) ||
                 kind_is(tokens[outer], TK::ApostropheOpenBrace));
        }

        // `q <= repeat (n) @(e) d;` -- a `repeat` right after an assignment
        // operator is a timing control on that assignment, not a loop.
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::RepeatKeyword) || !is_code_token(tokens[i]))
                continue;
            const size_t op = prev_code(tokens, i);
            tokens[i].immutable.topology.is_intra_assignment_repeat =
                op != npos && (kind_is(tokens[op], TK::Equals) || kind_is(tokens[op], TK::LessThanEquals));
        }

        mark_min_typ_max_colons(tokens);
        mark_case_items_and_macro_statements(tokens);

        // The `while` after a `do`'s body.  Needs the macro-statement ends
        // frozen just above.
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::DoKeyword) || !is_code_token(tokens[i]))
                continue;
            const size_t end = simple_statement_end_from(tokens, next_code(tokens, i + 1, tokens.size()));
            const size_t w = end == npos ? npos : next_code(tokens, end + 1, tokens.size());
            if (w != npos && kind_is(tokens[w], TK::WhileKeyword))
                tokens[w].immutable.topology.ends_do_while = true;
        }

        // `lbl: stmt` -- an identifier where a statement or item starts,
        // followed by `:`.  Runs after the case walk, whose colons it must
        // see as statement starts (`ST_A: lbl: assert ...`).
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!kind_is(tokens[i], TK::Colon) || !is_code_token(tokens[i]))
                continue;
            if (tokens[i].immutable.topology.is_case_item_colon ||
                tokens[i].immutable.topology.is_block_name_colon)
                continue;
            const size_t name = prev_code(tokens, i);
            if (name != npos && kind_is(tokens[name], TK::Identifier) &&
                tokens[i].immutable.syntax.paren_depth == 0 &&
                tokens[i].immutable.syntax.bracket_depth == 0 &&
                at_statement_start(tokens, name))
                tokens[i].immutable.topology.is_item_label_colon = true;
        }

        // How deep in indent scopes each token sits.  WrapPass decides whether
        // a list fits before IndentPass has run, so it cannot read an indent;
        // this is the part of one that is a fact about the tokens.  It is the
        // same walk IndentPass makes over the same two predicates, less what
        // only a later pass knows: a brace-less controlled body and a
        // scope-opening macro each add a level this does not count, so the
        // number can fall short of the indent and never exceeds it.
        // The outermost design unit is kept apart from the count because
        // whether it indents its items is an option, not a fact.
        {
            struct Unit { int depth; };
            struct Branch { int depth; size_t units; };
            std::vector<Unit> units;
            std::vector<Branch> branches;
            int depth = 0;
            for (size_t i = 0; i < tokens.size(); ++i) {
                auto& t = tokens[i];
                if (is_passthrough(t))
                    continue;
                if (is_conditional_preprocessor_directive(t)) {
                    using SK = slang::syntax::SyntaxKind;
                    switch (t.lex.directive_kind) {
                    case SK::IfDefDirective:
                    case SK::IfNDefDirective:
                        branches.push_back({depth, units.size()});
                        break;
                    case SK::ElsIfDirective:
                    case SK::ElseDirective:
                        // Alternative branches open the same scopes once.
                        if (!branches.empty()) {
                            depth = branches.back().depth;
                            units.resize(std::min(units.size(), branches.back().units));
                        }
                        break;
                    case SK::EndIfDirective:
                        if (!branches.empty())
                            branches.pop_back();
                        break;
                    default:
                        break;
                    }
                }
                const bool closes = closes_indent_scope_at(tokens, i) || is_outer_close(t.lex.kind);
                if (closes)
                    depth = std::max(0, depth - 1);
                if (is_outer_close(t.lex.kind) && !units.empty()) {
                    depth = units.back().depth;
                    units.pop_back();
                }
                t.immutable.topology.scope_depth = depth;
                t.immutable.topology.in_outermost_unit = !units.empty();
                const bool unit = t.immutable.topology.opens_design_unit;
                const bool nested_unit = unit && !units.empty();
                if (unit)
                    units.push_back({depth});
                if (!closes && (opens_indent_scope_at(tokens, i) || nested_unit))
                    ++depth;
            }
        }
    }

private:
    // Can a statement or item begin at `idx`?  Read from the facts already
    // frozen for the tokens before it, so a macro that ends a statement makes
    // the next macro's position a start too (`` `uvm_info(..) `CHECK(..) ``).
    static bool at_statement_start(const TokenStream& tokens, size_t idx) {
        const size_t p = prev_code(tokens, idx);
        if (p == npos)
            return true;
        const Tok& pt = tokens[p];
        const TK k = pt.lex.kind;
        if (k == TK::Semicolon || k == TK::BeginKeyword || k == TK::ElseKeyword ||
            k == TK::DoKeyword || k == TK::ForeverKeyword || k == TK::GenerateKeyword ||
            is_outer_close(k) || is_procedural_block_at(tokens, p))
            return true;
        if (k == TK::ForkKeyword)
            return is_fork_block_open(tokens, p);
        if (is_close_block(k))
            return k != TK::CloseBrace || pt.immutable.topology.closes_indent_scope;
        if (pt.immutable.topology.is_case_item_colon || pt.immutable.topology.may_end_macro_statement)
            return true;
        // `begin : name`, `end : name` -- the label is part of its keyword,
        // whatever the name lexes as.
        if (const size_t colon = prev_code(tokens, p);
            colon != npos && tokens[colon].immutable.topology.is_block_name_colon)
            return true;
        // `#10 stmt` and `#(T) stmt` -- a delay control prefixes a statement.
        if (k == TK::IntegerLiteral || k == TK::RealLiteral || k == TK::TimeLiteral ||
            k == TK::Identifier) {
            const size_t hash = prev_code(tokens, p);
            if (hash != npos && kind_is(tokens[hash], TK::Hash))
                return true;
        }
        // The `)` closing a control header or an event/delay control.
        if (k == TK::CloseParenthesis && pt.immutable.syntax.matching_token != npos) {
            const size_t owner = prev_code(tokens, pt.immutable.syntax.matching_token);
            if (owner == npos)
                return false;
            if (tokens[owner].immutable.topology.is_intra_assignment_repeat)
                return false;
            const TK o = tokens[owner].lex.kind;
            // A randsequence header is followed by its first production.
            return o == TK::IfKeyword || o == TK::ForKeyword || o == TK::ForeachKeyword ||
                   o == TK::WhileKeyword || o == TK::RepeatKeyword || o == TK::WaitKeyword ||
                   o == TK::At || o == TK::Hash || o == TK::RandSequenceKeyword;
        }
        return false;
    }

    // A case item is `<labels> : <statement>`, and the labels are arbitrary
    // expressions, so the item's colon is found by position rather than by the
    // token in front of it: the first `:` at the case body's own depth after a
    // point where an item can start -- the header's `)`, a `;`, an `end`/`join`
    // back at that depth, or a nested `endcase`.  Once found, the rest of the
    // item is a statement and its colons (statement labels, `begin : blk`,
    // ternaries, assignment-pattern keys) are not labels.  A `?` seen while
    // looking owns the next `:`, so a ternary inside a label is not cut short.
    //
    // The same walk freezes TopologyFacts::may_end_macro_statement, because the
    // two depend on each other: a semicolonless macro statement ends a case
    // item just as a `;` does (`` 2'd0: `NOP 2'd1: ... ``), and a case item's
    // colon is where a macro statement can start.
    // SyntaxFacts::in_property_expr.  Needs matching_token, so it runs after
    // the delimiter walk.
    static void mark_property_expressions(TokenStream& tokens) {
        auto is_assertion = [](TK k) {
            return k == TK::AssertKeyword || k == TK::AssumeKeyword || k == TK::CoverKeyword ||
                   k == TK::RestrictKeyword || k == TK::ExpectKeyword;
        };
        auto mark = [&](size_t first, size_t last) {
            for (size_t k = first; k <= last && k < tokens.size(); ++k)
                tokens[k].immutable.syntax.in_property_expr = true;
        };
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_code_token(tokens[i]))
                continue;
            const TK k = tokens[i].lex.kind;
            const bool decl_keyword = k == TK::PropertyKeyword || k == TK::SequenceKeyword;
            size_t open = npos;
            if (k == TK::ExpectKeyword) {
                open = next_code(tokens, i + 1, tokens.size());
            } else if (decl_keyword) {
                const size_t p = prev_code(tokens, i);
                if (p != npos && is_assertion(tokens[p].lex.kind)) {
                    open = next_code(tokens, i + 1, tokens.size());
                } else {
                    // A declaration: through its end keyword.
                    const TK end_kind = k == TK::PropertyKeyword ? TK::EndPropertyKeyword
                                                                 : TK::EndSequenceKeyword;
                    size_t e = i + 1;
                    while (e < tokens.size() && !kind_is(tokens[e], end_kind))
                        ++e;
                    if (e < tokens.size()) {
                        mark(i + 1, e - 1);
                        i = e;
                    }
                    continue;
                }
            }
            if (open == npos || !kind_is(tokens[open], TK::OpenParenthesis) ||
                tokens[open].immutable.syntax.matching_token == npos)
                continue;
            mark(open + 1, tokens[open].immutable.syntax.matching_token - 1);
        }
    }

    // A min:typ:max triple is one value, not a ternary.  It sits directly in
    // a delay or parameter-value list `#(...)`, or in a specify block's path
    // delay `= (...)` or timing-check arguments.  Colons that answer a `?`
    // at the same depth are the ternary's, and a `[` or `{` inside owns its
    // own colons.
    static void mark_min_typ_max_colons(TokenStream& tokens) {
        struct Open { bool mintypmax; int bd, brd, questions; };
        std::vector<Open> opens;
        bool in_specify = false;
        for (size_t i = 0; i < tokens.size(); ++i) {
            Tok& t = tokens[i];
            if (!is_code_token(t))
                continue;
            const auto& sx = t.immutable.syntax;
            if (kind_is(t, TK::SpecifyKeyword)) in_specify = true;
            else if (kind_is(t, TK::EndSpecifyKeyword)) in_specify = false;
            if (kind_is(t, TK::OpenParenthesis)) {
                const size_t owner = prev_code(tokens, i);
                const bool delay = owner != npos && kind_is(tokens[owner], TK::Hash);
                const bool specify_value = in_specify && owner != npos &&
                    (kind_is(tokens[owner], TK::Equals) || kind_is(tokens[owner], TK::SystemIdentifier));
                opens.push_back({delay || specify_value, sx.bracket_depth, sx.brace_depth, 0});
                continue;
            }
            if (kind_is(t, TK::CloseParenthesis)) {
                if (!opens.empty()) opens.pop_back();
                continue;
            }
            if (opens.empty())
                continue;
            Open& o = opens.back();
            if (sx.bracket_depth != o.bd || sx.brace_depth != o.brd)
                continue;
            if (kind_is(t, TK::Question)) ++o.questions;
            else if (kind_is(t, TK::Comma)) o.questions = 0;
            else if (kind_is(t, TK::Colon)) {
                if (o.questions > 0) --o.questions;
                else t.immutable.topology.is_min_typ_max_colon = o.mintypmax;
            }
        }
    }

    static void mark_case_items_and_macro_statements(TokenStream& tokens) {
        struct CaseBody {
            int pd, bd, brd, block_depth;
            size_t header_end;
            bool seeking;
            int pending_questions;
        };
        std::vector<CaseBody> cases;
        int block_depth = 0;
        auto at_body_depth = [&](const Tok& t, const CaseBody& c) {
            return t.immutable.syntax.paren_depth == c.pd &&
                   t.immutable.syntax.bracket_depth == c.bd &&
                   t.immutable.syntax.brace_depth == c.brd &&
                   block_depth == c.block_depth;
        };
        auto item_can_start = [&](CaseBody& c) {
            c.seeking = true;
            c.pending_questions = 0;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (!is_code_token(t))
                continue;

            if (kind_is(t, TK::MacroUsage) && at_statement_start(tokens, i)) {
                const bool in_case_item = !cases.empty() && !cases.back().seeking &&
                                          at_body_depth(t, cases.back());
                const size_t end = macro_statement_end(tokens, i, in_case_item);
                if (end != npos) {
                    tokens[end].immutable.topology.may_end_macro_statement = true;
                    if (in_case_item)
                        item_can_start(cases.back());
                }
            }

            if (kind_is(t, TK::BeginKeyword) || kind_is(t, TK::ForkKeyword)) {
                ++block_depth;
                continue;
            }
            if (kind_is(t, TK::EndKeyword) || kind_is(t, TK::JoinKeyword) ||
                kind_is(t, TK::JoinAnyKeyword) || kind_is(t, TK::JoinNoneKeyword)) {
                block_depth = std::max(0, block_depth - 1);
                if (!cases.empty() && at_body_depth(t, cases.back()))
                    item_can_start(cases.back());
                continue;
            }

            if (kind_is(t, TK::CaseKeyword) || kind_is(t, TK::CaseXKeyword) ||
                kind_is(t, TK::CaseZKeyword) || kind_is(t, TK::RandCaseKeyword)) {
                CaseBody c{t.immutable.syntax.paren_depth, t.immutable.syntax.bracket_depth,
                           t.immutable.syntax.brace_depth, block_depth, npos, false, 0};
                size_t open = kind_is(t, TK::RandCaseKeyword)
                    ? npos : next_code(tokens, i + 1, tokens.size());
                if (open != npos && kind_is(tokens[open], TK::OpenParenthesis))
                    c.header_end = tokens[open].immutable.syntax.matching_token;
                if (c.header_end == npos)
                    c.seeking = true;
                cases.push_back(c);
                continue;
            }
            if (cases.empty())
                continue;
            if (kind_is(t, TK::EndCaseKeyword)) {
                cases.pop_back();
                if (!cases.empty() && at_body_depth(t, cases.back()))
                    item_can_start(cases.back());
                continue;
            }

            CaseBody& c = cases.back();
            if (i == c.header_end) {
                item_can_start(c);
                continue;
            }
            if (!at_body_depth(t, c))
                continue;
            if (kind_is(t, TK::Semicolon)) {
                item_can_start(c);
            } else if (c.seeking && kind_is(t, TK::Question) &&
                       !t.lex.continues_vector_literal) {
                // `4'b1???:` -- those `?` are digits, not conditionals.
                ++c.pending_questions;
            } else if (c.seeking && kind_is(t, TK::Colon)) {
                if (c.pending_questions > 0) {
                    --c.pending_questions;
                    continue;
                }
                // `end : blk` names the block that just closed.
                if (t.immutable.topology.is_block_name_colon)
                    continue;
                t.immutable.topology.is_case_item_colon = true;
                c.seeking = false;
            }
        }
    }
};

// The keyword that owns a `#(` in a class header: `class` for the class's own
// parameter port list (`class base #(type T = int)`), `extends`/`implements`
// for a specialization of another class (`extends p #(T)`).  npos when the
// `#(` is not in a class header.
inline size_t class_header_parameter_list_owner(const TokenStream& tokens, size_t open) {
    size_t hash = prev_code(tokens, open);
    if (hash == npos || !kind_is(tokens[hash], TK::Hash))
        return npos;
    for (size_t n = hash; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::Semicolon))
            break;
        if (kind_is(tokens[i], TK::ExtendsKeyword) || kind_is(tokens[i], TK::ImplementsKeyword) ||
            kind_is(tokens[i], TK::ClassKeyword))
            return i;
    }
    return npos;
}

// `class base #(type T = int, int W = 8)` -- the class's own parameter ports.
inline bool is_class_header_parameter_list(const TokenStream& tokens, size_t open) {
    const size_t owner = class_header_parameter_list_owner(tokens, open);
    return owner != npos && kind_is(tokens[owner], TK::ClassKeyword);
}

// Reads the classification SyntaxPass already froze.  The previous version
// tested only the token before the brace for `=`, `'` or `[`, which recognised
// an assignment pattern but missed `inside {...}`, `{<<8{...}}`, and every
// concatenation reached through an operator or a comma -- their closing braces
// were pushed onto their own line as if they were an `end`.
inline bool is_expression_brace(const TokenStream& tokens, size_t brace) {
    if (brace >= tokens.size())
        return false;
    // `'{` only ever opens an assignment pattern.
    if (kind_is(tokens[brace], TK::ApostropheOpenBrace))
        return true;
    return kind_is(tokens[brace], TK::OpenBrace) &&
           !tokens[brace].immutable.topology.opens_brace_block;
}

// `{<<8{data}}` -- the `<<` of a stream concatenation, not the shift operator
// it shares a spelling with.  A shift can never appear directly after `{`,
// because a concatenation element cannot begin with one, so the position alone
// separates them without any lookahead.
inline bool is_stream_operator_at(const TokenStream& tokens, size_t idx) {
    if (idx >= tokens.size())
        return false;
    if (!kind_is(tokens[idx], TK::LeftShift) && !kind_is(tokens[idx], TK::RightShift))
        return false;
    const size_t p = prev_code(tokens, idx);
    return p != npos && kind_is(tokens[p], TK::OpenBrace);
}

// `{}` with nothing between the braces, not even a comment.
inline bool is_empty_brace_pair(const TokenStream& tokens, size_t brace) {
    return brace < tokens.size() && kind_is(tokens[brace], TK::OpenBrace) &&
           brace + 1 < tokens.size() && tokens[brace].immutable.syntax.matching_token == brace + 1;
}

// Whether `brace` sits in a statement block that is kept on its line: an
// inline constraint inside parentheses, `if (!randomize(x) with { ... })`,
// holding no comment that forces a line to end.
inline bool is_in_inline_brace_block(const TokenStream& tokens, size_t brace) {
    if (tokens[brace].immutable.syntax.paren_depth == 0)
        return false;
    for (size_t n = brace; n > 0; --n) {
        const auto& e = tokens[n - 1];
        if (!is_code_token(e))
            continue;
        if ((kind_is(e, TK::CloseBrace) || kind_is(e, TK::CloseParenthesis)) &&
            e.immutable.syntax.matching_token != npos && e.immutable.syntax.matching_token < n - 1) {
            n = e.immutable.syntax.matching_token + 1;
            continue;
        }
        if (!kind_is(e, TK::OpenBrace))
            continue;
        if (!e.immutable.topology.opens_brace_block || e.immutable.syntax.paren_depth == 0)
            return false;
        const size_t close = e.immutable.syntax.matching_token;
        for (size_t k = n; close != npos && k < close; ++k)
            if (tokens[k].lex.comment_kind == CommentLexemeKind::Line ||
                (tokens[k].lex.comment_kind != CommentLexemeKind::None &&
                 tokens[k].immutable.comment.role == CommentRole::OwnLine))
                return false;
        return true;
    }
    return false;
}

inline bool is_multiline_brace_construct(const TokenStream& tokens, size_t brace) {
    if (brace >= tokens.size() || !kind_is(tokens[brace], TK::OpenBrace))
        return false;
    // A `dist` list inside an inline constraint stays with it; expanding the
    // list alone tore the condition apart.
    if (is_in_inline_brace_block(tokens, brace))
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
enum class MacroRole { ObjectLikeExpr, FunctionLikeExpr, StatementLike, DeclarationLike, ControlFlowLike, BlockBeginLike, BlockEndLike, StatementTerminatorLike };

struct MacroClassifier {
    std::unordered_set<std::string> object_like_expr;
    std::unordered_set<std::string> function_like_expr;
    std::unordered_set<std::string> statement_like;
    std::unordered_set<std::string> declaration_like;
    std::unordered_set<std::string> control_flow_like;
    std::unordered_set<std::string> statement_terminator_like;
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
        add(statement_terminator_like, m.statement_terminator_like);
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
        if (statement_terminator_like.count(name)) return MacroRole::StatementTerminatorLike;
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

    // classify() defaults an unknown macro to ObjectLikeExpr; this is only
    // true when the user said so.
    bool is_configured_expression(const std::string& raw_text) const {
        const std::string name = extract_name(raw_text);
        return object_like_expr.count(name) > 0 || function_like_expr.count(name) > 0;
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
                t.mutable_.macro.suppress_alignment = true;
                // Whitespace sensitivity is about the argument spelling; the
                // role is about where the invocation sits among statements.
                // They are independent -- a multi-line `define used as a
                // declaration_like item helper is both -- so freezing the
                // arguments must not skip the role below.
                if (whitespace_sensitive) {
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
                }
                {
                    MacroRole role = mc.classify(t.lex.text);
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
        mark_statement_ends(tokens, mc);
    }
private:
    // Settle where semicolonless macro statements end.  SyntaxPass found the
    // ones TokenKinds can prove; [format.macros] overrides it both ways -- a
    // configured statement or item macro always ends one (unless its own `;`
    // follows), a configured terminator (`` `define SEMI ; ``) always ends
    // the statement it closes, and a configured expression or control-flow
    // macro never does.
    static void mark_statement_ends(TokenStream& tokens, const MacroClassifier& mc) {
        for (size_t i = 0; i < tokens.size(); ++i) {
            const Tok& t = tokens[i];
            if (!kind_is(t, TK::MacroUsage) || !is_code_token(t))
                continue;
            const size_t end = macro_invocation_end(tokens, i);
            bool ends = tokens[end].immutable.topology.may_end_macro_statement;
            const MacroRole role = mc.classify(t.lex.text);
            // A block-begin macro opens a body that runs to its block-end
            // macro (see simple_statement_end_from), exactly like `begin`.
            if (role == MacroRole::StatementTerminatorLike) {
                ends = true;
            } else if (mc.is_configured_expression(t.lex.text) || role == MacroRole::ControlFlowLike ||
                       role == MacroRole::BlockBeginLike) {
                ends = false;
            } else if (t.mutable_.macro.force_line_break) {
                const size_t next = next_code(tokens, end + 1, tokens.size());
                ends = next == npos || !kind_is(tokens[next], TK::Semicolon);
            }
            tokens[end].mutable_.macro.ends_statement = ends;
        }
    }

    const FormatOptions& opts_;
};

// WrapPass owns WrapMetadata and reads syntax/macro metadata.  It uses stable
// syntax/source facts only; there is no dependency on rendered columns.
class WrapPass final : public IFormatPass {
public:
    explicit WrapPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "wrap"; }
    void run(TokenStream& tokens) override {
        // A macro invocation that already carries its own trailing `;` in
        // source is an ordinary statement-terminated call, not the
        // semicolonless UVM/OpenTitan pattern the statement-macro line-break
        // logic below exists for.  Forcing a break at the macro token or its
        // invocation's closing parenthesis in that case would split the
        // statement's own semicolon onto its own line; the semicolon's own
        // must_break_after handling further down already places the correct
        // line break after it.
        auto next_is_source_semicolon = [&](size_t after) {
            size_t next = next_code(tokens, after + 1, tokens.size());
            return next != npos && kind_is(tokens[next], TK::Semicolon);
        };

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
            // PP directives (ifdef/endif/else/define/…) are always on their own
            // line -- except a conditional inside a dimension, which selects a
            // bound (`` [`ifdef W 63 `else 31 `endif :0] ``) and reads best
            // where it stands.  It is a complete lexeme, so staying inline is
            // as safe as any other token.
            if (t.lex.is_directive &&
                !(is_conditional_preprocessor_directive(t) && t.immutable.syntax.bracket_depth > 0)) {
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
                if (!next_is_source_semicolon(break_at))
                    tokens[break_at].mutable_.wrap.must_break_after = true;
            }
            // A semicolonless macro statement or item (`` `uvm_info(...) ``,
            // `` `ASSERT(...) ``) ends here, so the next one starts a line.
            // Where that is, is decided once by SyntaxPass and MacroPass
            // (MacroMetadata::ends_statement); an invocation followed by its
            // own `;` never carries it.
            if (t.mutable_.macro.ends_statement && !next_is_source_semicolon(i))
                t.mutable_.wrap.must_break_after = true;
            if (i > 0 &&
                tokens[i - 1].immutable.comment.role == CommentRole::Trailing &&
                tokens[i - 1].lex.comment_kind == CommentLexemeKind::Line)
                t.mutable_.wrap.must_break_before = true;
            if (kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth == 0) {
                // Every statement-ending `;` ends its line -- including one
                // before a delay (`@(posedge clk); #1 x = y;`), which used to
                // be exempt.  The module-header `import p::*; #(...)` that
                // exemption was for breaks as a header import anyway.
                t.mutable_.wrap.must_break_after = true;
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
                if (control != npos && tokens[control].immutable.topology.ends_do_while)
                    t.mutable_.wrap.must_break_before = false;
            }
            if (kind_is(t, TK::CloseParenthesis) && t.immutable.syntax.matching_token != npos) {
                size_t before_open = prev_code(tokens, t.immutable.syntax.matching_token);
                if (before_open != npos && !is_property_operator_keyword(tokens[before_open]) &&
                    (kind_is(tokens[before_open], TK::CaseKeyword) ||
                     kind_is(tokens[before_open], TK::CaseXKeyword) ||
                     kind_is(tokens[before_open], TK::CaseZKeyword) ||
                     kind_is(tokens[before_open], TK::RandSequenceKeyword))) {
                    // `case (x) inside` / `case (x) matches` -- the header
                    // runs through the keyword.
                    const size_t after = next_code(tokens, i + 1, tokens.size());
                    if (after != npos && (kind_is(tokens[after], TK::InsideKeyword) ||
                                          kind_is(tokens[after], TK::MatchesKeyword)))
                        tokens[after].mutable_.wrap.must_break_after = true;
                    else
                        t.mutable_.wrap.must_break_after = true;
                }
            }
            // `generate` opens a region like `begin`; its first item starts
            // the next line.
            if (kind_is(t, TK::GenerateKeyword) || kind_is(t, TK::SpecifyKeyword) ||
                kind_is(t, TK::TableKeyword))
                t.mutable_.wrap.must_break_after = true;
            // Each UDP row is a line of its own.
            if (t.lex.is_table_row)
                t.mutable_.wrap.must_break_before = true;
            // `randcase` has no header; its first item starts the next line.
            if (kind_is(t, TK::RandCaseKeyword))
                t.mutable_.wrap.must_break_after = true;
            if (kind_is(t, TK::Comma)) t.mutable_.wrap.can_break_after = true;
            // Close-block keywords always start a new line; CloseBrace only when
            // not inside a parenthesised expression (e.g. `inside {A, B}`).
            const bool property_keyword = is_property_operator_keyword(t);
            if (is_outer_close(t.lex.kind) ||
                (is_close_block(t.lex.kind) && !property_keyword &&
                 !(kind_is(t, TK::CloseBrace) &&
                   (t.immutable.syntax.paren_depth > 0 ||
                    is_empty_brace_pair(tokens, t.immutable.syntax.matching_token) ||
                    (t.immutable.syntax.matching_token != npos &&
                     is_expression_brace(tokens, t.immutable.syntax.matching_token))))))
                t.mutable_.wrap.must_break_before = true;
            size_t next_i = next_code(tokens, i + 1, tokens.size());
            bool followed_by_label_colon =
                next_i != npos && tokens[next_i].immutable.topology.is_block_name_colon;
            // `} name;`, and `} [1:0] name;` -- a packed dimension after a
            // struct, union or enum body belongs to the declaration the `}`
            // is in the middle of.  A statement block's `}` is in the middle
            // of nothing: `foreach (a[i]) { ... } b == 1;` in a constraint, or
            // `cx: cross a, b { ... } cy: cross a, b;`, starts a new item.
            const size_t close_opener = kind_is(t, TK::CloseBrace) ? t.immutable.syntax.matching_token : npos;
            const bool closes_statement_block =
                close_opener != npos && tokens[close_opener].immutable.topology.opens_brace_block &&
                !is_struct_or_union_body_brace(tokens, close_opener);
            bool close_brace_before_decl_name =
                kind_is(t, TK::CloseBrace) && !closes_statement_block && next_i != npos &&
                (kind_is(tokens[next_i], TK::Identifier) || kind_is(tokens[next_i], TK::SystemIdentifier) ||
                 kind_is(tokens[next_i], TK::OpenBracket));
            bool close_brace_before_semicolon =
                kind_is(t, TK::CloseBrace) && next_i != npos && kind_is(tokens[next_i], TK::Semicolon);
            bool close_before_inline_else =
                kind_is(t, TK::CloseBrace) && next_i != npos && kind_is(tokens[next_i], TK::ElseKeyword) &&
                !opts_.statement.wrap_end_else_clauses;
            // `endcase;` -- a property's `case` is an expression, and the
            // `;` that ends the property follows it on its line.
            bool property_endcase_before_semicolon =
                kind_is(t, TK::EndCaseKeyword) &&
                (t.immutable.syntax.in_property_expr || t.immutable.syntax.in_production) && next_i != npos &&
                kind_is(tokens[next_i], TK::Semicolon);
            bool end_before_do_while =
                kind_is(t, TK::EndKeyword) && next_i != npos && tokens[next_i].immutable.topology.ends_do_while;
            if ((kind_is(t, TK::BeginKeyword) && !followed_by_label_colon) ||
                (is_fork_block_open(tokens, i) && !followed_by_label_colon) ||
                (is_outer_close(t.lex.kind) && !followed_by_label_colon) ||
                (is_close_block(t.lex.kind) && !followed_by_label_colon && !property_keyword &&
                 !end_before_do_while &&
                 !property_endcase_before_semicolon &&
                 !close_brace_before_decl_name &&
                 !close_brace_before_semicolon &&
                 !close_before_inline_else &&
                 !(kind_is(t, TK::CloseBrace) &&
                   (t.immutable.syntax.paren_depth > 0 ||
                    (t.immutable.syntax.matching_token != npos &&
                     is_expression_brace(tokens, t.immutable.syntax.matching_token)))))) {
                t.mutable_.wrap.must_break_after = true;
            }
            // The name after `begin :` / `end :` ends the line its keyword
            // would have ended -- whatever it lexes as (`endfunction : new`).
            if (is_code_token(t)) {
                size_t p = prev_code(tokens, i);
                if (p != npos && tokens[p].immutable.topology.is_block_name_colon)
                    t.mutable_.wrap.must_break_after = true;
            }
            if (opts_.statement.begin_newline &&
                (kind_is(t, TK::BeginKeyword) ||
                 is_fork_block_open(tokens, i) ||
                 (kind_is(t, TK::OpenBrace) && !is_expression_brace(tokens, i) &&
                  !is_empty_brace_pair(tokens, i) && t.immutable.syntax.paren_depth == 0)))
                t.mutable_.wrap.must_break_before = true;
            if (kind_is(t, TK::OpenBrace) && is_multiline_brace_construct(tokens, i)) {
                const bool empty = is_empty_brace_pair(tokens, i);
                if (opts_.statement.begin_newline && !empty)
                    t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = !empty;
                if (t.immutable.syntax.matching_token != npos) {
                    size_t close = t.immutable.syntax.matching_token;
                    tokens[close].mutable_.wrap.must_break_before = !empty;
                    size_t after_close = next_code(tokens, close + 1, tokens.size());
                    tokens[close].mutable_.wrap.must_break_after =
                        !(after_close != npos && kind_is(tokens[after_close], TK::Semicolon));
                }
            }
            if (kind_is(t, TK::OpenBrace)) {
                // A statement block's `}` already starts a line; its `{` ends
                // one, as `begin` does (`first : { a = 1; };` in randsequence).
                // Not inside parentheses: an inline constraint in a condition
                // (`if (!randomize() with { a < 5; })`) has its `;`s and `}`
                // kept on the line, so breaking only after `{` split it once.
                // One holding a `//` or own-line comment cannot stay on a line.
                auto holds_breaking_comment = [&]() {
                    const size_t close = t.immutable.syntax.matching_token;
                    if (close == npos)
                        return false;
                    for (size_t k = i + 1; k < close && k < tokens.size(); ++k)
                        if (tokens[k].lex.comment_kind == CommentLexemeKind::Line ||
                            (tokens[k].lex.comment_kind != CommentLexemeKind::None &&
                             tokens[k].immutable.comment.role == CommentRole::OwnLine))
                            return true;
                    return false;
                };
                if (is_empty_brace_pair(tokens, i)) {
                    // `constraint c {}` -- nothing to put on a line of its own.
                } else if (is_struct_or_union_body_brace(tokens, i) ||
                    (t.immutable.topology.opens_brace_block &&
                     (t.immutable.syntax.paren_depth == 0 || holds_breaking_comment())))
                    t.mutable_.wrap.must_break_after = true;
            }
            if (opts_.statement.wrap_end_else_clauses && kind_is(t, TK::ElseKeyword) && i > 0 && (kind_is(tokens[i - 1], TK::EndKeyword) || kind_is(tokens[i - 1], TK::CloseBrace))) t.mutable_.wrap.must_break_before = true;
            // else always breaks unless wrap_end_else_clauses handled it above
            if (kind_is(t, TK::ElseKeyword) && !property_keyword) {
                bool prev_is_end_or_brace = (i > 0 && (kind_is(tokens[i-1], TK::EndKeyword) || kind_is(tokens[i-1], TK::CloseBrace)));
                if (!prev_is_end_or_brace)
                    t.mutable_.wrap.must_break_before = true;
            }

            // Any statement/block terminator that is followed by a same-line
            // trailing comment should keep that comment attached and move the
            // physical line break after the comment.  The semicolon path above
            // handles ordinary statements, but EOF comments after final
            // `endmodule` / `endclass` labels need the same treatment.
            //
            // The break goes after the whole run of trailing comments, not
            // the first: `end /* p_next */ // combinational` is one line.
            if (t.mutable_.wrap.must_break_after && t.lex.comment_kind == CommentLexemeKind::None) {
                size_t last_trailing = npos;
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (is_passthrough(tokens[j])) continue;
                    if (tokens[j].lex.comment_kind == CommentLexemeKind::None ||
                        tokens[j].immutable.comment.role != CommentRole::Trailing)
                        break;
                    last_trailing = j;
                }
                if (last_trailing != npos) {
                    t.mutable_.wrap.must_break_after = false;
                    for (size_t j = i + 1; j < last_trailing; ++j)
                        tokens[j].mutable_.wrap.must_break_after = false;
                    tokens[last_trailing].mutable_.wrap.must_break_after = true;
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
            //   parameter int W = 8
            // `ifdef WIDE
            //   , parameter int X = 64
            // A comma right after a conditional directive already starts a
            // line; it leads the item after it rather than ending the one
            // before, so the two share that line.
            auto leading_comma = [&](size_t comma) {
                if (comma == npos || comma >= tokens.size() || !kind_is(tokens[comma], TK::Comma))
                    return false;
                for (size_t p = comma; p > open + 1; --p) {
                    const Tok& t = tokens[p - 1];
                    if (t.lex.is_directive)
                        return true;
                    // `input a // c` / `, input b` -- a `//` comment ends
                    // the line the same way, and the comma after it would
                    // otherwise sit on a line of its own.
                    if (t.lex.comment_kind == CommentLexemeKind::Line)
                        return true;
                    if (is_code_token(t))
                        return false;
                }
                return false;
            };
            // A statement block (`cp: coverpoint a { bins t = (1 => 2), (4 => 5); }`)
            // is `;`-separated: its statements break at their own `;`, and a
            // top-level comma belongs to the statement it sits in.
            const bool statement_block = kind == WrapListKind::BraceBlock &&
                                         tokens[open].immutable.topology.opens_brace_block;
            for (size_t n = 0; n < items.size(); ++n) {
                if (statement_block && n > 0)
                    break;
                const auto& item = items[n];
                size_t leading_block_comment = npos;
                if (n > 0 && kind != WrapListKind::InstancePorts) {
                    for (size_t c = item.first; c > open + 1; --c) {
                        size_t p = c - 1;
                        if (tokens[p].lex.comment_kind == CommentLexemeKind::Block) {
                            // `rst_n, /* reset */` at the end of its line
                            // describes the item before the comma and stays
                            // there; see the break placement below.
                            const bool trails_previous_item =
                                tokens[p].immutable.comment.role == CommentRole::Trailing &&
                                tokens[p].immutable.comment.ends_line;
                            if (!(p + 1 < tokens.size() && tokens[p + 1].lex.comment_kind != CommentLexemeKind::None &&
                                  tokens[p + 1].immutable.comment.role == CommentRole::OwnLine) &&
                                !trails_previous_item)
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
                } else if (n > 0 && leading_comma(prev_code(tokens, item.first)))
                    tokens[item.first].mutable_.wrap.must_break_before = false;
                else if (break_first_item || n > 0)
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
                // A leading comma shares its line with the item after it;
                // the break between the two items is already in front of it.
                if (item.comma != npos && !statement_block && !leading_comma(item.comma)) {
                    size_t break_token = item.comma;
                    for (size_t c = item.comma + 1; c < tokens.size(); ++c) {
                        if (tokens[c].lex.comment_kind != CommentLexemeKind::None &&
                            tokens[c].immutable.comment.role == CommentRole::Trailing &&
                            (kind == WrapListKind::InstancePorts ||
                             tokens[c].lex.comment_kind == CommentLexemeKind::Line ||
                             tokens[c].immutable.comment.ends_line ||
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

            // A port declared without a keyword still declares: `bus_if.mp m`,
            // `pkg::cfg_t cfg`, `my_t [3:0] v` -- a name straight after a
            // name or a `]`.  Non-ANSI items are bare port expressions (`a`,
            // `a[3:0]`, `.a(b)`, `{a, b}`) and never have that shape.
            auto declares_port = [&](size_t first, size_t last) {
                // `interface g` / `interface.mp g` -- a generic interface port.
                if (is_declaration_keyword(tokens[first].lex.kind) || kind_is(tokens[first], TK::InterfaceKeyword))
                    return true;
                int pd = 0, bd = 0, brd = 0;
                size_t prev = npos;
                for (size_t k = first; k <= last; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenParenthesis)) ++pd;
                    else if (kind_is(tokens[k], TK::CloseParenthesis) && pd > 0) --pd;
                    else if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (kind_is(tokens[k], TK::OpenBrace)) ++brd;
                    else if (kind_is(tokens[k], TK::CloseBrace) && brd > 0) --brd;
                    if (pd != 0 || bd != 0 || brd != 0)
                        continue;
                    if (prev != npos && is_identifier_like(tokens[k]) &&
                        (is_identifier_like(tokens[prev]) || kind_is(tokens[prev], TK::CloseBracket)))
                        return true;
                    prev = k;
                }
                return false;
            };
            bool non_ansi_ports = kind == WrapListKind::ModulePorts && !items.empty();
            for (size_t n = 0; non_ansi_ports && n < items.size(); ++n)
                if (declares_port(items[n].first, items[n].last))
                    non_ansi_ports = false;
            if (non_ansi_ports) {
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

            // `input wire [7:0] a, b,` and a modport's `output valid, data,`:
            // an item that is only a name continues the declaration before
            // it, so it stays on that declaration's line.  Only after a
            // declaration item -- a non-ANSI list is all bare names and keeps
            // its own layout above.
            // The list is ANSI when any item declares; the first one need not
            // start with a keyword (`bus_if.master bus, input logic clk, rst_n`).
            const bool ansi_ports = kind == WrapListKind::ModulePorts && !items.empty() && !non_ansi_ports;
            if (ansi_ports || kind == WrapListKind::ModportBody) {
                auto is_bare_name = [&](const ListItem& item) {
                    // A modport port expression `.d(data)` continues the
                    // direction before it the same way a name does.
                    if (kind == WrapListKind::ModportBody && kind_is(tokens[item.first], TK::Dot)) {
                        const size_t name = next_code(tokens, item.first + 1, item.last + 1);
                        const size_t open = name == npos ? npos : next_code(tokens, name + 1, item.last + 1);
                        return open != npos && kind_is(tokens[open], TK::OpenParenthesis) &&
                               tokens[open].immutable.syntax.matching_token != npos &&
                               next_code(tokens, tokens[open].immutable.syntax.matching_token + 1,
                                         item.last + 1) == npos;
                    }
                    if (!kind_is(tokens[item.first], TK::Identifier))
                        return false;
                    size_t k = next_code(tokens, item.first + 1, item.last + 1);
                    while (k != npos && kind_is(tokens[k], TK::OpenBracket) &&
                           tokens[k].immutable.syntax.matching_token != npos)
                        k = next_code(tokens, tokens[k].immutable.syntax.matching_token + 1, item.last + 1);
                    return k == npos || kind_is(tokens[k], TK::Equals);
                };
                auto comma_carries_line_comment = [&](size_t comma) {
                    for (size_t c = comma + 1; c < tokens.size(); ++c) {
                        if (tokens[c].lex.comment_kind != CommentLexemeKind::None)
                            return true;
                        if (is_code_token(tokens[c]))
                            return false;
                    }
                    return false;
                };
                bool continues = false; // an earlier item on this line declared
                for (size_t n = 0; n < items.size(); ++n) {
                    const bool bare = is_bare_name(items[n]);
                    if (n > 0 && bare && continues && items[n - 1].comma != npos &&
                        !comma_carries_line_comment(items[n - 1].comma)) {
                        tokens[items[n].first].mutable_.wrap.must_break_before = false;
                        tokens[items[n - 1].comma].mutable_.wrap.must_break_after = false;
                    } else {
                        // A non-ANSI expression (`.a(x)`) declares nothing a
                        // name could continue.
                        continues = !bare && (kind == WrapListKind::ModportBody ||
                                              declares_port(items[n].first, items[n].last));
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

        // A brace-less body starts its own line, so the calls below must be
        // measured from there rather than from the end of its header.
        apply_procedural_block_wrap(tokens);
        apply_single_statement_control_wrap(tokens);

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
                // A call inside a dimension or select (`logic [f(a, b)-1:0]`,
                // `mem[h(a, b)]`) is an operand, like a call inside another
                // call's arguments: breaking it one argument per line splits
                // the declaration or select around it.
                nested_argument_open[i] = !argument_stack.empty() ||
                                          tokens[i].immutable.syntax.bracket_depth > 0;
                // A `#(...)` list is a list of values too: the paren of a
                // named override `.W(16)` inside it is an operand, not a call.
                if (tokens[i].immutable.topology.starts_argument_list ||
                    tokens[i].immutable.topology.starts_parameter_list)
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
                // Any `#(` that is not a parameter port list -- a
                // specialization (`extends p #(T)`), an instance's override
                // list, a parameterized type -- is a list of values: it stays
                // on one line unless it does not fit, or something inside
                // ends a line anyway: a `//` comment, an own-line comment or
                // a directive.  Left joined, whatever follows that break
                // lands at the statement's indent and reads as a new
                // statement.  The declaration gate below -- one parameter
                // per line -- is for parameter port lists.
                const bool declaration = find_header_keyword_before(tokens, open) != npos ||
                                         is_class_header_parameter_list(tokens, open);
                auto items = top_level_list_items(tokens, open + 1, close);
                if (items.empty())
                    continue;
                bool block = opts_.module.parameter_layout != "hanging";
                const int one_line = line_prefix_width(tokens, open, opts_) + 1 +
                                     compact_width(tokens, open + 1, close) + 1;
                const bool directive = parameter_list_contains_directive(tokens, open, close);
                bool breaks_inside = directive;
                for (size_t k = open + 1; k < close && !breaks_inside; ++k)
                    breaks_inside = tokens[k].lex.comment_kind == CommentLexemeKind::Line ||
                                    (tokens[k].lex.comment_kind != CommentLexemeKind::None &&
                                     tokens[k].immutable.comment.role == CommentRole::OwnLine);
                bool expand = declaration
                    ? block || items.size() > 1 || directive ||
                      one_line > opts_.function_declaration.line_length
                    : breaks_inside || one_line > opts_.function_call.line_length;
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
                // Only a modport item's own parentheses: its name follows
                // `modport` or, in `modport a (...), b (...);`, a comma at
                // the keyword's depth.  A port expression `.a(addr)` and a
                // prototype `import task send(...)` inside the item are not.
                size_t before_name = prev_code(tokens, prev);
                const bool modport_item =
                    before_name != npos &&
                    (kind_is(tokens[before_name], TK::ModPortKeyword) ||
                     (kind_is(tokens[before_name], TK::Comma) &&
                      tokens[open].immutable.syntax.in_modport &&
                      tokens[open].immutable.syntax.paren_depth == 0));
                if (modport_item) {
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
            // A gate's terminals are positional and few; the instance stays
            // on its line rather than being broken like a call.
            if (is_gate_terminal_open(tokens, open))
                continue;

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
            // `sequence s(a, b)`, `property p(...)`, `let max(a, b)`,
            // `covergroup cg(int lo, int hi)`, `checker chk(logic clk)` --
            // the formals of a declaration, laid out like a function's.
            if (const size_t name = prev_code(tokens, open);
                !is_decl && name != npos && kind_is(tokens[name], TK::Identifier)) {
                const size_t kw = prev_code(tokens, name);
                is_decl = kw != npos && (kind_is(tokens[kw], TK::SequenceKeyword) ||
                                         kind_is(tokens[kw], TK::PropertyKeyword) ||
                                         kind_is(tokens[kw], TK::LetKeyword) ||
                                         kind_is(tokens[kw], TK::CoverGroupKeyword) ||
                                         kind_is(tokens[kw], TK::CheckerKeyword));
            }
            if (is_decl) {
                int approx = line_prefix_width(tokens, open, opts_) + 1 + compact_width(tokens, open + 1, close) + 1;
                // `function int f( // why` -- a comment right after the `(`
                // ends that line, so the arguments cannot hang from it: the
                // list is laid out as a block whatever its length.
                const bool comment_after_open =
                    open + 1 < close && tokens[open + 1].lex.comment_kind != CommentLexemeKind::None &&
                    !is_passthrough(tokens[open + 1]);
                if (comment_after_open && !top_level_list_items(tokens, open + 1, close).empty()) {
                    apply_list(open, WrapListKind::FunctionDeclBlock, true, true, true);
                } else if (approx > opts_.function_declaration.line_length) {
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
                    // Breaking helps only if the arguments then start inside
                    // the limit: after the `(` for a hanging list, one indent
                    // past the callee for a block list.  A call already past
                    // the limit would only push them further right.
                    const int open_end = line_prefix_width(tokens, open, opts_) + 1;
                    int approx = open_end + compact_width(tokens, open + 1, close) + 1;
                    const size_t callee = prev_code(tokens, open);
                    const int args_col = opts_.function_call.layout == "hanging" || callee == npos
                                             ? open_end
                                             : line_prefix_width(tokens, callee, opts_) + opts_.indent_size;
                    if (approx > opts_.function_call.line_length &&
                        args_col <= opts_.function_call.line_length)
                        do_break = true;
                }
                // `foo(a, // first` / `b, c);` -- a comment that ends a line
                // between the arguments breaks the list whatever its length,
                // and a list that breaks breaks at every argument.  Leaving
                // the decision at "fits" broke it at the comment alone.
                if (!do_break && items.size() > 1) {
                    const int inner = tokens[open].immutable.syntax.paren_depth + 1;
                    for (size_t k = open + 1; k < close && !do_break; ++k) {
                        const Tok& c = tokens[k];
                        if (c.lex.comment_kind == CommentLexemeKind::None || is_passthrough(c) ||
                            c.immutable.syntax.paren_depth != inner ||
                            c.immutable.syntax.bracket_depth != tokens[open].immutable.syntax.bracket_depth ||
                            c.immutable.syntax.brace_depth != tokens[open].immutable.syntax.brace_depth)
                            continue;
                        do_break = c.immutable.comment.role == CommentRole::OwnLine ||
                                   c.lex.comment_kind == CommentLexemeKind::Line;
                    }
                }
                if (do_break && opts_.function_call.break_policy != "never") {
                    bool hanging = opts_.function_call.layout == "hanging";
                    apply_list(open, hanging ? WrapListKind::FunctionHanging
                                             : WrapListKind::FunctionBlock,
                               !hanging, !hanging, !hanging);
                }
            }
        }

        // Last, so no list packing above can break inside these.
        freeze_attribute_instances(tokens);
        freeze_vector_literals(tokens);

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

        // A blank line BlankLinePass keeps also ends the line before it, even
        // in the middle of a statement (`y = a +` / blank / `f(b);`).  Record
        // that break here so every later pass measures the line the renderer
        // will actually start there.
        if (opts_.blank_lines_between_items > 0) {
            for (size_t i = 1; i < tokens.size(); ++i)
                if (!is_passthrough(tokens[i]) && is_blank_line_boundary(tokens, i))
                    tokens[i].mutable_.wrap.must_break_before = true;
        }

        mark_expression_continuations(tokens);
    }
private:
    // A comment inside an expression ends a line in the middle of it:
    //
    //   assign d = a +
    //              // why
    //              b;
    //
    // The comment and the operand after it continue the expression, so they
    // are indented past the statement rather than landing at its column,
    // where they read as a new statement.  A line continues an expression
    // when the code before it cannot end one -- an operator, an opening
    // delimiter, a comma inside one -- or when the line itself starts with a
    // binary operator (`| b;`).  Only lines no list layout owns: an item of
    // a wrapped list, and the comments between items, take the list's
    // indent, and a statement block's contents their scope's.  Decided here,
    // after every break is final, from TokenKinds only.
    static void mark_expression_continuations(TokenStream& tokens) {
        std::vector<size_t> opens; // enclosing delimiters, innermost last
        size_t prev = npos;        // last code token
        // Unmatched conditional `?`s per nesting level (index 0 is outside
        // every delimiter), so a `:` is known to be the conditional's second
        // half rather than a label, a range or a pattern key.
        std::vector<int> questions(1, 0);
        bool prev_is_conditional_colon = false;
        struct Branch { size_t prev; std::vector<size_t> opens; std::vector<int> questions; bool conditional_colon; };
        std::vector<Branch> branches; // state at each open `ifdef, innermost last
        for (size_t i = 0; i < tokens.size(); ++i) {
            Tok& t = tokens[i];
            if (is_passthrough(t))
                continue;
            const bool starts_line = i > 0 && (t.mutable_.wrap.must_break_before ||
                                               tokens[i - 1].mutable_.wrap.must_break_after);
            if (starts_line && prev != npos && !t.lex.is_directive &&
                !t.mutable_.macro.suppress_wrapping && !t.lex.in_attribute_instance &&
                t.mutable_.wrap.list_kind == WrapListKind::None) {
                const Tok& p = tokens[prev];
                const size_t enclosing = opens.empty() ? npos : opens.back();
                const bool in_list = enclosing != npos &&
                                     tokens[enclosing].mutable_.wrap.list_open == enclosing &&
                                     tokens[enclosing].mutable_.wrap.list_kind != WrapListKind::None;
                const bool list_boundary = in_list && (prev == enclosing || kind_is(p, TK::Comma));
                const bool block_open = (kind_is(p, TK::OpenBrace) &&
                                         (p.immutable.topology.opens_brace_block ||
                                          is_struct_or_union_body_brace(tokens, prev)));
                const TK pk = p.lex.kind;
                // `case (s) inside` / `matches`: the header's own break, set
                // above, ends the header; the first case item is no operand.
                const bool case_header_end = (pk == TK::InsideKeyword || pk == TK::MatchesKeyword) &&
                                             p.mutable_.wrap.must_break_after;
                // `always @*` -- the `*` is an event control, not a product.
                const size_t before_prev = prev_code(tokens, prev);
                const bool event_star = pk == TK::Star && before_prev != npos &&
                                        kind_is(tokens[before_prev], TK::At);
                const bool open_delim = pk == TK::OpenParenthesis || pk == TK::OpenBracket ||
                                        pk == TK::OpenBrace || pk == TK::ApostropheOpenBrace;
                // `@(posedge a or` -- the event list's `or` joins two events.
                // A property's `a or b` joins two operands the same way.
                const bool event_or = pk == TK::OrKeyword &&
                                      (p.immutable.syntax.paren_depth > 0 || p.immutable.syntax.in_property_expr);
                // `: b;` -- a line led by the conditional's `:` is its second half.
                const bool leads_conditional_colon = kind_is(t, TK::Colon) && questions.back() > 0;
                // `` `ifdef INV ~ `endif `` / `clk;` -- an operator that is
                // only ever a prefix still has its operand to come.
                const bool prefix_only_op = pk == TK::Tilde || pk == TK::Exclamation ||
                                            pk == TK::TildeAnd || pk == TK::TildeOr;
                // `logic a, // why` / `b;` -- a declarator or an assignment
                // after a comma outside every delimiter is the rest of the
                // statement.  Not after a list's `)` (`sub u1 (...),` /
                // `u2 (...)`, a modport's `), dbg (`): those items own their
                // layout.  And not a case item's next label (`S1, // c` /
                // `S2: x = 1;`), which a `:` reached before anything that
                // ends a declarator gives away.
                bool statement_comma = pk == TK::Comma && enclosing == npos;
                if (statement_comma && before_prev != npos &&
                    kind_is(tokens[before_prev], TK::CloseParenthesis)) {
                    const size_t list = tokens[before_prev].immutable.syntax.matching_token;
                    if (list != npos && tokens[list].mutable_.wrap.list_kind != WrapListKind::None)
                        statement_comma = false;
                }
                if (statement_comma) {
                    int depth = 0;
                    for (size_t k = i; k < tokens.size(); ++k) {
                        if (!is_code_token(tokens[k])) continue;
                        const TK kk = tokens[k].lex.kind;
                        if (kk == TK::OpenParenthesis || kk == TK::OpenBracket || kk == TK::OpenBrace ||
                            kk == TK::ApostropheOpenBrace)
                            ++depth;
                        else if (kk == TK::CloseParenthesis || kk == TK::CloseBracket || kk == TK::CloseBrace)
                            --depth;
                        if (depth != 0) continue;
                        if (kk == TK::Colon) { statement_comma = false; break; }
                        if (kk == TK::Semicolon || kk == TK::Question || is_assignment_op(kk))
                            break;
                    }
                }
                const bool continues =
                    is_binary_op(pk) || is_assignment_op(pk) || pk == TK::Question || open_delim ||
                    prefix_only_op || statement_comma ||
                    prev_is_conditional_colon || event_or || leads_conditional_colon ||
                    (pk == TK::Comma && enclosing != npos) ||
                    (t.lex.comment_kind == CommentLexemeKind::None && is_binary_op(t.lex.kind) &&
                     !in_prefix_position(tokens, i)) ||
                    // `@(posedge clk` / `` `ifdef R `` / `or negedge rst_n` --
                    // a line led by the event list's `or` continues it, as
                    // one led by a binary operator does.
                    (kind_is(t, TK::OrKeyword) &&
                     (t.immutable.syntax.paren_depth > 0 || t.immutable.syntax.in_property_expr));
                if (continues && !list_boundary && !block_open && !case_header_end && !event_star &&
                    !p.lex.in_attribute_instance)
                    t.mutable_.wrap.continuation = true;
            }
            // Each branch of `` `ifdef `` continues from what preceded the
            // `` `ifdef ``, not from the end of the branch before it.
            if (t.lex.is_directive) {
                using SK = slang::syntax::SyntaxKind;
                switch (t.lex.directive_kind) {
                case SK::IfDefDirective:
                case SK::IfNDefDirective:
                    branches.push_back({prev, opens, questions, prev_is_conditional_colon});
                    break;
                case SK::ElsIfDirective:
                case SK::ElseDirective:
                    if (!branches.empty()) {
                        prev = branches.back().prev;
                        opens = branches.back().opens;
                        questions = branches.back().questions;
                        prev_is_conditional_colon = branches.back().conditional_colon;
                    }
                    break;
                case SK::EndIfDirective:
                    if (!branches.empty())
                        branches.pop_back();
                    break;
                default:
                    break;
                }
            }
            if (!is_code_token(t))
                continue;
            const TK k = t.lex.kind;
            prev_is_conditional_colon = false;
            if (k == TK::OpenParenthesis || k == TK::OpenBracket || k == TK::OpenBrace ||
                k == TK::ApostropheOpenBrace) {
                opens.push_back(i);
                questions.push_back(0);
            } else if ((k == TK::CloseParenthesis || k == TK::CloseBracket || k == TK::CloseBrace) &&
                       !opens.empty()) {
                opens.pop_back();
                questions.pop_back();
            } else if (k == TK::Question && !t.lex.continues_vector_literal) {
                ++questions.back();
            } else if (k == TK::Colon && questions.back() > 0) {
                --questions.back();
                prev_is_conditional_colon = true;
            } else if (k == TK::Semicolon && opens.empty()) {
                questions.back() = 0;
            }
            prev = i;
        }
        // `b - c` / `// why` / `+ d;` -- an own-line comment inside a
        // continued expression belongs with the line it sits above.  A
        // comment is no operand, so the rules above never mark it, and it
        // was left at the statement's indent between two indented lines.
        for (size_t i = 0; i < tokens.size(); ++i) {
            Tok& t = tokens[i];
            if (is_passthrough(t) || t.lex.comment_kind == CommentLexemeKind::None ||
                t.immutable.comment.role != CommentRole::OwnLine)
                continue;
            const size_t nx = next_code(tokens, i + 1, tokens.size());
            if (nx != npos && tokens[nx].mutable_.wrap.continuation)
                t.mutable_.wrap.continuation = true;
        }
    }

    static void apply_procedural_block_wrap(TokenStream& tokens) {
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_procedural_block_at(tokens, i))
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

            // A control's body starts its own line -- including a body that
            // is itself a `forever`/`do` (`if (c) do ... while (d);`).
            auto break_pending_body = [&](size_t at) {
                Tok& b = tokens[at];
                if (!single_stmt_pending || ctrl_just_closed || b.lex.comment_kind != CommentLexemeKind::None)
                    return;
                single_stmt_pending = false;
                const bool is_block = kind_is(b, TK::BeginKeyword) ||
                                      is_fork_block_open(tokens, at) ||
                                      kind_is(b, TK::OpenBrace);
                const bool closes = is_close_block(b.lex.kind) || is_outer_close(b.lex.kind) ||
                                    b.mutable_.macro.closes_indent_scope;
                if (!is_block && !closes)
                    b.mutable_.wrap.must_break_before = true;
            };

            if (kind_is(t, TK::ForeverKeyword) || kind_is(t, TK::DoKeyword)) {
                break_pending_body(i);
                size_t body = next_code(tokens, i + 1, tokens.size());
                if (body != npos &&
                    !kind_is(tokens[body], TK::BeginKeyword) &&
                    !kind_is(tokens[body], TK::ForkKeyword) &&
                    !kind_is(tokens[body], TK::OpenBrace))
                    tokens[body].mutable_.wrap.must_break_before = true;
                continue;
            }

            if (kind_is(t, TK::ElseKeyword) && !is_property_operator_keyword(t)) {
                size_t body = next_code(tokens, i + 1, tokens.size());
                if (body != npos &&
                    !kind_is(tokens[body], TK::BeginKeyword) &&
                    !kind_is(tokens[body], TK::ForkKeyword) &&
                    !kind_is(tokens[body], TK::OpenBrace) &&
                    !kind_is(tokens[body], TK::IfKeyword))
                    tokens[body].mutable_.wrap.must_break_before = true;
                continue;
            }

            if (is_single_stmt_control(t) && !is_property_operator_keyword(t))
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
                    do_while_tail = control != npos && tokens[control].immutable.topology.ends_do_while;
                }
                single_stmt_pending = !do_while_tail;
                ctrl_just_closed = true;
            }

            break_pending_body(i);
            ctrl_just_closed = false;
        }
    }

    // A based literal's value pieces must stay adjacent (see
    // LexemeFacts::continues_vector_literal); a line break is trivia too.
    static void freeze_vector_literals(TokenStream& tokens) {
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (!tokens[i].lex.continues_vector_literal)
                continue;
            tokens[i].mutable_.wrap.must_break_before = false;
            tokens[i].mutable_.wrap.can_break_before = false;
            tokens[i - 1].mutable_.wrap.must_break_after = false;
            tokens[i - 1].mutable_.wrap.can_break_after = false;
        }
    }

    // An attribute instance is an atom: `(* async_reg = "true" *)` annotates the
    // declaration that follows and is not a breakable list, however much its
    // OpenParenthesis looks like one.  Breaking it apart moves only trivia, so
    // the token-stream safety net cannot catch the damage -- and the result no
    // longer parses.  Clearing the flags here, after every rule above has run,
    // keeps the decision in one place instead of adding a guard to each.
    static void freeze_attribute_instances(TokenStream& tokens) {
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!tokens[i].lex.in_attribute_instance)
                continue;
            size_t end = i;
            while (end + 1 < tokens.size() && tokens[end + 1].lex.in_attribute_instance)
                ++end;

            // A break before the attribute and after it stays available; only
            // the inside of the span is frozen.
            for (size_t k = i; k <= end; ++k) {
                if (k != end) {
                    tokens[k].mutable_.wrap.must_break_after = false;
                    tokens[k].mutable_.wrap.can_break_after = false;
                }
                if (k != i)
                    tokens[k].mutable_.wrap.must_break_before = false;
            }
            // An attribute on a design unit has a line of its own, as do the
            // others stacked with it: `(* keep_hierarchy *)` above `module`.
            size_t n = next_code(tokens, end + 1, tokens.size());
            while (n != npos && tokens[n].lex.in_attribute_instance)
                n = next_code(tokens, n + 1, tokens.size());
            if (n != npos && tokens[n].immutable.topology.opens_design_unit)
                tokens[end].mutable_.wrap.must_break_after = true;
            i = end;
        }
    }

    const FormatOptions& opts_;
};

//   always_comb
//   `ifdef FAST
//     x = 1;
//   `else
//     x = 2;
//   `endif
//
// A body that opens with a conditional directive is one statement per branch:
// each branch fills the same slot.  When the body ending at `end` is followed
// by that conditional's `` `else ``/`` `elsif ``, it runs on to the last code
// token before the matching `` `endif ``.  Otherwise `end` stands.
inline size_t extend_over_conditional_branches(const TokenStream& tokens, size_t body, size_t end) {
    using SK = slang::syntax::SyntaxKind;
    auto opens = [&](size_t i) {
        return tokens[i].lex.is_directive && (tokens[i].lex.directive_kind == SK::IfDefDirective ||
                                              tokens[i].lex.directive_kind == SK::IfNDefDirective);
    };
    auto closes = [&](size_t i) {
        return tokens[i].lex.is_directive && tokens[i].lex.directive_kind == SK::EndIfDirective;
    };
    auto branches = [&](size_t i) {
        return tokens[i].lex.is_directive && (tokens[i].lex.directive_kind == SK::ElseDirective ||
                                              tokens[i].lex.directive_kind == SK::ElsIfDirective);
    };
    // Conditionals opened between the control and its body.
    int depth = 0;
    const size_t before = prev_code(tokens, body);
    for (size_t i = before == npos ? 0 : before + 1; i < body; ++i) {
        if (opens(i))
            ++depth;
        else if (closes(i) && depth > 0)
            --depth;
    }
    if (depth == 0 || end == npos)
        return end;
    size_t k = end + 1;
    while (k < tokens.size() && !is_code_token(tokens[k]) && !tokens[k].lex.is_directive)
        ++k;
    if (k >= tokens.size() || !branches(k))
        return end;
    int nest = 0;
    size_t last = end;
    for (; k < tokens.size(); ++k) {
        if (opens(k)) {
            ++nest;
        } else if (closes(k)) {
            if (nest == 0)
                return last;
            --nest;
        } else if (is_code_token(tokens[k])) {
            last = k;
        }
    }
    return end;
}

// body start -> body end for every control body that is not a block: the
// statement an `always`/`initial`/`final`, an `if`/`for`/`foreach`/`while`/
// `repeat`, an `else` or a `forever` controls.  One token can start several
// (`always if ...` starts the `always` body; the `if` body starts later), so
// each start maps to a list.  A body that is itself a block (`begin`, `fork`,
// a brace block) is left out -- the block indents its own contents.
inline std::unordered_map<size_t, std::vector<size_t>> controlled_body_extents(const TokenStream& tokens) {
    std::unordered_map<size_t, std::vector<size_t>> out;
    auto add = [&](size_t body, size_t end) {
        if (body == npos || body >= tokens.size())
            return;
        // `(* full_case *) case (x) ... endcase` -- the attribute leads the
        // body and indents with it, but the statement it annotates decides
        // what kind of body this is and where it ends.  Measured from the
        // attribute's `(`, the body ended at the first `;` and `endcase` (or
        // an `else`) fell back to the control's column.
        size_t stmt = body;
        while (stmt != npos && tokens[stmt].lex.in_attribute_instance)
            stmt = next_code(tokens, stmt + 1, tokens.size());
        if (stmt == npos)
            return;
        if (stmt != body)
            end = simple_statement_end_from(tokens, stmt);
        if (end == npos || end < body)
            return;
        const Tok& b = tokens[stmt];
        if (kind_is(b, TK::BeginKeyword) || is_fork_block_open(tokens, body) ||
            kind_is(b, TK::OpenBrace))
            return;
        if (closes_indent_scope_at(tokens, body) || is_outer_close(b.lex.kind) ||
            b.mutable_.macro.closes_indent_scope)
            return;
        const size_t extent_end = extend_over_conditional_branches(tokens, body, end);
        // `if (a)` / `// c` / `x = 1;` -- an own-line comment between a
        // control and its body belongs to the body and takes its level.
        size_t start = body;
        while (start > 0 && tokens[start - 1].lex.comment_kind != CommentLexemeKind::None &&
               tokens[start - 1].immutable.comment.role == CommentRole::OwnLine &&
               !is_passthrough(tokens[start - 1]))
            --start;
        out[start].push_back(extent_end);
    };
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!is_code_token(tokens[i]) || is_property_operator_keyword(tokens[i]))
            continue;
        const TK k = tokens[i].lex.kind;
        if (is_procedural_block_at(tokens, i)) {
            const size_t body = procedural_body_start(tokens, i);
            if (body == npos)
                continue;
            size_t end = simple_statement_end_from(tokens, body);
            if (end == npos)
                end = tokens[i].immutable.syntax.stmt_end;
            add(body, end);
        } else if (k == TK::ElseKeyword) {
            // `else if` is indented by the nested `if` itself.
            const size_t body = next_code(tokens, i + 1, tokens.size());
            if (body != npos && !kind_is(tokens[body], TK::IfKeyword))
                add(body, simple_statement_end_from(tokens, body));
        } else if (k == TK::ForeverKeyword) {
            add(next_code(tokens, i + 1, tokens.size()), simple_statement_end_from(tokens, i));
        } else if (k == TK::DoKeyword) {
            const size_t body = next_code(tokens, i + 1, tokens.size());
            add(body, simple_statement_end_from(tokens, body));
        } else if (tokens[i].immutable.topology.is_case_item_colon &&
                   !tokens[i].immutable.syntax.in_property_expr) {
            // A case item's statement sits one level inside its label, as
            // the contents of `A: begin ... end` do: `A:` / `// c` / `x = 1;`
            // indents `x`, and `A: if (c) x = 1; else x = 2;` puts `else` at
            // the statement's level rather than the label's.
            const size_t body = next_code(tokens, i + 1, tokens.size());
            if (body != npos && !kind_is(tokens[body], TK::Semicolon))
                add(body, simple_statement_end_from(tokens, body));
        } else if (is_single_stmt_control(tokens[i])) {
            // The `while` closing a do-while controls nothing.
            if (tokens[i].immutable.topology.ends_do_while)
                continue;
            const size_t body = single_statement_control_body_start(tokens, i);
            if (body != npos)
                add(body, simple_statement_end_from(tokens, body));
        }
    }
    return out;
}

// IndentPass owns IndentMetadata and reads wrap/source facts.  The level is a
// deterministic stack over token kinds, so format(format(x)) recomputes the same
// levels from canonical output.
class IndentPass final : public IFormatPass {
public:
    explicit IndentPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "indent"; }
    void run(TokenStream& tokens) override {
        int level = 0;
        // Prototypes (`extern`/`pure virtual`/DPI `function`, `typedef class`)
        // open no scope: opens_indent_scope_at() reads
        // TopologyFacts::is_prototype.

        // Every non-block body of a control -- `always`/`initial`/`final`,
        // `if`/`for`/`foreach`/`while`/`repeat`, `else`, `forever` -- is one
        // level deeper than its control until the statement ends.  Bodies
        // nest (`for (...) if (...) return i;`), so the levels are a stack:
        // a single flag here once let the outer body's level leak for the
        // rest of the file.
        const auto body_extents = controlled_body_extents(tokens);
        std::vector<size_t> open_bodies; // body end token, innermost last

        // A design unit restores the level it started at, so a level leaked
        // inside one can never shift the next unit.
        // A nested unit (`module outer; module inner; ... endmodule`, IEEE
        // 1800 23.4) closes at its opener's column, so the opener is kept.
        struct OuterUnit { int level; size_t open_bodies; size_t opener; };
        std::vector<OuterUnit> outer_units;
        // `` `ifdef A module m (input a); `else module m (input b); `endif ``
        // -- alternative headers of one unit.  A branch that opened a unit
        // leaves the next branch where the `` `ifdef `` found it, or the
        // second header would read as a unit nested in the first.
        struct Branch { int level; size_t outer_units; size_t open_bodies; int scope_net; };
        std::vector<Branch> branches;
        // Indent scopes opened minus scopes closed so far; a branch's own
        // share is the difference from the value its `` `ifdef `` saved.
        int scope_net = 0;

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (is_passthrough(t)) continue;

            if (is_conditional_preprocessor_directive(t)) {
                using SK = slang::syntax::SyntaxKind;
                switch (t.lex.directive_kind) {
                case SK::IfDefDirective:
                case SK::IfNDefDirective:
                    branches.push_back({level, outer_units.size(), open_bodies.size(), scope_net});
                    break;
                case SK::ElsIfDirective:
                case SK::ElseDirective:
                    if (!branches.empty() && outer_units.size() > branches.back().outer_units) {
                        level = branches.back().level;
                        outer_units.resize(branches.back().outer_units);
                        open_bodies.resize(std::min(open_bodies.size(), branches.back().open_bodies));
                        scope_net = branches.back().scope_net;
                    } else if (!branches.empty()) {
                        // Likewise a branch that opened a block:
                        // `` `ifdef A always_ff @(posedge clk) begin `else
                        // always_ff @(posedge clk or negedge rst_n) begin
                        // `endif `` is one `begin`, closed by one `end`.
                        // The scopes this branch opened (or closed) are
                        // undone, so every branch starts where the first
                        // did and the code after `` `endif `` continues
                        // from the last.  Controlled bodies are not scopes
                        // and keep their own extent across the branches.
                        level = std::max(0, level - (scope_net - branches.back().scope_net));
                        scope_net = branches.back().scope_net;
                    }
                    break;
                case SK::EndIfDirective:
                    if (!branches.empty())
                        branches.pop_back();
                    break;
                default:
                    break;
                }
            }

            // Compute indent — close tokens first so they dedent before assignment
            bool closes = closes_indent_scope_at(tokens, i) || is_outer_close(t.lex.kind) ||
                          t.mutable_.macro.closes_indent_scope;
            if (closes) {
                level = std::max(0, level - 1);
                --scope_net;
            }
            size_t closed_unit_opener = npos;
            if (is_outer_close(t.lex.kind) && !outer_units.empty()) {
                level = outer_units.back().level;
                open_bodies.resize(std::min(open_bodies.size(), outer_units.back().open_bodies));
                closed_unit_opener = outer_units.back().opener;
                outer_units.pop_back();
            }

            if (auto it = body_extents.find(i); it != body_extents.end()) {
                for (size_t end : it->second) {
                    ++level;
                    open_bodies.push_back(end);
                }
            }

            t.mutable_.indent.base_indent = level * opts_.indent_size;
            if (size_t hdr = module_header_import_owner(tokens, i); hdr != npos)
                t.mutable_.indent.base_indent =
                    tokens[hdr].mutable_.indent.base_indent + opts_.indent_size;
            if (size_t hdr = module_header_parameter_hash_owner(tokens, i); hdr != npos)
                t.mutable_.indent.base_indent = tokens[hdr].mutable_.indent.base_indent;
            // The port list's `(` -- on a line of its own after a header
            // `import` -- sits with its header, as a `#(` there does.
            if (t.immutable.topology.starts_port_list)
                if (size_t hdr = find_header_keyword_before(tokens, i); hdr != npos)
                    t.mutable_.indent.base_indent = tokens[hdr].mutable_.indent.base_indent;
            const bool nested_unit = t.immutable.topology.opens_design_unit && !outer_units.empty();
            if (is_outer_close(t.lex.kind))
                t.mutable_.indent.base_indent = closed_unit_opener == npos
                    ? 0 : tokens[closed_unit_opener].mutable_.indent.base_indent;
            else if (t.immutable.topology.opens_design_unit && !nested_unit &&
                     opts_.default_indent_level_inside_outmost_block == 0)
                t.mutable_.indent.base_indent = 0; // OuterOpen itself is at outer level
            if (is_conditional_preprocessor_directive(t))
                t.mutable_.indent.base_indent = 0;
            if (t.mutable_.wrap.continuation)
                t.mutable_.indent.continuation_indent = opts_.indent_size;

            // Open scope after assigning indent to the opener token.
            // Suppress for function/task used as qualifiers in import/extern
            // declarations, and for class used in typedef forward declarations.
            const bool design_unit = t.immutable.topology.opens_design_unit;
            if (design_unit)
                outer_units.push_back({level, open_bodies.size(), i});
            if (!closes) {
                // default_indent_level_inside_outmost_block is about the
                // outermost unit; a nested one always indents its items.
                if (opens_indent_scope_at(tokens, i) ||
                    (design_unit && (opts_.default_indent_level_inside_outmost_block > 0 || nested_unit)) ||
                    t.mutable_.macro.opens_indent_scope) {
                    ++level;
                    ++scope_net;
                }
            }

            // Several nested bodies can end on one token (`for (..) if (..) x;`).
            while (!open_bodies.empty() && open_bodies.back() <= i) {
                level = std::max(0, level - 1);
                open_bodies.pop_back();
            }
        }

        auto line_start_of = [&](size_t idx) {
            size_t s = 0;
            for (size_t n = idx; n > 0; --n) {
                size_t i = n - 1;
                if (tokens[n].mutable_.wrap.must_break_before || tokens[i].mutable_.wrap.must_break_after ||
                    ends_verbatim_line(tokens[i])) {
                    s = n;
                    break;
                }
            }
            return s;
        };
        auto column_before = [&](size_t idx) {
            size_t s = line_start_of(idx);
            int base = tokens[s].mutable_.indent.base_indent +
                       tokens[s].mutable_.indent.continuation_indent;
            int col = base;

            // Indentation is now intentionally downstream of spacing.  The
            // old implementation used compact_width(), which reimplemented a
            // small subset of spacing policy and therefore drifted from the
            // actual renderer.  For example, compact_width() counted the line
            // prefix below as if it contained a space after unary `!`:
            //
            //   if (! uvm_config_db #(T)::get(
            //
            // while SpacingPass and the renderer correctly emit:
            //
            //   if (!uvm_config_db #(T)::get(
            //
            // That one phantom space made hanging-call continuation arguments
            // start one column too far right.  Use the already-computed
            // SpaceMetadata here so "column before token" means the same thing
            // to IndentPass as it later means to render_tokens().  That
            // includes an inline `/* c */` and a directive: the renderer
            // prints them, so they occupy columns like any other token.
            for (size_t k = s; k < idx && k < tokens.size(); ++k) {
                const Tok& tok = tokens[k];
                if (is_passthrough(tok)) {
                    const std::string_view text(tok.lex.text);
                    const size_t nl = last_newline_offset(text);
                    col = nl == std::string::npos ? col + static_cast<int>(text.size())
                                                  : static_cast<int>(text.size() - nl - 1);
                    continue;
                }
                if (k != s) {
                    col += tok.mutable_.space.suppress_space
                        ? 0
                        : tok.mutable_.space.spaces_before;
                }
                // A block comment spanning lines leaves the column after its
                // last line break.
                const size_t nl = last_newline_offset(tok.lex.text);
                col = nl == std::string::npos ? col + token_width(tok)
                                              : static_cast<int>(tok.lex.text.size() - nl - 1);
            }
            if (idx != s && idx < tokens.size() && is_code_token(tokens[idx])) {
                const Tok& tok = tokens[idx];
                col += tok.mutable_.space.suppress_space
                    ? 0
                    : tok.mutable_.space.spaces_before;
            }
            return col;
        };
        // `anchor` is the token the indent was measured from; AlignPass moves
        // the line by whatever alignment padding lands before it.
        auto set_indent = [&](size_t k, int indent, size_t anchor) {
            tokens[k].mutable_.indent.base_indent = std::max(0, indent);
            tokens[k].mutable_.indent.anchor_token = anchor;
        };
        auto set_item_indent = [&](size_t first, size_t last, int indent, size_t anchor) {
            if (first >= tokens.size()) return;
            set_indent(first, indent, anchor);
            for (size_t k = first + 1; k <= last && k < tokens.size(); ++k) {
                // Conditional directives stay at column 0 (see the main loop);
                // one nested inside an item must not take the item's indent.
                if (is_conditional_preprocessor_directive(tokens[k]))
                    continue;
                if (tokens[k].mutable_.wrap.must_break_before ||
                    (tokens[k].lex.comment_kind != CommentLexemeKind::None && tokens[k].immutable.comment.role == CommentRole::OwnLine))
                    set_indent(k, indent, anchor);
            }
        };

        for (size_t open = 0; open < tokens.size(); ++open) {
            WrapListKind kind = tokens[open].mutable_.wrap.list_kind;
            if (kind == WrapListKind::None || tokens[open].mutable_.wrap.list_open != open)
                continue;
            // A statement block (`constraint c { ... }`) is `;`-separated,
            // not a list: it opens an indent scope, and the main loop above
            // already indents its statements and their controlled bodies.
            // Forcing each top-level "item" to one indent flattened
            // `if (c) len < 4;` and `foreach (q[i]) q[i] > 0;`.
            if (kind == WrapListKind::BraceBlock && tokens[open].immutable.topology.opens_brace_block)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos || close >= tokens.size())
                continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty())
                continue;

            const size_t open_line = line_start_of(open);
            int base = tokens[open_line].mutable_.indent.base_indent +
                       tokens[open_line].mutable_.indent.continuation_indent;
            int item_indent = base + opts_.indent_size;
            int close_indent = base;
            size_t anchor = open_line;

            size_t name = prev_code(tokens, open);
            int name_col = (name == npos) ? base : column_before(name);
            // A hanging list's items line up with its first one, which sits
            // after whatever space the inside-paren options put after `(`.
            int after_open_col = column_before(open) + token_width(tokens[open]);
            if (const size_t first = next_code(tokens, open + 1, close);
                first != npos && !tokens[first].mutable_.wrap.must_break_before &&
                !tokens[open].mutable_.wrap.must_break_after &&
                !tokens[first].mutable_.space.suppress_space)
                after_open_col += tokens[first].mutable_.space.spaces_before;

            switch (kind) {
            case WrapListKind::FunctionBlock:
                // "One level from the call" is from where the callee starts,
                // not from its last name: `obj.sub.meth(`, `pkg::cls::fn(` and
                // `cfg_db #(T)::set(` pushed the arguments right by the
                // length of the prefix.
                while (name != npos) {
                    const size_t sep = prev_code(tokens, name);
                    if (sep == npos || !(kind_is(tokens[sep], TK::Dot) || kind_is(tokens[sep], TK::DoubleColon)))
                        break;
                    size_t q = prev_code(tokens, sep);
                    while (q != npos &&
                           (kind_is(tokens[q], TK::CloseParenthesis) || kind_is(tokens[q], TK::CloseBracket)) &&
                           tokens[q].immutable.syntax.matching_token != npos) {
                        q = prev_code(tokens, tokens[q].immutable.syntax.matching_token);
                        if (q != npos && kind_is(tokens[q], TK::Hash))
                            q = prev_code(tokens, q);
                    }
                    // A chain already broken (`).next(`) hangs from the line
                    // its `)` starts, which sits at the chain's own indent.
                    if (q != npos && line_start_of(q) != open_line) {
                        name = open_line;
                        name_col = column_before(name);
                        break;
                    }
                    if (q == npos ||
                        !(kind_is(tokens[q], TK::Identifier) || kind_is(tokens[q], TK::SystemIdentifier) ||
                          kind_is(tokens[q], TK::ThisKeyword) || kind_is(tokens[q], TK::SuperKeyword) ||
                          kind_is(tokens[q], TK::LocalKeyword) || kind_is(tokens[q], TK::UnitSystemName)))
                        break;
                    name = q;
                    name_col = column_before(name);
                }
                item_indent = name_col + opts_.indent_size;
                close_indent = name_col;
                if (name != npos) anchor = name;
                break;
            case WrapListKind::FunctionHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                anchor = open;
                break;
            case WrapListKind::ModuleParametersBlock:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::ModuleParametersHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                anchor = open;
                break;
            case WrapListKind::FunctionDeclBlock:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::FunctionDeclHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                anchor = open;
                break;
            case WrapListKind::InstancePorts:
                // Ports indent from the instantiated module's line, not from
                // wherever a hanging `#(...)` override list left the `(`.
                if (size_t inst = instance_name_before(tokens, open); inst != npos) {
                    size_t mod = prev_code(tokens, inst);
                    while (mod != npos && tokens[mod].lex.in_attribute_instance)
                        mod = prev_code(tokens, mod);
                    if (mod != npos && kind_is(tokens[mod], TK::CloseParenthesis) &&
                        tokens[mod].immutable.syntax.matching_token != npos) {
                        const size_t hash = prev_code(tokens, tokens[mod].immutable.syntax.matching_token);
                        if (hash != npos && kind_is(tokens[hash], TK::Hash))
                            mod = prev_code(tokens, hash);
                    }
                    if (mod != npos && is_identifier_like(tokens[mod])) {
                        anchor = line_start_of(mod);
                        base = tokens[anchor].mutable_.indent.base_indent;
                    }
                }
                item_indent = base + std::max(0, opts_.instance.port_indent_level) * opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::ModulePorts:
                if (size_t hdr = find_header_keyword_before(tokens, open); hdr != npos) {
                    base = tokens[hdr].mutable_.indent.base_indent;
                    anchor = npos;
                }
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
                set_item_indent(item.first, item.last, item_indent, anchor);
            // An empty item (`sub u (clk, a, , y);`) is only its comma, and
            // top_level_list_items() has no item for it; the comma still
            // starts a line of the list and takes the item indent.
            {
                int pd = 0, bd = 0, brd = 0;
                for (size_t k = open + 1; k < close; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    const TK kk = tokens[k].lex.kind;
                    if (kk == TK::OpenParenthesis) ++pd;
                    else if (kk == TK::CloseParenthesis && pd > 0) --pd;
                    else if (kk == TK::OpenBracket) ++bd;
                    else if (kk == TK::CloseBracket && bd > 0) --bd;
                    else if (kk == TK::OpenBrace || kk == TK::ApostropheOpenBrace) ++brd;
                    else if (kk == TK::CloseBrace && brd > 0) --brd;
                    else if (kk == TK::Comma && pd == 0 && bd == 0 && brd == 0 &&
                             (tokens[k].mutable_.wrap.must_break_before ||
                              tokens[k - 1].mutable_.wrap.must_break_after))
                        set_indent(k, item_indent, anchor);
                }
            }
            for (size_t k = open + 1; k < close; ++k) {
                if (tokens[k].lex.comment_kind != CommentLexemeKind::None &&
                    (tokens[k].immutable.comment.role == CommentRole::OwnLine ||
                     tokens[k].mutable_.wrap.must_break_before))
                    set_indent(k, item_indent, anchor);
                if ((kind == WrapListKind::ModuleParametersBlock ||
                     kind == WrapListKind::ModuleParametersHanging) &&
                    tokens[k].lex.is_directive &&
                    !is_conditional_preprocessor_directive(tokens[k]) &&
                    tokens[k].mutable_.wrap.must_break_before)
                    set_indent(k, item_indent, anchor);
            }
            if (tokens[close].mutable_.wrap.must_break_before)
                set_indent(close, close_indent, anchor);
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
                   kind_is(tokens[cur_first], TK::LocalParamKeyword) ||
                   kind_is(tokens[cur_first], TK::TypeKeyword)) &&
                  tokens[cur_first].immutable.syntax.paren_depth > 0)))
                ln.disabled = true;
            if (cur_first != npos && cur_first < inside_control_begin.size() &&
                inside_control_begin[cur_first])
                ln.disabled = true;
            // A line continuing an expression is part of the statement above.
            if (cur_first != npos && tokens[cur_first].mutable_.wrap.continuation)
                ln.disabled = true;
            // `import "DPI-C" c_name = function void f();` -- the `=` names
            // the C linkage; nothing is assigned.
            if (cur_first != npos && (kind_is(tokens[cur_first], TK::ImportKeyword) ||
                                      kind_is(tokens[cur_first], TK::ExportKeyword)))
                ln.disabled = true;
            // `parameter W = 8,` / `D = 4,` -- a later line of a wrapped
            // parameter or argument list is placed by the list's own layout;
            // its `=` is a default value, not a statement's assignment.
            // (A `)` closing the list carries its own depth and starts
            // whatever follows the list.)
            if (cur_first != npos && tokens[cur_first].immutable.syntax.paren_depth > 0 &&
                !kind_is(tokens[cur_first], TK::CloseParenthesis))
                ln.disabled = true;
            // `IDLE = 3'd0,` -- an enum member's `=` gives it a value.  Its
            // columns belong to `enum_declaration.align`.
            if (cur_first != npos) {
                const size_t list = tokens[cur_first].mutable_.wrap.list_open;
                if (list != npos && list < tokens.size() &&
                    tokens[list].mutable_.wrap.list_kind == WrapListKind::EnumBody)
                    ln.disabled = true;
            }
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
                    ln.lhs_prefix_width = rendered_width(tokens, scan_start, after_assign) + 1;
                    scan_start = after_assign;
                }
            }
            // `4'h12: y = 5;` -- the case label is a prefix of the line, like
            // `assign `, so the assignment's LHS field starts after it.
            for (size_t k = scan_start; k < end_idx; ++k) {
                if (!is_code_token(tokens[k]))
                    continue;
                if (tokens[k].immutable.topology.is_case_item_colon) {
                    const size_t after_label = next_code(tokens, k + 1, end_idx);
                    if (after_label != npos) {
                        ln.lhs_prefix_width = rendered_width(tokens, scan_start, after_label) + 1;
                        scan_start = after_label;
                    }
                    break;
                }
                if (kind_is(tokens[k], TK::Semicolon) || is_assignment_op(tokens[k].lex.kind))
                    break;
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
                ln.lhs_width = rendered_width(tokens, scan_start, ln.assign_idx);
                // Count identifiers at bracket depth 0 only: arr[b] has one
                // top-level identifier (arr), so it should not be treated like
                // a two-identifier user-defined-type declaration (packet_t v).
                // A member select names one variable too: `cb.data <= d` has
                // one top-level name, while `bus_if.master m = ...` still
                // has two.  A timing control names no variable either:
                // `#5 a = b`, `#dly a = b` and `@(posedge clk) a = b` all
                // align like `a = b`, the control counted in the LHS field.
                // A concatenation is one target however many names it
                // holds: `{p, q} = r` aligns like `x = r`.
                int identifiers_before_assign = 0;
                int ident_bd = 0, ident_pd = 0, ident_brd = 0;
                for (size_t k = scan_start; k < ln.assign_idx; ++k) {
                    if (is_passthrough(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenBracket)) ++ident_bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && ident_bd > 0) --ident_bd;
                    else if (kind_is(tokens[k], TK::OpenParenthesis)) ++ident_pd;
                    else if (kind_is(tokens[k], TK::CloseParenthesis) && ident_pd > 0) --ident_pd;
                    else if (kind_is(tokens[k], TK::OpenBrace) || kind_is(tokens[k], TK::ApostropheOpenBrace)) ++ident_brd;
                    else if (kind_is(tokens[k], TK::CloseBrace) && ident_brd > 0) --ident_brd;
                    if (ident_bd == 0 && ident_pd == 0 && ident_brd == 0 && is_code_token(tokens[k]) &&
                        is_identifier_like(tokens[k])) {
                        const size_t before = prev_code(tokens, k);
                        const bool member = before != npos && before >= scan_start && kind_is(tokens[before], TK::Dot);
                        const bool control = before != npos && before >= scan_start &&
                                             (kind_is(tokens[before], TK::Hash) || kind_is(tokens[before], TK::At) ||
                                              kind_is(tokens[before], TK::DoubleHash));
                        if (!member && !control)
                            ++identifiers_before_assign;
                    }
                }
                // `localparam my_t B = 2;` holds two names as well, but its
                // keyword already says what it is, and `localparam int B`
                // beside it is aligned; skipping it split the group.
                const bool parameter_decl = kind_is(tokens[scan_start], TK::ParameterKeyword) ||
                                            kind_is(tokens[scan_start], TK::LocalParamKeyword);
                if (identifiers_before_assign >= 2 && !parameter_decl)
                    ln.disabled = true;
            }
            lines.push_back(ln);
            cur_first = npos;
            cur_start = end_idx;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& tok = tokens[i];
            if (is_passthrough(tok)) continue;
            // A format-off region -- its body through the `verilog_format: on`
            // line -- ends its line.  It is passthrough and carries no break,
            // which folded the statement after it into the region's line.
            if (i > 0 && (tok.mutable_.wrap.must_break_before ||
                          tokens[i - 1].mutable_.wrap.must_break_after ||
                          tokens[i - 1].lex.is_disabled_region_body)) {
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
                    int group_prefix = 0;
                    for (size_t k = li; k < j; ++k) {
                        group_lhs = std::max(group_lhs, lines[k].lhs_width);
                        group_prefix = std::max(group_prefix, lines[k].lhs_prefix_width);
                    }
                    for (size_t k = li; k < j; ++k) {
                        // Adaptive keeps each line's own prefix and field;
                        // otherwise every operator in the group shares one
                        // column, whatever case label stands in front of it.
                        const int lhs_field = opts_.statement.align_adaptive
                            ? std::max(opts_.statement.lhs_min_width, lines[k].lhs_width)
                            : group_lhs;
                        const int prefix = opts_.statement.align_adaptive
                            ? lines[k].lhs_prefix_width
                            : group_prefix;
                        const int target = opts_.tab_align
                            ? snap_to_grid(prefix + lhs_field + op_gap, opts_.indent_size)
                            : prefix + lhs_field + op_gap;
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

        // Can this line open a variable declaration?  A leading type keyword
        // is not enough: `void'(...)` and `int'(x) ...` are casts,
        // `static function base #(T) create();` is a subroutine header, and
        // `int W = 8)` inside parentheses is a later line of a wrapped
        // parameter or argument list, which the list's own layout places.
        auto starts_variable_declaration = [&](const Line& ln) {
            if (ln.first == npos || ln.end <= ln.first)
                return false;
            if (tokens[ln.first].immutable.syntax.paren_depth > 0)
                return false;
            const size_t nx = next_code(tokens, ln.first + 1, ln.end);
            if (nx != npos && kind_is(tokens[nx], TK::Apostrophe))
                return false;
            for (size_t k = ln.first; k < ln.end; ++k)
                if (is_code_token(tokens[k]) &&
                    (kind_is(tokens[k], TK::FunctionKeyword) || kind_is(tokens[k], TK::TaskKeyword)))
                    return false;
            return true;
        };

        // What a variable declaration line is, for both aligners below: the
        // default-width one and the sectioned one.  They used to disagree --
        // the default-width path took only a leading type keyword, so
        // `req_t r;`, `pkg::cfg_t c;` and `rand int r;` were left unaligned
        // beside the `logic`/`int` lines they sit with.
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
            // Last token of the name: a macro call's `)` when the name is
            // `` `CAT(foo, _q) ``, else first_name itself.
            size_t name_end{npos};
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

        // A comma inside a net delay `wire #(1, 2) w1, w2;` or a
        // parameterized type `p #(A, B) h;` separates no declarators.
        auto first_top_level_delim = [&](size_t begin, size_t end) {
            int bd = 0, pd = 0, brd = 0;
            for (size_t k = begin; k < end; ++k) {
                if (!is_code_token(tokens[k])) continue;
                if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                else if (kind_is(tokens[k], TK::OpenParenthesis)) ++pd;
                else if (kind_is(tokens[k], TK::CloseParenthesis) && pd > 0) --pd;
                else if (kind_is(tokens[k], TK::OpenBrace) || kind_is(tokens[k], TK::ApostropheOpenBrace)) ++brd;
                else if (kind_is(tokens[k], TK::CloseBrace) && brd > 0) --brd;
                else if (bd == 0 && pd == 0 && brd == 0 && kind_is(tokens[k], TK::Comma))
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
            if (!starts_variable_declaration(ln))
                return false;
            size_t eq = npos;
            size_t semi = find_statement_semicolon(ln, eq);
            if (semi == npos)
                return false;

            // `tri0 t;`, `genvar g;`, `local int b;`, `virtual my_if vif;`.
            // These are accepted here and not in is_var_decl_leading_keyword,
            // which also feeds spacing: `virtual` and `local` lead a method
            // as readily as a property, and starts_variable_declaration has
            // already turned away a line holding `function`/`task`.
            const TK lead = tokens[ln.first].lex.kind;
            bool plausible_start = is_var_decl_leading_keyword(lead) ||
                                   lead == TK::VarKeyword || is_net_type_keyword(lead) ||
                                   lead == TK::GenVarKeyword || lead == TK::LocalKeyword ||
                                   lead == TK::ProtectedKeyword;
            if (lead == TK::VirtualKeyword) {
                // `virtual class base;` declares a class, not a handle.
                const size_t nx = next_code(tokens, ln.first + 1, semi);
                plausible_start = nx != npos && !kind_is(tokens[nx], TK::ClassKeyword);
            }
            // Last token of a leading type name: `pkg::cfg_t cfg;` -- a
            // scoped type name is one type, and the declarator follows it.
            size_t type_last = ln.first;
            if (!plausible_start && is_identifier_like(tokens[ln.first])) {
                for (;;) {
                    const size_t sep = next_code(tokens, type_last + 1, semi);
                    // `mailbox #(item_c) mbx;` -- the parameter assignment is
                    // part of the type, and `base_c #(int)::this_t h;` goes on
                    // from it.
                    if (sep != npos && kind_is(tokens[sep], TK::Hash)) {
                        const size_t open = next_code(tokens, sep + 1, semi);
                        const size_t close = open != npos && kind_is(tokens[open], TK::OpenParenthesis)
                                                 ? tokens[open].immutable.syntax.matching_token
                                                 : npos;
                        if (close == npos || close >= semi)
                            break;
                        type_last = close;
                        continue;
                    }
                    // `my_if.mp ifp;` -- an interface with its modport is
                    // one type.  Only with a declarator right after it: a
                    // member select never has a name following.
                    if (sep != npos && type_last == ln.first && kind_is(tokens[sep], TK::Dot)) {
                        const size_t modport = next_code(tokens, sep + 1, semi);
                        const size_t declarator =
                            modport == npos ? npos : next_code(tokens, modport + 1, semi);
                        if (modport == npos || declarator == npos ||
                            !kind_is(tokens[modport], TK::Identifier) ||
                            !kind_is(tokens[declarator], TK::Identifier))
                            break;
                        type_last = modport;
                        break;
                    }
                    if (sep == npos || !kind_is(tokens[sep], TK::DoubleColon))
                        break;
                    const size_t part = next_code(tokens, sep + 1, semi);
                    if (part == npos || !is_identifier_like(tokens[part]))
                        break;
                    type_last = part;
                }
                size_t nx = next_code(tokens, type_last + 1, semi);
                plausible_start = nx != npos &&
                    (is_identifier_like(tokens[nx]) || kind_is(tokens[nx], TK::OpenBracket));
            }
            if (!plausible_start)
                return false;
            // `arr[i] <= d;` / `cnt[i] += 1;` -- a declaration's only
            // top-level assignment is its initializer `=`; any other
            // assignment operator before it makes the line a statement,
            // or `arr` reads as the type, `[i]` as its packed dimension
            // and `d` as the declarator.
            {
                int pd = 0, bd = 0, brd = 0;
                for (size_t k = ln.first; k < (eq == npos ? semi : eq); ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenParenthesis)) ++pd;
                    else if (kind_is(tokens[k], TK::CloseParenthesis) && pd > 0) --pd;
                    else if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (kind_is(tokens[k], TK::OpenBrace) || kind_is(tokens[k], TK::ApostropheOpenBrace)) ++brd;
                    else if (kind_is(tokens[k], TK::CloseBrace) && brd > 0) --brd;
                    else if (pd == 0 && bd == 0 && brd == 0 && is_assignment_op(tokens[k].lex.kind))
                        return false;
                    // `data[0] < data[1];` in a constraint, `a[0] ##1 b[1];`
                    // in a sequence -- an expression statement that starts
                    // with a select reads as type, dimension and declarator
                    // just as the assignments above do.
                    else if (pd == 0 && bd == 0 && brd == 0 && is_expression_operator(tokens[k].lex.kind))
                        return false;
                    // `x[0]: y = 1;` -- a case label with a select.  No
                    // declaration holds a case item's colon; without this
                    // `x` reads as the type and `[0]` as its dimension.
                    if (tokens[k].immutable.topology.is_case_item_colon)
                        return false;
                }
            }

            size_t first_delim = first_top_level_delim(ln.first + 1, eq == npos ? semi : eq);
            size_t first_name = previous_decl_name(ln.first + 1, first_delim);
            if (first_name == npos || first_name <= type_last)
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
            // A macro call named as the declarator (`logic `CAT(foo, _q);`)
            // is one unit; its argument list is not an instance's ports.
            // `sub u_c [3:0] (...)` is an instance array.
            const size_t name_end = kind_is(tokens[first_name], TK::MacroUsage)
                                        ? macro_invocation_end(tokens, first_name)
                                        : first_name;
            if (name_end >= semi)
                return false;
            size_t after_name = next_code_past_dimensions(tokens, name_end);
            if (after_name != npos && after_name < semi &&
                kind_is(tokens[after_name], TK::OpenParenthesis))
                return false;

            size_t packed_dim = npos;
            for (size_t k = type_last + 1; k < first_name; ++k) {
                if (kind_is(tokens[k], TK::OpenBracket)) {
                    packed_dim = k;
                    break;
                }
            }
            // If first_name sits inside the packed_dim brackets, this is a
            // subscript LHS (e.g. arr[p.value] = 3;), not a var declaration.
            if (packed_dim != npos) {
                size_t close_dim = tokens[packed_dim].immutable.syntax.matching_token;
                if (close_dim != npos && first_name > packed_dim && first_name < close_dim)
                    return false;
            }
            out = {ln.first, ln.end, semi, eq, first_name, first_delim,
                   packed_dim, packed_dim != npos, ln.indent, name_end};
            return true;
        };

        // `(* keep *) logic dbg;` -- an attribute annotates the declaration
        // and is no part of it.  The declaration aligners read each line from
        // the first token after it, so the line joins the columns of the
        // declarations around it instead of being passed over.
        std::vector<Line> decl_lines = lines;
        for (Line& ln : decl_lines) {
            while (ln.first != npos && ln.first < ln.end && tokens[ln.first].lex.in_attribute_instance) {
                const size_t next = next_code(tokens, ln.first + 1, ln.end);
                if (next == npos)
                    break;
                ln.first = next;
            }
        }

        if (opts_.var_declaration.align) {
            int declaration_line_count = 0;
            // The width of the type column a line asks for.  A line led by a
            // type keyword or a port direction asks for that one token, as it
            // always has; a line led by a user type or a qualifier asks for
            // everything ahead of its packed dimension or name, because there
            // `rand bit` and `pkg::cfg_t` are the type and not just `rand`.
            // Zero means the line is not a declaration this aligner takes.
            auto default_type_width = [&](const Line& ln) {
                if (ln.first == npos)
                    return 0;
                if (is_type_keyword(tokens[ln.first].lex.kind) ||
                    is_port_direction(tokens[ln.first].lex.kind))
                    return token_width(tokens[ln.first]);
                VarLine v;
                if (!is_var_decl_line(ln, v))
                    return 0;
                const size_t stop = v.packed_dim != npos ? v.packed_dim : v.first_name;
                int width = 0;
                size_t prev = npos;
                for (size_t k = ln.first; k < stop && k < ln.end; ++k) {
                    if (!is_code_token(tokens[k]))
                        continue;
                    const bool tight = kind_is(tokens[k], TK::DoubleColon) ||
                                       kind_is(tokens[k], TK::OpenParenthesis) ||
                                       kind_is(tokens[k], TK::CloseParenthesis) ||
                                       kind_is(tokens[k], TK::Comma) ||
                                       (prev != npos && (kind_is(tokens[prev], TK::DoubleColon) ||
                                                         kind_is(tokens[prev], TK::OpenParenthesis)));
                    if (prev != npos && !tight)
                        ++width;
                    width += token_width(tokens[k]);
                    prev = k;
                }
                return width;
            };
            // The keyword lines share one width across the file, as they always
            // have.  A wider user type widens only the run of declarations it
            // sits in: one `virtual bus_if #(.W(16)).master vif;` in a class
            // should not move every `logic` in the file.
            std::vector<int> type_width(decl_lines.size(), 0);
            int keyword_width = 0;
            for (size_t i = 0; i < decl_lines.size(); ++i) {
                type_width[i] = default_type_width(decl_lines[i]);
                if (type_width[i] == 0)
                    continue;
                ++declaration_line_count;
                const TK lead = tokens[decl_lines[i].first].lex.kind;
                if (is_type_keyword(lead) || is_port_direction(lead))
                    keyword_width = std::max(keyword_width, type_width[i]);
            }
            std::vector<int> run_width(decl_lines.size(), keyword_width);
            for (size_t i = 0; i < decl_lines.size();) {
                if (type_width[i] == 0) {
                    ++i;
                    continue;
                }
                size_t j = i;
                int widest = keyword_width;
                while (j < decl_lines.size() && type_width[j] > 0 && decl_lines[j].indent == decl_lines[i].indent)
                    widest = std::max(widest, type_width[j++]);
                for (size_t k = i; k < j; ++k)
                    run_width[k] = widest;
                i = j;
            }
            for (size_t line_index = 0; line_index < decl_lines.size(); ++line_index) {
                const Line& ln = decl_lines[line_index];
                if (ln.first == npos || ln.end <= ln.first)
                    continue;
                size_t first = ln.first;
                if (type_width[line_index] == 0)
                    continue;
                const int declaration_keyword_width = run_width[line_index];
                // `input v;` in a clocking block is a clocking signal, not a
                // port; the port columns (section widths) do not apply.
                if (tokens[first].immutable.syntax.in_clocking_block)
                    continue;
                // `input a, b;` in a non-ANSI body is a port declaration.  Its
                // columns are port_declaration's, and the branch below only
                // seeds them for that aligner to finish; with it off the line
                // was left half placed -- `input       a,  b          ;`.
                if (is_port_direction(tokens[first].lex.kind) && !opts_.port_declaration.align)
                    continue;
                if (!starts_variable_declaration(ln))
                    continue;

                size_t semi = npos;
                size_t eq = npos;
                size_t comma = npos;
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
                        if (comma == npos && kind_is(tokens[k], TK::Comma))
                            comma = k;
                        if (kind_is(tokens[k], TK::Semicolon)) {
                            semi = k;
                            break;
                        }
                    }
                }
                if (semi == npos)
                    continue;

                // The column belongs to the first declarator: `logic a, b;`
                // aligns `a`, and `b` follows its comma.
                size_t name = npos;
                // Port-direction lines keep their own column rules below.
                if (is_port_direction(tokens[first].lex.kind))
                    comma = npos;
                size_t name_limit = std::min(eq == npos ? semi : eq, comma == npos ? semi : comma);
                for (size_t n = name_limit; n > first + 1; --n) {
                    size_t k = n - 1;
                    if (!is_code_token(tokens[k]))
                        continue;
                    // `d [N]` -- an unpacked dimension trails the name; an
                    // identifier inside it is not the declarator.
                    if (kind_is(tokens[k], TK::CloseBracket)) {
                        const size_t open = tokens[k].immutable.syntax.matching_token;
                        if (open == npos || open <= first)
                            break;
                        n = open + 1;
                        continue;
                    }
                    // `logic [3:0] `CAT(foo, _q);` -- the macro call is the
                    // name; nothing inside its arguments is a column.
                    if (kind_is(tokens[k], TK::CloseParenthesis)) {
                        const size_t open = tokens[k].immutable.syntax.matching_token;
                        if (open == npos || open <= first)
                            break;
                        const size_t macro = prev_code(tokens, open);
                        if (macro != npos && macro > first && kind_is(tokens[macro], TK::MacroUsage)) {
                            name = macro;
                            break;
                        }
                        n = open + 1;
                        continue;
                    }
                    if (is_identifier_like(tokens[k])) {
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
                        int type_width = rendered_width(tokens, type_first, name);
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
                } else if (dim == npos && declaration_line_count >= 2) {
                    // The same name column as a line with a packed dimension,
                    // measured from the widest keyword rather than this
                    // line's own, so `int a;` and `logic b;` line up.
                    name_target = base + declaration_keyword_width + 1 + section2;
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
                        int rhs_width = rendered_width(tokens, eq + 1, semi);
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
                                   : (base + rendered_width(tokens, first, semi) + section3 - 1);
                }
            }
        }

        if (opts_.var_declaration.align && opts_.var_declaration.section1_min_width > 0) {

            std::vector<VarLine> vlines(decl_lines.size());
            std::vector<bool> is_vline(decl_lines.size(), false);
            for (size_t i = 0; i < decl_lines.size(); ++i)
                is_vline[i] = is_var_decl_line(decl_lines[i], vlines[i]);

            for (size_t i = 0; i < decl_lines.size();) {
                if (!is_vline[i]) {
                    ++i;
                    continue;
                }
                size_t j = i;
                int indent = vlines[i].indent;
                int group_type_width = 0;
                int group_packed_width = 0;
                int group_name_width = 0;
                int group_trailing_width = 0;
                while (j < decl_lines.size() &&
                       (is_vline[j] ||
                        (decl_lines[j].first != npos &&
                         tokens[decl_lines[j].first].lex.comment_kind != CommentLexemeKind::None)) &&
                       (!is_vline[j] || vlines[j].indent == indent)) {
                    // Comment-only line inside a declaration group: skip it without
                    // breaking the group so section2 (packed-dim column) remains
                    // consistent across declarations separated by comments.
                    if (!is_vline[j]) { ++j; continue; }
                    group_type_width = std::max(
                        group_type_width,
                        rendered_width(tokens, vlines[j].first,
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
                                                rendered_width(tokens, vlines[j].first_name,
                                                               vlines[j].name_end + 1));

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
                    for (size_t k = vlines[j].name_end + 1; k < vlines[j].first_delim; ++k) {
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
                    if (!is_vline[li]) continue; // comment line, no alignment tokens
                    const auto& vl = vlines[li];
                    for (size_t k = vl.first; k <= vl.semi && k < tokens.size(); ++k) {
                        tokens[k].mutable_.align.enabled = false;
                        tokens[k].mutable_.align.target_column = -1;
                    }

                    // Preferred section boundaries are absolute columns for
                    // this declaration group.  Later boundaries are not chained
                    // from locally widened earlier boundaries; instead, each
                    // boundary first tries to use its preferred column and only
                    // moves right when that specific line's actual text would
                    // overlap it.  This keeps long section2 text from pushing
                    // an otherwise empty section4/semicolon column.
                    const int preferred_dim_col = vl.indent + effective_section1;
                    const int preferred_name_col = preferred_dim_col + effective_section2;
                    const int preferred_trailing_col = preferred_name_col + effective_section3;
                    const int preferred_delim_col = preferred_trailing_col + effective_section4;

                    int dim_col = preferred_dim_col;
                    if (vl.packed_dim != npos) {
                        dim_col = std::max(dim_col,
                                           vl.indent + rendered_width(tokens, vl.first, vl.packed_dim) + 1);
                        tokens[vl.packed_dim].mutable_.align.enabled = true;
                        tokens[vl.packed_dim].mutable_.align.target_column = dim_col;
                    }

                    // A declaration line is a fixed section record, not a
                    // compact list of only the sections that have text.  When
                    // a packed-dimension section is absent, section2 still
                    // contributes its configured/adaptive width so the signal
                    // name remains in section3 instead of sliding left into
                    // section2.  If section2 is present and longer than its
                    // preferred width, only the name boundary is repaired; the
                    // later trailing/delimiter boundaries still try their own
                    // preferred columns before moving right for real overlap.
                    int name_col = preferred_name_col;
                    if (vl.packed_dim != npos) {
                        size_t close = tokens[vl.packed_dim].immutable.syntax.matching_token;
                        if (close != npos && close < vl.first_name)
                            name_col = std::max(name_col,
                                                dim_col + token_text_width(tokens, vl.packed_dim, close + 1) + 1);
                    } else {
                        name_col = std::max(name_col,
                                            vl.indent + rendered_width(tokens, vl.first, vl.first_name) + 1);
                    }
                    const int name_width = rendered_width(tokens, vl.first_name, vl.name_end + 1);
                    size_t unpacked_dim = npos;
                    for (size_t k = vl.name_end + 1; k < vl.first_delim; ++k) {
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

                        // Section4 starts at the preferred trailing column when
                        // possible.  A long name repairs only this boundary;
                        // it does not automatically drag the following
                        // delimiter boundary to the right.
                        int trailing_start = std::max(preferred_trailing_col,
                                                      name_col + name_width + 1);
                        tokens[open_bracket].mutable_.align.enabled = true;
                        tokens[open_bracket].mutable_.align.target_column = trailing_start;

                        if (after_target != npos) {
                            tokens[after_target].mutable_.align.enabled = true;
                            tokens[after_target].mutable_.align.target_column =
                                std::max(preferred_trailing_col,
                                         trailing_start + token_text_width(tokens, open_bracket, close_bracket + 1) + 1);
                        }
                    };

                    auto align_delim = [&](size_t name, size_t delim) {
                        if (delim != npos && delim < tokens.size()) {
                            tokens[delim].mutable_.align.enabled = true;
                            tokens[delim].mutable_.align.target_column =
                                name == vl.first_name
                                    ? preferred_delim_col
                                    : tokens[name].mutable_.align.target_column + effective_section3 + effective_section4;
                        }
                    };

                    if (vl.eq != npos) {
                        if (unpacked_dim != npos) {
                            align_unpacked_trailing(unpacked_dim, vl.eq);
                        } else {
                            tokens[vl.eq].mutable_.align.enabled = true;
                            tokens[vl.eq].mutable_.align.target_column =
                                std::max(preferred_trailing_col,
                                         name_col + name_width + 1);
                        }
                        tokens[vl.semi].mutable_.align.enabled = true;
                        tokens[vl.semi].mutable_.align.target_column = preferred_delim_col;
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
                // A clocking block's `input v;` is a clocking signal, not a port.
                if (ln.first == npos || !is_port_direction(tokens[ln.first].lex.kind) ||
                    tokens[ln.first].immutable.syntax.paren_depth != 0 ||
                    tokens[ln.first].immutable.syntax.in_clocking_block)
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
                                     : rendered_width(tokens, type_first,
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
                                                                     group_type_width + 1), opts_);
                const int effective_s3 = opts_.port_declaration.align_adaptive
                                             ? s3
                                             : option_width(std::max(opts_.port_declaration.section3_min_width,
                                                                     group_packed_width + 1), opts_);
                const int effective_s4 = opts_.port_declaration.align_adaptive
                                             ? s4
                                             : option_width(std::max(opts_.port_declaration.section4_min_width,
                                                                     group_name_width + 1), opts_);
                // Section5 holds the unpacked dimension.  Non-adaptive widens it
                // to the group's widest one so the separator column stays common.
                int group_unpacked_width = 0;
                for (size_t gi = li; gi < j; ++gi) {
                    const size_t open = port_lines[gi].unpacked_dim;
                    if (open == npos) continue;
                    const size_t close = tokens[open].immutable.syntax.matching_token;
                    if (close != npos)
                        group_unpacked_width = std::max(group_unpacked_width,
                                                        token_text_width(tokens, open, close + 1));
                }
                const int effective_s5 = opts_.port_declaration.align_adaptive
                                             ? s5
                                             : std::max(s5, group_unpacked_width);

                for (size_t gi = li; gi < j; ++gi) {
                    const auto& pl = port_lines[gi];
                    for (size_t k = pl.first; k <= pl.semi && k < tokens.size(); ++k) {
                        tokens[k].mutable_.align.enabled = false;
                        tokens[k].mutable_.align.target_column = -1;
                    }

                    const int base = pl.indent;
                    const int preferred_type_col = base + effective_s1;
                    const int preferred_packed_col = preferred_type_col + effective_s2;
                    // Preserve the five-section port-declaration model even
                    // for lines with omitted type or packed-dimension text.
                    // Empty sections still occupy their section width; later
                    // sections must not be renumbered leftward just because an
                    // earlier section has no token on this line or in this
                    // group.  Each boundary uses its preferred column unless
                    // actual text on this line would overlap it.
                    const int preferred_name_col = preferred_packed_col + effective_s3;
                    const int trailing_gap = 0;
                    const int preferred_unpacked_col = preferred_name_col + effective_s4 + trailing_gap;
                    const int preferred_first_sep_col = preferred_unpacked_col + effective_s5;

                    const int type_col = std::max(preferred_type_col,
                                                  base + pl.direction_width + 1);
                    if (pl.type_first != npos) {
                        tokens[pl.type_first].mutable_.align.enabled = true;
                        tokens[pl.type_first].mutable_.align.target_column = type_col;
                    }

                    int packed_col = preferred_packed_col;
                    if (pl.packed_dim != npos) {
                        if (pl.type_first != npos)
                            packed_col = std::max(packed_col, type_col + pl.type_width + 1);
                        tokens[pl.packed_dim].mutable_.align.enabled = true;
                        tokens[pl.packed_dim].mutable_.align.target_column = packed_col;
                    }

                    int name_col = preferred_name_col;
                    if (pl.packed_dim != npos) {
                        size_t packed_close = tokens[pl.packed_dim].immutable.syntax.matching_token;
                        if (packed_close != npos && packed_close < pl.first_name)
                            name_col = std::max(name_col,
                                                packed_col + token_text_width(tokens, pl.packed_dim,
                                                                              packed_close + 1) + 1);
                    } else if (pl.type_first != npos) {
                        name_col = std::max(name_col, type_col + pl.type_width + 1);
                    } else {
                        name_col = std::max(name_col, base + pl.direction_width + 1);
                    }
                    tokens[pl.first_name].mutable_.align.enabled = true;
                    tokens[pl.first_name].mutable_.align.target_column = name_col;

                    const int unpacked_col = std::max(preferred_unpacked_col,
                                                      name_col + pl.name_width + 1);
                    if (pl.unpacked_dim != npos) {
                        tokens[pl.unpacked_dim].mutable_.align.enabled = true;
                        tokens[pl.unpacked_dim].mutable_.align.target_column = unpacked_col;
                    }
                    // The separator returns to its preferred column whenever
                    // this line's text ends before it; an overflow moves only
                    // the boundaries it actually overlaps.
                    int text_end = name_col + pl.name_width;
                    if (pl.unpacked_dim != npos) {
                        const size_t close = tokens[pl.unpacked_dim].immutable.syntax.matching_token;
                        if (close != npos)
                            text_end = unpacked_col + token_text_width(tokens, pl.unpacked_dim, close + 1);
                    }
                    tokens[pl.first_delim].mutable_.align.enabled = true;
                    tokens[pl.first_delim].mutable_.align.target_column =
                        std::max(preferred_first_sep_col, text_end);

                    // Subsequent comma declarators on the same non-ANSI line
                    // are still aligned relative to the previous separator.
                    // This keeps `input logic a, b;` readable without trying
                    // to fold secondary names into the cross-line section
                    // model, which only describes the first declarator.
                    int next_name_col = tokens[pl.first_delim].mutable_.align.target_column + 2;
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
                // Module ports only: a hanging function or task argument list
                // (`f(input logic [7:0] a,` / `input logic [7:0] b,`) has
                // direction-led lines too, and its layout places them.
                const size_t list_delim = prev_code(tokens, ln.first);
                if (list_delim == npos || tokens[list_delim].mutable_.wrap.list_kind != WrapListKind::ModulePorts)
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
                int type_width = rendered_width(tokens, type_first, dim);
                int dim_target = type_col + type_width + 1;
                if (!opts_.port_declaration.align_adaptive)
                    dim_target = std::max(dim_target, base + opts_.port_declaration.section1_min_width +
                                                      opts_.port_declaration.section2_min_width);
                tokens[dim].mutable_.align.enabled = true;
                tokens[dim].mutable_.align.target_column = dim_target;
            }
        }

        // `input logic clk, rst_n,` -- WrapPass keeps the bare names of one
        // declaration on its row.  True when `item` sits on the row of the
        // item before it.
        auto shares_row = [&](const auto& prev, const auto& item) {
            if (prev.comma == npos || tokens[item.first].mutable_.wrap.must_break_before ||
                tokens[prev.comma].mutable_.wrap.must_break_after)
                return false;
            // A `//` comment or a directive ends the row whatever the flags say.
            for (size_t k = prev.last + 1; k < item.first; ++k)
                if (tokens[k].lex.is_directive || tokens[k].lex.comment_kind == CommentLexemeKind::Line)
                    return false;
            return true;
        };

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
                    int namew;      // section4: the name, or the run `clk, rst_n` sharing its row
                    int typew;
                    int leadw;      // section1 as rendered: direction, or a whole interface type,
                                    // a leading `/* c */` included (it moves its own row only)
                    bool directed;  // false: `bus_if.master m`, `my_t x` -- no direction
                };
                // Width of the row from `first` (its first code token) through
                // `last`, a leading `/* c */` included -- rendered_width()
                // skips comments.
                auto row_width_through = [&](size_t first, size_t last) {
                    size_t row = first;
                    while (row > 0 && !tokens[row].mutable_.wrap.must_break_before &&
                           !tokens[row - 1].mutable_.wrap.must_break_after &&
                           tokens[row - 1].lex.comment_kind == CommentLexemeKind::Block)
                        --row;
                    int w = 0;
                    for (size_t k = row; k <= last; ++k) {
                        if (k != row && !tokens[k].mutable_.space.suppress_space)
                            w += tokens[k].mutable_.space.spaces_before;
                        w += token_width(tokens[k]);
                    }
                    return w;
                };
                std::vector<Decl> decls;
                for (size_t n = 0; n < items.size(); ++n) {
                    const auto& item = items[n];
                    // The names after the first are one field with it: the
                    // row's comma is the one that ends the last of them.
                    size_t run = n;
                    while (run + 1 < items.size() && shares_row(items[run], items[run + 1]))
                        ++run;
                    const bool has_run = run != n;
                    const size_t row_comma = items[run].comma;
                    const size_t row_last = items[run].last;
                    n = run;
                    const bool directed = is_port_direction(tokens[item.first].lex.kind);
                    // An interface port (`intf.mp bus`, `interface.slave b`) or
                    // a typed port with no direction: its type takes section1.
                    if (!directed && !kind_is(tokens[item.first], TK::InterfaceKeyword) &&
                        !kind_is(tokens[item.first], TK::Identifier))
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
                    if (!directed) {
                        size_t unpacked_dim = npos;
                        for (size_t k = name + 1; k < item.last; ++k) {
                            if (kind_is(tokens[k], TK::OpenBracket)) {
                                unpacked_dim = k;
                                break;
                            }
                        }
                        decls.push_back({item.first, npos, npos, name, has_run ? npos : unpacked_dim, row_comma,
                                         has_run ? rendered_width(tokens, name, row_last + 1)
                                                 : token_width(tokens[name]),
                                         0, row_width_through(item.first, prev_code(tokens, name)), false});
                        continue;
                    }
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
                                 : rendered_width(tokens, type_first, packed_dim != npos ? packed_dim : name);
                    size_t unpacked_dim = npos;
                    for (size_t k = name + 1; k < item.last; ++k) {
                        if (kind_is(tokens[k], TK::OpenBracket)) {
                            unpacked_dim = k;
                            break;
                        }
                    }
                    decls.push_back({item.first, type_first, packed_dim, name, has_run ? npos : unpacked_dim,
                                     row_comma,
                                     has_run ? rendered_width(tokens, name, row_last + 1)
                                             : token_width(tokens[name]),
                                     tw, row_width_through(item.first, item.first), true});
                }
                int base = decls.empty() ? 0 : tokens[decls.front().first].mutable_.indent.base_indent;
                const int s1 = option_width(opts_.port_declaration.section1_min_width, opts_);
                const int s2 = option_width(opts_.port_declaration.section2_min_width, opts_);
                const int s3 = option_width(opts_.port_declaration.section3_min_width, opts_);
                const int s4 = option_width(opts_.port_declaration.section4_min_width, opts_);
                const int s5 = option_width(opts_.port_declaration.section5_min_width, opts_);
                // The configured section widths describe fixed semantic
                // fields.  Section5 is reserved whenever its minimum width is
                // enabled, even on ANSI port lines that have no trailing text
                // after the port name.
                bool group_has_packed = false;
                int group_direction_width = 0;
                int group_type_width = 0;
                int group_packed_width = 0;
                int group_name_width = 0;
                int group_undirected_width = 0; // `intf.mp` spans sections 1-3
                for (const auto& d : decls) {
                    group_name_width = std::max(group_name_width, d.namew);
                    if (!d.directed) {
                        group_undirected_width = std::max(group_undirected_width, d.leadw);
                        continue;
                    }
                    group_direction_width = std::max(group_direction_width, token_width(tokens[d.first]));
                    group_type_width = std::max(group_type_width, d.typew);
                    if (d.packed_dim != npos) {
                        group_has_packed = true;
                        size_t packed_close = tokens[d.packed_dim].immutable.syntax.matching_token;
                        if (packed_close != npos && packed_close < d.name)
                            group_packed_width =
                                std::max(group_packed_width,
                                         token_text_width(tokens, d.packed_dim, packed_close + 1));
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
                                                group_type_width + 1), opts_);
                const int effective_s3 =
                    opts_.port_declaration.align_adaptive
                        ? s3
                        : option_width(std::max(opts_.port_declaration.section3_min_width,
                                                group_packed_width + 1), opts_);
                // A non-adaptive group widens for an interface type the way it
                // widens for a long direction: the type section takes the rest.
                const int undirected_extra =
                    opts_.port_declaration.align_adaptive
                        ? 0
                        : option_width(std::max(0, group_undirected_width + 1 -
                                                       (effective_s1 + effective_s2 + effective_s3)), opts_);

                const int preferred_type_col = base + effective_s1;
                const int preferred_packed_col = preferred_type_col + effective_s2 + undirected_extra;
                // The port name is section4.  It always begins after the
                // direction, type, and packed-dimension sections.  A missing
                // section2 or section3 token is an empty field with preserved
                // width; it must not cause section4 to collapse into an
                // earlier column.  Long text repairs only the boundary it
                // would overlap; later section boundaries keep their own
                // preferred columns whenever possible.
                const int preferred_name_col = preferred_packed_col + effective_s3;
                const int preferred_trailing_col = preferred_name_col +
                    (opts_.port_declaration.align_adaptive ? s4
                                                           : option_width(std::max(opts_.port_declaration.section4_min_width,
                                                                                   group_name_width + 1), opts_));
                // Section5 holds the unpacked dimension.  Non-adaptive widens it
                // to the group's widest one so the `,` column stays common.
                int group_unpacked_width = 0;
                for (const auto& d : decls) {
                    if (d.unpacked_dim == npos) continue;
                    const size_t close = tokens[d.unpacked_dim].immutable.syntax.matching_token;
                    if (close != npos)
                        group_unpacked_width = std::max(group_unpacked_width,
                                                        token_text_width(tokens, d.unpacked_dim, close + 1));
                }
                const int effective_s5 = opts_.port_declaration.align_adaptive
                                             ? s5
                                             : std::max(s5, group_unpacked_width);
                const int preferred_comma_col = preferred_trailing_col + effective_s5;

                for (const auto& d : decls) {
                    int type_target = std::max(preferred_type_col, base + d.leadw + 1);
                    if (d.type_first != npos) {
                        tokens[d.type_first].mutable_.align.enabled = true;
                        tokens[d.type_first].mutable_.align.target_column = type_target;
                    }

                    int packed_col = preferred_packed_col;
                    if (d.packed_dim != npos && d.type_first != npos) {
                        int packed_width = rendered_width(tokens, d.type_first, d.packed_dim);
                        packed_col = std::max(packed_col, type_target + packed_width + 1);
                        tokens[d.packed_dim].mutable_.align.enabled = true;
                        tokens[d.packed_dim].mutable_.align.target_column = packed_col;
                    }

                    int decl_name_target = preferred_name_col;
                    if (d.packed_dim != npos) {
                        size_t packed_close = tokens[d.packed_dim].immutable.syntax.matching_token;
                        if (packed_close != npos && packed_close < d.name) {
                            decl_name_target = std::max(
                                decl_name_target,
                                packed_col + token_text_width(tokens, d.packed_dim, packed_close + 1) + 1);
                        }
                    } else if (d.type_first != npos) {
                        decl_name_target = std::max(decl_name_target, type_target + d.typew + 1);
                    } else {
                        decl_name_target = std::max(decl_name_target, base + d.leadw + 1);
                    }
                    tokens[d.name].mutable_.align.enabled = true;
                    tokens[d.name].mutable_.align.target_column = decl_name_target;

                    // Section5/trailing begins at its preferred column when
                    // possible.  A long section4 name repairs this boundary,
                    // and the comma returns to its own preferred column as
                    // soon as this line's text ends before it: an overflow
                    // moves only the boundaries it actually overlaps.
                    const int name_end = decl_name_target + d.namew;
                    const int trailing_start = std::max(preferred_trailing_col, name_end + 1);
                    int text_end = name_end;

                    if (d.unpacked_dim != npos) {
                        size_t close = tokens[d.unpacked_dim].immutable.syntax.matching_token;
                        if (close == npos)
                            continue;
                        text_end = trailing_start + token_text_width(tokens, d.unpacked_dim, close + 1);
                    }
                    const int comma_target = std::max(preferred_comma_col, text_end);

                    if (d.unpacked_dim != npos) {
                        tokens[d.unpacked_dim].mutable_.align.enabled = true;
                        tokens[d.unpacked_dim].mutable_.align.target_column = trailing_start;
                        if (d.comma != npos && s5 > 0) {
                            tokens[d.comma].mutable_.align.enabled = true;
                            tokens[d.comma].mutable_.align.target_column = comma_target;
                        }
                    } else if (d.comma != npos) {
                        if (s5 <= 0) {
                            tokens[d.comma].mutable_.align.enabled = false;
                            tokens[d.comma].mutable_.align.target_column = -1;
                            continue;
                        }
                        tokens[d.comma].mutable_.align.enabled = true;
                        tokens[d.comma].mutable_.align.target_column = comma_target;
                    }
                    // The last item has no comma; its trailing comment goes
                    // where it would sit after one, in line with the comments
                    // of the items above.
                    if (d.comma == npos && s5 > 0) {
                        const size_t last = prev_code(tokens, close);
                        const size_t c = last == npos ? npos : last + 1;
                        if (c != npos && c < close && last >= d.name &&
                            tokens[c].lex.comment_kind != CommentLexemeKind::None &&
                            tokens[c].immutable.comment.role == CommentRole::Trailing &&
                            !tokens[c].mutable_.wrap.must_break_before) {
                            tokens[c].mutable_.align.enabled = true;
                            tokens[c].mutable_.align.target_column = comma_target + 2;
                        }
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
            int max_namew = 0;
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
                // `, .rst_n(rst_n)` -- a comma that had to stay in front of
                // its connection (after a `//` comment or a directive) shares
                // the row, so the name field starts that much later.
                const size_t lead = prev_code(tokens, dot);
                if (lead != npos && lead > open && kind_is(tokens[lead], TK::Comma) &&
                    !tokens[dot].mutable_.wrap.must_break_before)
                    nw += 2;
                // Measured as it will render, from `(` to `)` less the two
                // parens: a select or a call inside the connection (`a[n]`,
                // `f(x, y)`) is narrower than a private spacing estimate
                // makes it, which padded the widest row, and the padding
                // space_inside_parens puts inside the parens is part of the
                // field, or the widest row overran the column.
                int sw = rendered_width(tokens, op, cl + 1) - 2;
                max_namew = std::max(max_namew, nw);
                max_sig = std::max(max_sig, sw);
                conns.push_back({name, op, cl, nw, sw});
            }
            for (const auto& c : conns) {
                int item_indent = tokens[prev_code(tokens, c.name)].mutable_.indent.base_indent;
                int sig_width = opts_.instance.align_adaptive
                    ? std::max(opts_.instance.instance_port_between_paren_width, c.sigw)
                    : max_sig;
                tokens[c.op].mutable_.align.enabled = true;
                tokens[c.op].mutable_.align.alignment_group = group;
                // The field runs from `.` to `(`: the dot, the name and at
                // least one space.  Non-adaptive sizes every row by the
                // widest name so the `(` share one column.
                const int configured_port_width = option_width(opts_.instance.instance_port_name_width, opts_);
                const int namew = opts_.instance.align_adaptive ? c.namew : max_namew;
                tokens[c.op].mutable_.align.target_column =
                    item_indent + std::max(configured_port_width, namew + 2);
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
                struct E { size_t first, last, eq, comma; int namew, valw; };
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
                    es.push_back({item.first, item.last, eq, item.comma, nw, vw});
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
                    const int comma_target =
                        opts_.tab_align ? (eq_target + snap_to_grid(std::max(1, local_valw) + opts_.indent_size,
                                                                     opts_.indent_size) - 1) :
                        (e.eq == npos) ? (local_valw > 0 ? (eq_target + 2 + local_valw)
                                                         : eq_target)
                                       : (eq_target + 2 + local_valw);
                    if (e.comma != npos) {
                        tokens[e.comma].mutable_.align.enabled = true;
                        tokens[e.comma].mutable_.align.target_column = comma_target;
                    } else {
                        // The last item has no comma; its trailing comment
                        // goes where it would sit after one, in line with
                        // the comments of the items above.
                        const size_t c = e.last + 1;
                        if (c < close && tokens[c].lex.comment_kind != CommentLexemeKind::None &&
                            tokens[c].immutable.comment.role == CommentRole::Trailing &&
                            !tokens[c].mutable_.wrap.must_break_before) {
                            tokens[c].mutable_.align.enabled = true;
                            tokens[c].mutable_.align.target_column = comma_target + 2;
                        }
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
                    // `import task send(` ... `),` -- a prototype WrapPass
                    // broke over several lines has no width to put in the
                    // signal column: its comma follows the `)` that ends it,
                    // and it must not widen the column for the other items.
                    bool multi_line = false;
                    for (size_t k = sig + 1; k <= item.last && !multi_line; ++k)
                        multi_line = tokens[k].mutable_.wrap.must_break_before ||
                                     tokens[k - 1].mutable_.wrap.must_break_after;
                    // `output addr, wdata, valid,` -- names sharing the row
                    // are no column either; their commas stay with them.
                    const size_t n = static_cast<size_t>(&item - items.data());
                    if (multi_line || (n + 1 < items.size() && shares_row(item, items[n + 1]))) {
                        ms.push_back({item.first, sig, npos, dw, 0});
                        continue;
                    }
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
        shift_column_anchored_lines(tokens, opts_.blank_lines_between_items);
    }
private:
    // IndentPass measures a hanging list's column (and a block call's name
    // column) from SpaceMetadata alone, before any padding exists:
    //
    //   y          = f(a,
    //         b);          <- measured from `y = f(`
    //
    // Walk the lines the way render_tokens() will, tracking how far each
    // token has been pushed right of where IndentPass measured it, and move
    // each line whose indent was measured from a column by the drift of that
    // column's token.  The drift carries the anchor line's own shift, so a
    // list nested in a shifted list moves with it.  Alignment targets on a
    // shifted line move with the line, keeping their padding.
    static void shift_column_anchored_lines(TokenStream& tokens, int blank_lines_between_items) {
        std::vector<int> drift(tokens.size(), 0);
        int col = 0;     // column as rendered
        int natural = 0; // column as IndentPass measured it
        int line_shift = 0;
        bool at_line_start = true;
        for (size_t i = 0; i < tokens.size(); ++i) {
            Tok& tok = tokens[i];
            // BlankLinePass runs later; a blank line it will emit breaks the line too.
            if (i > 0 && (tok.mutable_.wrap.must_break_before || tok.mutable_.comment.force_own_line ||
                          (blank_lines_between_items > 0 && is_blank_line_boundary(tokens, i))))
                at_line_start = true;
            if (is_passthrough(tok)) {
                // Emitted verbatim with no indent, as render_tokens() does.
                if (at_line_start) {
                    col = natural = 0;
                    line_shift = 0;
                }
                drift[i] = col - natural;
                std::string_view text(tok.lex.text);
                const size_t nl = last_newline_offset(text);
                if (nl == std::string_view::npos) {
                    col += static_cast<int>(text.size());
                    natural += static_cast<int>(text.size());
                    at_line_start = false;
                } else {
                    col = natural = static_cast<int>(text.size() - nl - 1);
                    line_shift = 0;
                    at_line_start = col == 0;
                }
                if (tok.mutable_.wrap.must_break_after) at_line_start = true;
                continue;
            }
            if (at_line_start) {
                const int indent = tok.mutable_.indent.base_indent + tok.mutable_.indent.continuation_indent +
                                   tok.mutable_.comment.relative_indent;
                const size_t anchor = tok.mutable_.indent.anchor_token;
                line_shift = (anchor != npos && anchor < i) ? drift[anchor] : 0;
                tok.mutable_.align.indent_shift = line_shift;
                natural = std::max(0, indent);
                col = std::max(0, indent + line_shift);
                at_line_start = false;
            } else {
                const int spaces = tok.mutable_.space.suppress_space ? 0 : tok.mutable_.space.spaces_before;
                natural += spaces;
                int gap = spaces;
                if (tok.mutable_.align.enabled && tok.mutable_.align.target_column >= 0) {
                    tok.mutable_.align.target_column += line_shift;
                    if (col < tok.mutable_.align.target_column)
                        gap = std::max(gap, tok.mutable_.align.target_column - col);
                }
                col += gap;
            }
            drift[i] = col - natural;
            // A block comment spanning lines ends on a line no padding reached.
            if (const size_t nl = last_newline_offset(tok.lex.text); nl != std::string::npos) {
                col = natural = static_cast<int>(tok.lex.text.size() - nl - 1);
                line_shift = 0;
            } else {
                col += static_cast<int>(tok.lex.text.size());
                natural += static_cast<int>(tok.lex.text.size());
            }
            if (tok.mutable_.wrap.must_break_after) at_line_start = true;
        }
    }
    const FormatOptions& opts_;
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
                // A line break terminates an escaped identifier just as a space
                // does, so only the same-line case needs the separator kept.
                const bool after_escaped_on_same_line =
                    i > 0 && tokens[i - 1].lex.is_escaped_identifier &&
                    !t.mutable_.wrap.must_break_before && !t.mutable_.comment.force_own_line;
                t.mutable_.space.spaces_before = after_escaped_on_same_line ? 1 : 0;
                continue;
            }
            const Tok& L = tokens[i - 1];
            int spaces = 1;

            // An escaped identifier is delimited by whitespace, so the token
            // after it can never be closed up against it -- `\\esc` + `;` would
            // re-lex as the single identifier `\\esc;`.  This outranks every
            // no-space rule below, so decide it before any of them run.
            if (L.lex.is_escaped_identifier) {
                t.mutable_.space.spaces_before = 1;
                t.mutable_.space.suppress_space = false;
                continue;
            }

            // `(*` and `*)` are single lexemes in the LRM.  Letting the ordinary
            // paren rules put a space between the halves turns an attribute into
            // a parenthesised expression, so keep them closed up.  The text
            // between the delimiters spaces like any other expression.
            if (t.lex.in_attribute_instance && L.lex.in_attribute_instance &&
                ((kind_is(L, TK::OpenParenthesis) && kind_is(t, TK::Star)) ||
                 (kind_is(L, TK::Star) && kind_is(t, TK::CloseParenthesis)))) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // Slang represents based literals as multiple tokens -- `12'h7c4`
            // is `12`, `'h`, `7`, `c4` and `4'b1??0` is `4`, `'b`, `1`, `?`,
            // `?`, `0`.  Keep the size against its base and the value pieces
            // closed up; a space before a `?` digit re-lexes it as the
            // conditional operator, so no later rule may reopen the gap.
            // A macro can be the size: `` `WIDTH'h3 ``, `` `W(8)'d1 ``.
            auto is_macro_size = [&]() {
                if (kind_is(L, TK::MacroUsage))
                    return true;
                if (!kind_is(L, TK::CloseParenthesis) || L.immutable.syntax.matching_token == npos)
                    return false;
                const size_t callee = prev_code(tokens, L.immutable.syntax.matching_token);
                return callee != npos && kind_is(tokens[callee], TK::MacroUsage);
            };
            if ((kind_is(t, TK::IntegerBase) && (kind_is(L, TK::IntegerLiteral) || is_macro_size())) ||
                t.lex.continues_vector_literal) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // SVA repetition `b[->1]`, `b[*1:3]`, `b[=2:$]` is an operator,
            // not a dimension: everything inside its brackets binds, and the
            // dimension options (padding, range-colon spacing) do not apply.
            {
                const size_t li = prev_code(tokens, i);
                const size_t lli = li == npos ? npos : prev_code(tokens, li);
                size_t enclosing = npos;
                if (kind_is(t, TK::CloseBracket)) {
                    enclosing = t.immutable.syntax.matching_token;
                } else if (t.immutable.syntax.bracket_depth > 0) {
                    for (size_t n = i; n > 0; --n) {
                        const Tok& b = tokens[n - 1];
                        if (kind_is(b, TK::CloseBracket) && b.immutable.syntax.matching_token != npos &&
                            b.immutable.syntax.matching_token < n - 1) {
                            n = b.immutable.syntax.matching_token + 1;
                            continue;
                        }
                        if (kind_is(b, TK::OpenBracket)) {
                            enclosing = n - 1;
                            break;
                        }
                    }
                }
                if ((enclosing != npos && enclosing < tokens.size() && is_code_token(t) &&
                     tokens[enclosing].immutable.topology.is_repetition_bracket) ||
                    (li != npos && tokens[li].immutable.topology.is_repetition_bracket) ||
                    (lli != npos && tokens[lli].immutable.topology.is_repetition_bracket)) {
                    t.mutable_.space.spaces_before = 0;
                    t.mutable_.space.suppress_space = true;
                    continue;
                }
            }

            // An inline conditional directive is delimited by whitespace:
            // `` `else31 `` would re-lex as a macro named `else31`.
            if (L.lex.is_directive || (t.lex.is_directive && !kind_is(L, TK::OpenBracket))) {
                t.mutable_.space.spaces_before = 1;
                t.mutable_.space.suppress_space = false;
                continue;
            }

            // Basic no-space rules
            if (no_space_before(t.lex.kind) || no_space_after(L.lex.kind)) spaces = 0;
            // `( /*autoinst*/` keeps its space before the list.  A block
            // comment that is the parens' only content is padded like any
            // other first token, symmetric with its `)`: `.o(/* unused */)`.
            if (t.lex.comment_kind != CommentLexemeKind::None && kind_is(L, TK::OpenParenthesis)) {
                const bool sole_content = t.lex.comment_kind == CommentLexemeKind::Block &&
                    L.immutable.syntax.matching_token != npos &&
                    next_code(tokens, i + 1, tokens.size()) == L.immutable.syntax.matching_token;
                if (!sole_content) spaces = 1;
            }
            // `typedef enum { // open` -- a comment after a `{` keeps its
            // space too, as it does after a block brace.
            if (t.lex.comment_kind != CommentLexemeKind::None && kind_is(L, TK::OpenBrace) &&
                next_code(tokens, i + 1, tokens.size()) != L.immutable.syntax.matching_token)
                spaces = 1;
            // Empty positional argument: `, ,` — keep one space so the slot is visible
            if (kind_is(t, TK::Comma) && kind_is(L, TK::Comma)) spaces = 1;

            // Unary ops: no space after.
            // Exception: `~ &a`, `~ |a`, `~ ^a` — tilde followed by a
            // reduction operator must keep a space so it isn't read as
            // the compound `~&` / `~|` / `~^` operator.
            //
            // Which tokens are unary depends on position: `-a`, `&c` and
            // prefix `++x` bind to their operand, while binary `b ^~ c` and
            // postfix `x++ > 3` space like any binary operator.
            const size_t li = prev_code(tokens, i);
            const bool L_prefix = li != npos && li == i - 1 && in_prefix_position(tokens, li);
            const bool L_unary =
                (is_unary_op(L.lex.kind) || is_sign_or_reduction_op(L.lex.kind)) &&
                (L_prefix || kind_is(L, TK::Tilde) || kind_is(L, TK::Exclamation));
            if (L_unary) {
                if (!unary_pair_merges(L.lex.kind, t.lex.kind)) {
                    t.mutable_.space.spaces_before = 0;
                    t.mutable_.space.suppress_space = true;
                    continue;
                }
                spaces = 1;
            }

            // Hierarchy: . and ::
            if (kind_is(L, TK::Dot) || kind_is(L, TK::DoubleColon)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }
            // `#( .WIDTH(8) )` -- a padded `(` pads before a named connection
            // too, as it does before any other first token.
            const bool padded_open = kind_is(L, TK::OpenParenthesis) &&
                (opts_.spacing.space_inside_parens ||
                 (L.immutable.topology.starts_argument_list && opts_.function_call.space_inside_paren));
            // `sub u ((* keep *) .p(x));` -- an attribute keeps a space on each
            // side, before the connection it annotates as before anything else.
            const bool after_attribute = kind_is(L, TK::CloseParenthesis) &&
                                         L.lex.in_attribute_instance;
            const bool dot_spaced = kind_is(t, TK::Dot) &&
                (dot_keeps_space_after(tokens, i) || padded_open || after_attribute);
            if (dot_spaced) {
                spaces = 1;
            } else if (kind_is(t, TK::Dot) || kind_is(t, TK::DoubleColon)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // # hash: no space before next token.  `##` is the cycle delay
            // (`a ##1 b`, `##[1:3]`) and binds the same way.
            if (kind_is(L, TK::Hash) || kind_is(L, TK::DoubleHash)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // No space before '[' when it's an index/dimension on an identifier or closer
            if (kind_is(t, TK::OpenBracket) &&
                (is_identifier_like(L) || kind_is(L, TK::CloseBracket) || kind_is(L, TK::CloseParenthesis) ||
                 kind_is(L, TK::NewKeyword) ||
                 // `{b, c}[3:0]` -- a select on a concatenation.  A `}` that
                 // closes a block or a struct body ends no expression.
                 (kind_is(L, TK::CloseBrace) && L.immutable.syntax.matching_token != npos &&
                  !tokens[L.immutable.syntax.matching_token].immutable.topology.opens_brace_block)) &&
                !closes_strength(tokens, i - 1))
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
            // An empty port list (`sub u0 ();`) is no list to WrapPass, so it
            // is recognised by its shape here.
            const bool empty_instance_ports =
                kind_is(t, TK::OpenParenthesis) && t.immutable.syntax.matching_token != npos &&
                next_code(tokens, i + 1, tokens.size()) == t.immutable.syntax.matching_token &&
                is_instance_port_open(tokens, i);
            if (kind_is(t, TK::OpenParenthesis) && is_function_task_declaration_open(tokens, i))
                spaces = opts_.function_declaration.space_before_paren ? 1 : 0;
            // Function/task call spacing.  `new` lexes as its own keyword rather
            // than an identifier, but `new(...)` is a constructor call and must
            // follow the same option -- otherwise every class constructor
            // renders as `new (name)`.
            // `type(x)` and `binsof(cp)` are call-shaped keywords too.
            else if (kind_is(t, TK::OpenParenthesis) && (kind_is(L, TK::Identifier) || kind_is(L, TK::SystemIdentifier) || kind_is(L, TK::MacroUsage) || kind_is(L, TK::NewKeyword) ||
                                                        kind_is(L, TK::TypeKeyword) || kind_is(L, TK::BinsOfKeyword) ||
                                                        // `q.unique()`, `q.and()` -- a member named by a keyword
                                                        // is still a method.  Nothing but a name follows a `.`.
                                                        (is_code_token(L) && i >= 2 && kind_is(tokens[i - 2], TK::Dot))))
                spaces = opts_.function_call.space_before_paren ? 1 : 0;
            // `new[10](init)` -- the initializer is the constructor's argument
            // list, so it spaces like `new(...)`.
            else if (kind_is(t, TK::OpenParenthesis) && kind_is(L, TK::CloseBracket) &&
                     L.immutable.syntax.matching_token != npos) {
                const size_t before_dim = prev_code(tokens, L.immutable.syntax.matching_token);
                if (before_dim != npos && kind_is(tokens[before_dim], TK::NewKeyword))
                    spaces = opts_.function_call.space_before_paren ? 1 : 0;
                // `u_arr[3:0](...)` -- an instance array's connections space
                // like the plain instance's `u_one(...)`.
                else if (t.mutable_.wrap.list_kind == WrapListKind::InstancePorts ||
                         is_gate_terminal_open(tokens, i) || empty_instance_ports)
                    spaces = opts_.function_call.space_before_paren ? 1 : 0;
            }
            // `rand join (0.5) a b` -- the lexer hands a production's `join`
            // over as a plain word; it is still a keyword and calls nothing.
            if (kind_is(t, TK::OpenParenthesis) && kind_is(L, TK::Identifier)) {
                const size_t word = prev_code(tokens, i);
                const size_t before = word == npos ? npos : prev_code(tokens, word);
                if (before != npos && kind_is(tokens[before], TK::RandKeyword))
                    spaces = 1;
            }
            if (kind_is(t, TK::OpenParenthesis) && opts_.instance.align &&
                (t.mutable_.wrap.list_kind == WrapListKind::InstancePorts || is_gate_terminal_open(tokens, i) ||
                 empty_instance_ports))
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

            // } brace: 1 space after (unless followed by ; or ,).  Only a brace
            // that closes a statement block takes this; an expression brace's
            // `}` spaces like any other closing token, so a nested
            // concatenation renders `{f, {g, h}}` rather than `{f, {g, h} }`.
            if (kind_is(L, TK::CloseBrace) && closes_indent_scope_at(tokens, i - 1) &&
                !kind_is(t, TK::Semicolon) && !kind_is(t, TK::Comma)) spaces = 1;
            // A statement block kept on one line, `with { a < 5; b == 3; }`,
            // is padded inside its braces like `begin ... end`.
            if (kind_is(L, TK::OpenBrace) && L.immutable.topology.opens_brace_block)
                spaces = 1;
            if (kind_is(t, TK::CloseBrace) && t.immutable.syntax.matching_token != npos &&
                kind_is(tokens[t.immutable.syntax.matching_token], TK::OpenBrace) &&
                tokens[t.immutable.syntax.matching_token].immutable.topology.opens_brace_block)
                spaces = 1;
            // An empty block has nothing to pad: `constraint c {}`.
            if (kind_is(t, TK::CloseBrace) && is_empty_brace_pair(tokens, i - 1))
                spaces = 0;
            // A padded `)` keeps its pad after a `}` as after anything else.
            if (kind_is(L, TK::CloseBrace) && kind_is(t, TK::CloseParenthesis) &&
                !opts_.spacing.space_inside_parens &&
                !(t.immutable.topology.ends_argument_list && opts_.function_call.space_inside_paren))
                spaces = 0;

            // Apostrophe / cast: no space
            if (kind_is(t, TK::Apostrophe) || kind_is(L, TK::Apostrophe)) spaces = 0;
            // Apostrophe-open-brace assignment patterns / casts.  Only a
            // cast's type (`pkt_t'{...}`) and an opening delimiter bind to the
            // pattern; after a comma, a pattern key's `:` or a `?` it is an
            // operand like any other (`data : '{raw : 0}`, `c ? '{1} : '{2}`).
            if (kind_is(t, TK::ApostropheOpenBrace) &&
                (is_identifier_like(L) || is_type_keyword(L.lex.kind) ||
                 kind_is(L, TK::CloseParenthesis) || kind_is(L, TK::CloseBracket) ||
                 kind_is(L, TK::OpenParenthesis) || kind_is(L, TK::OpenBracket) ||
                 kind_is(L, TK::OpenBrace) || kind_is(L, TK::ApostropheOpenBrace)))
                spaces = 0;
            // `tagged Pair '{.a, .b}` -- the name is a union member's tag and
            // the pattern its value, as in `tagged Valid .n`; it is not the
            // cast `Pair'{...}`.
            if (kind_is(t, TK::ApostropheOpenBrace) && kind_is(L, TK::Identifier)) {
                const size_t tag_kw = prev_code(tokens, i - 1);
                if (tag_kw != npos && kind_is(tokens[tag_kw], TK::TaggedKeyword))
                    spaces = 1;
            }

            // Postfix ++ / --: no space before when attached to an identifier, ], or )
            if ((kind_is(t, TK::DoublePlus) || kind_is(t, TK::DoubleMinus)) &&
                (is_identifier_like(L) || kind_is(L, TK::CloseBracket) || kind_is(L, TK::CloseParenthesis)))
                spaces = 0;

            // Assignment operators.
            // LessThanEquals is context-sensitive: inside parens it's a comparison, not
            // non-blocking assignment.  Treat it as a regular binary op in that context.
            auto is_assign = [&](size_t at) {
                return is_assignment_op(tokens[at].lex.kind) && !is_relational_less_equal(tokens, at);
            };
            const bool t_assign = is_assign(i);
            const bool L_assign = is_assign(i - 1);
            if (t_assign) spaces = wants_before(opts_.spacing.assignment_operator_spacing) ? 1 : 0;
            if (L_assign) spaces = wants_after(opts_.spacing.assignment_operator_spacing) ? 1 : 0;

            // Binary operators (non-assignment).
            // Closing brackets carry depth=1 (before decrement), so exclude them from the
            // L-side dim check to avoid treating tokens after ] as inside a dimension.
            bool in_dim = t.immutable.syntax.bracket_depth > 0 ||
                          (L.immutable.syntax.bracket_depth > 0 && !kind_is(L, TK::CloseBracket));
            const std::string& bop_mode = in_dim ? opts_.spacing.dimension_binary_operator_spacing : opts_.spacing.binary_operator_spacing;
            // A sign or reduction in unary position is not a binary operator:
            // `f(-a)`, `y = &c`.  Its own gap after it was settled above.
            const bool t_unary = is_sign_or_reduction_op(t.lex.kind) && in_prefix_position(tokens, i);
            if (is_binary_op(t.lex.kind) && !t_assign && !t_unary)
                spaces = wants_before(bop_mode) ? 1 : 0;
            if (is_binary_op(L.lex.kind) && !L_assign && !L_unary)
                spaces = wants_after(bop_mode) ? 1 : 0;
            if (is_binary_op(L.lex.kind) && !L_assign &&
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
                // binary_operator_spacing style -- where one is needed.
                // `a*-b` and `c&~d` lex as written.
                if (operators_would_merge(L.lex.text, t.lex.text))
                    spaces = std::max(spaces, 1);
            }
            // `@(*)` -- the `*` is the implicit event list, not a multiplication,
            // so the `(` before it spaces like the one before any first token.
            if (kind_is(t, TK::Star) && kind_is(L, TK::OpenParenthesis)) {
                const size_t before_open = prev_code(tokens, i - 1);
                if (before_open != npos && kind_is(tokens[before_open], TK::At))
                    spaces = (opts_.spacing.space_inside_parens || opts_.spacing.space_inside_event_control_parens) ? 1 : 0;
            }
            // `inside` is a keyword operator — always needs spaces regardless of bop_mode
            if (kind_is(t, TK::InsideKeyword)) spaces = 1;
            if (kind_is(L, TK::InsideKeyword)) spaces = 1;

            // Range/part-select
            // A min:typ:max triple (`#(1:2:3)`) is one value, spaced as a range.
            const bool t_range_colon = kind_is(t, TK::Colon) && (in_dim || t.immutable.topology.is_min_typ_max_colon);
            const bool L_range_colon = kind_is(L, TK::Colon) && (in_dim || L.immutable.topology.is_min_typ_max_colon);
            if (t_range_colon) spaces = wants_before(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (L_range_colon) spaces = wants_after(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (kind_is(t, TK::PlusColon) || kind_is(t, TK::MinusColon)) spaces = wants_before(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;
            if (kind_is(L, TK::PlusColon) || kind_is(L, TK::MinusColon)) spaces = wants_after(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;

            // wait keyword: no space before ( (like a function call, not a control keyword)
            if (kind_is(t, TK::OpenParenthesis) && kind_is(L, TK::WaitKeyword)) spaces = 0;
            // `first_match(s)` is an operator applied to its argument, written
            // like a call.  So are `strong(s)` and `weak(s)`.
            if (kind_is(t, TK::OpenParenthesis) &&
                (kind_is(L, TK::FirstMatchKeyword) || kind_is(L, TK::StrongKeyword) ||
                 kind_is(L, TK::WeakKeyword)))
                spaces = 0;

            // @ event control spacing.  `always @(e)`, a statement `@(e);` and
            // `@(e) x = 1;` are one construct and follow the same options,
            // whatever the event expression holds.  A covergroup's sampling
            // event is not procedural and keeps its own spacing.
            if (kind_is(t, TK::At)) {
                // After a `;` the `@` starts a statement: the `;` decides.
                spaces = kind_is(L, TK::Semicolon) ||
                                 wants_before(opts_.spacing.procedural_event_control_at_spacing)
                             ? 1 : 0;
                // `assert property (@(posedge clk) ...)` -- right after a
                // `(` the parenthesis spacing decides, not the event rule.
                if (kind_is(L, TK::OpenParenthesis))
                    spaces = opts_.spacing.space_inside_parens ? 1 : 0;
                // The option is about the keyword the event control follows
                // (`always @(e)`).  An intra-assignment event takes the
                // operator's spacing (`q <= @(e) d;`), and a control's `)`
                // or delay is always separated from the `@` after it
                // (`wait (a) @(e) x = 1;`, `#5 @(e) x = 1;`,
                // `q <= repeat (2) @(e) d;`).
                const size_t before_L = prev_code(tokens, i - 1);
                const size_t L_open = kind_is(L, TK::CloseParenthesis) ? L.immutable.syntax.matching_token : npos;
                const size_t L_owner = L_open == npos ? npos : prev_code(tokens, L_open);
                if (L_assign)
                    spaces = wants_after(opts_.spacing.assignment_operator_spacing) ? 1 : 0;
                else if (closes_control_header(tokens, i - 1) ||
                         (before_L != npos && kind_is(tokens[before_L], TK::Hash)) ||
                         (L_owner != npos && tokens[L_owner].immutable.topology.is_intra_assignment_repeat))
                    spaces = 1;
                // `clocking cb @(e);`, `covergroup cg @(e);` -- the event
                // follows the name being declared, not a keyword, and a name
                // is separated from what comes after it.
                else if (is_covergroup_event_at(tokens, i) || is_named_clocking_event_at(tokens, i))
                    spaces = 1;
            }
            if (kind_is(L, TK::At)) {
                const bool covergroup_event = is_covergroup_event_at(tokens, i - 1);
                spaces = (!covergroup_event && wants_after(opts_.spacing.procedural_event_control_at_spacing)) ? 1 : 0;
            }
            // `@@(begin f)` -- a covergroup's block event, spaced as its `@(e)` is.
            if (kind_is(t, TK::DoubleAt))
                spaces = 1;
            if (kind_is(L, TK::DoubleAt))
                spaces = 0;
            // space_inside_event_control_parens: add space inside ( ) of procedural event control.
            // Only applies when ( directly follows @ which is not a standalone delay control.
            if (opts_.spacing.space_inside_event_control_parens) {
                if (kind_is(L, TK::OpenParenthesis) && i >= 2 && kind_is(tokens[i-2], TK::At)) {
                    if (!is_covergroup_event_at(tokens, i - 2)) spaces = 1;
                }
                if (kind_is(t, TK::CloseParenthesis) && t.immutable.syntax.matching_token != npos) {
                    size_t j = t.immutable.syntax.matching_token;
                    if (j >= 1 && j < tokens.size() && kind_is(tokens[j], TK::OpenParenthesis) &&
                        j >= 1 && kind_is(tokens[j-1], TK::At)) {
                        if (!is_covergroup_event_at(tokens, j - 1)) spaces = 1;
                    }
                }
            }

            // An attribute `(* keep *)` is one delimited unit: a space on
            // each side of it, and one inside each delimiter under every
            // config -- its `*` are not multiplications, and its `(` is not
            // a call's (`sub (* keep *) u`, `function (* noinline *) int f`).
            if (t.lex.in_attribute_instance) {
                const bool attr_open = kind_is(t, TK::OpenParenthesis) && i + 1 < tokens.size() &&
                                       tokens[i + 1].lex.in_attribute_instance &&
                                       kind_is(tokens[i + 1], TK::Star);
                const bool attr_close_star = kind_is(t, TK::Star) && i + 1 < tokens.size() &&
                                             tokens[i + 1].lex.in_attribute_instance &&
                                             kind_is(tokens[i + 1], TK::CloseParenthesis);
                const bool after_attr_open = i >= 2 && kind_is(L, TK::Star) && L.lex.in_attribute_instance &&
                                             kind_is(tokens[i - 2], TK::OpenParenthesis) &&
                                             tokens[i - 2].lex.in_attribute_instance;
                if (attr_open && !L.lex.in_attribute_instance &&
                    !(kind_is(L, TK::OpenParenthesis) || kind_is(L, TK::OpenBracket) ||
                      kind_is(L, TK::OpenBrace) || no_space_after(L.lex.kind) ||
                      is_binary_op(L.lex.kind) || is_assignment_op(L.lex.kind) || L_unary))
                    spaces = 1;
                if (attr_close_star || after_attr_open)
                    spaces = 1;
            }

            // "No space" around an operator or `@` is about its operands.  A
            // comment beside one is not an operand: `a+ // carry-in`,
            // `a&& /* gate */ b`, `always_ff /* c */ @(...)`.
            {
                auto operator_like = [&](const Tok& tok) {
                    return is_code_token(tok) && !tok.lex.in_attribute_instance &&
                           (is_binary_op(tok.lex.kind) || is_assignment_op(tok.lex.kind) ||
                            kind_is(tok, TK::At));
                };
                if ((t.lex.comment_kind != CommentLexemeKind::None && operator_like(L)) ||
                    (L.lex.comment_kind != CommentLexemeKind::None && operator_like(t)))
                    spaces = std::max(spaces, 1);
            }

            // End-label colon: `endclass: Foo`, `endfunction: bar`, etc. — no space before `:`
            if (kind_is(t, TK::Colon) && t.immutable.topology.is_block_name_colon)
                spaces = 0;
            // Case item labels are `label: stmt` whatever the label ends in.
            // Deciding from the left token alone gave `8'b0111:` but
            // `4'hc4 :`, `` `OP :`` and `default :` -- and closed up the
            // conditional in `c ? 4'd1 : 4'd2`, whose colon also follows a
            // number.
            if (kind_is(t, TK::Colon) && t.immutable.topology.is_case_item_colon)
                spaces = 0;
            // `a_x: assert property ...`, `cp: coverpoint x;`, `g: if (P)`
            // -- a label names the statement or item that follows it.
            if (kind_is(t, TK::Colon) && t.immutable.topology.is_item_label_colon)
                spaces = 0;
            // `instance top.a use lib.cell:cfg;` -- in a config's `use` clause
            // the colon joins a cell to the configuration that binds it.  It is
            // part of the name, not a conditional's or a label's.  `use` is a
            // keyword of config blocks and nothing else.
            {
                auto is_use_clause_colon = [&](size_t at) {
                    if (!kind_is(tokens[at], TK::Colon))
                        return false;
                    for (size_t p = prev_code(tokens, at); p != npos; p = prev_code(tokens, p)) {
                        if (kind_is(tokens[p], TK::UseKeyword))
                            return true;
                        if (kind_is(tokens[p], TK::Semicolon) || is_open_block(tokens[p].lex.kind) ||
                            is_close_block(tokens[p].lex.kind))
                            return false;
                    }
                    return false;
                };
                if (is_use_clause_colon(i) || is_use_clause_colon(i - 1))
                    spaces = 0;
            }

            // semicolon_spacing: controls space before/after `;` inside for-loop headers
            // (paren_depth > 0 identifies the for(;;) context vs statement-ending `;`)
            // A brace block's own `;` inside parentheses (`with { a; b; }`)
            // ends a statement, not a header clause.
            auto header_semicolon = [](const Tok& tok) {
                return kind_is(tok, TK::Semicolon) && tok.immutable.syntax.paren_depth > 0 &&
                       !tok.immutable.topology.separates_brace_block_items;
            };
            if (header_semicolon(t))
                spaces = wants_before(opts_.spacing.semicolon_spacing) ? 1 : 0;
            if (header_semicolon(L))
                spaces = wants_after(opts_.spacing.semicolon_spacing) ? 1 : 0;
            // An empty clause adds no padding: `for (;;)`, `for (i = 0;;)`.
            // Without this the `;` beside `(` took its before-space and the
            // one beside `)` did not, or the other way round.
            if ((header_semicolon(t) && (kind_is(L, TK::OpenParenthesis) || header_semicolon(L))) ||
                (header_semicolon(L) && kind_is(t, TK::CloseParenthesis)))
                spaces = 0;

            if ((kind_is(t, TK::Semicolon) && !header_semicolon(t)) ||
                (kind_is(t, TK::Comma) && !kind_is(L, TK::Comma)) ||
                (kind_is(t, TK::Dot) && !dot_spaced) || kind_is(t, TK::DoubleColon))
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
            // ... and the `)` after the implicit event list mirrors that `(`.
            if (kind_is(t, TK::CloseParenthesis) && kind_is(L, TK::Star) &&
                t.immutable.syntax.matching_token == i - 2) {
                const size_t before_open = prev_code(tokens, i - 2);
                if (before_open != npos && kind_is(tokens[before_open], TK::At))
                    spaces = (opts_.spacing.space_inside_parens || opts_.spacing.space_inside_event_control_parens) ? 1 : 0;
            }

            // `{4{a}}` -- the multiplier binds to its replicated braces.
            if (kind_is(t, TK::OpenBrace) && t.immutable.topology.is_replication_brace)
                spaces = 0;

            // Stream concatenation header: `{`, the stream operator, an optional
            // slice size, then the braces holding the operand all bind tightly.
            // This runs last because the operator-spacing rules above would
            // otherwise re-separate `<<` as the binary shift it is spelled like.
            {
                const size_t pc = prev_code(tokens, i);
                const size_t ppc = pc == npos ? npos : prev_code(tokens, pc);
                if (is_stream_operator_at(tokens, i) ||
                    (pc != npos && is_stream_operator_at(tokens, pc)) ||
                    ((kind_is(t, TK::OpenBrace) || kind_is(t, TK::ApostropheOpenBrace)) &&
                     ppc != npos && is_stream_operator_at(tokens, ppc)))
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
            if (is_blank_line_boundary(tokens, i))
                tokens[i].mutable_.blank.before = std::max(0, opts_.blank_lines_between_items);
        }
    }
private:
    const FormatOptions& opts_;
};

} // namespace svfmt
