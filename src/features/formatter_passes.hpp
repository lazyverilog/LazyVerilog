#pragma once

#include "formatter_token.hpp"
#include <algorithm>
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
           k == TK::CaseZKeyword || k == TK::RepeatKeyword || k == TK::WaitKeyword;
}
inline bool is_open_block(TK k) {
    return k == TK::BeginKeyword || k == TK::ClassKeyword || k == TK::FunctionKeyword ||
           k == TK::TaskKeyword || k == TK::CaseKeyword || k == TK::CaseXKeyword ||
           k == TK::CaseZKeyword || k == TK::OpenBrace;
}
inline bool is_outer_open(TK k) { return k == TK::ModuleKeyword || k == TK::InterfaceKeyword || k == TK::PackageKeyword; }
inline bool is_close_block(TK k) {
    return k == TK::EndKeyword || k == TK::EndClassKeyword || k == TK::EndFunctionKeyword ||
           k == TK::EndTaskKeyword || k == TK::EndCaseKeyword || k == TK::CloseBrace;
}
inline bool is_outer_close(TK k) { return k == TK::EndModuleKeyword || k == TK::EndInterfaceKeyword || k == TK::EndPackageKeyword; }
inline bool is_assignment_op(TK k) { return k == TK::Equals || k == TK::LessThanEquals || k == TK::PlusEqual || k == TK::MinusEqual; }
inline bool is_binary_op(TK k) {
    return k == TK::Plus || k == TK::Minus || k == TK::Star || k == TK::Slash || k == TK::Percent ||
           k == TK::DoubleEquals || k == TK::ExclamationEquals || k == TK::LessThan || k == TK::GreaterThan ||
           k == TK::LessThanEquals || k == TK::GreaterThanEquals || k == TK::DoubleAnd || k == TK::DoubleOr ||
           k == TK::And || k == TK::Or || k == TK::Xor || k == TK::LeftShift || k == TK::RightShift;
}
inline bool no_space_before(TK k) {
    return k == TK::CloseParenthesis || k == TK::CloseBracket || k == TK::CloseBrace ||
           k == TK::Semicolon || k == TK::Comma || k == TK::Dot || k == TK::DoubleColon ||
           k == TK::PlusColon || k == TK::MinusColon;
}
inline bool no_space_after(TK k) {
    return k == TK::OpenParenthesis || k == TK::OpenBracket || k == TK::OpenBrace ||
           k == TK::Dot || k == TK::DoubleColon || k == TK::Hash || k == TK::Apostrophe;
}
inline bool wants_before(const std::string& mode) { return mode == "before" || mode == "both"; }
inline bool wants_after(const std::string& mode) { return mode == "after" || mode == "both"; }

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
        bool pp = false;
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
            if (kind_is(t, TK::CloseParenthesis))
                t.immutable.topology.ends_argument_list = true;

            if (t.lex->is_comment) {
                t.immutable.comment.role = t.immutable.input_trivia.starts_original_line ? CommentRole::OwnLine : CommentRole::Trailing;
                t.immutable.comment.anchor_token = i == 0 ? npos : i - 1;
                t.immutable.comment.inside_expression = pd > 0 || bd > 0 || brd > 0;
                t.immutable.comment.inside_arg_list = pd > 0;
            }
            if (t.lex->is_directive) {
                if (t.lex->text == "`ifdef" || t.lex->text == "`ifndef" || t.lex->text == "`elsif") pp = true;
                if (t.lex->text == "`endif") pp = false;
            }
            if (kind_is(t, TK::OpenParenthesis)) { parens.push_back(i); ++pd; }
            else if (kind_is(t, TK::CloseParenthesis)) { if (!parens.empty()) { auto j = parens.back(); parens.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } pd = std::max(0, pd - 1); }
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

// MacroPass owns MacroMetadata.  It never changes wrapping or spacing directly;
// it only marks regions/macros that downstream passes must treat conservatively.
class MacroPass final : public IFormatPass {
public:
    const char* name() const override { return "macro"; }
    void run(TokenStream& tokens) override {
        for (auto& t : tokens) {
            if (t.lex->is_whitespace_sensitive) {
                t.mutable_.macro.passthrough = true;
                t.mutable_.macro.suppress_alignment = true;
                t.mutable_.macro.suppress_wrapping = true;
            } else if (t.lex->is_directive || kind_is(t, TK::MacroUsage)) {
                t.mutable_.macro.suppress_alignment = true;
            }
        }
    }
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
            if (t.lex->is_directive || t.immutable.comment.role == CommentRole::OwnLine) t.mutable_.wrap.must_break_before = true;
            if (i > 0 && tokens[i - 1].lex->is_comment && tokens[i - 1].lex->text.rfind("//", 0) == 0) t.mutable_.wrap.must_break_before = true;
            if (kind_is(t, TK::Semicolon)) { t.mutable_.wrap.must_break_after = true; ++group; }
            if (kind_is(t, TK::Comma)) t.mutable_.wrap.can_break_after = true;
            if (is_outer_close(t.lex->kind) || is_close_block(t.lex->kind)) t.mutable_.wrap.must_break_before = true;
            if (opts_.statement.begin_newline && (kind_is(t, TK::BeginKeyword) || kind_is(t, TK::OpenBrace))) t.mutable_.wrap.must_break_before = true;
            if (opts_.statement.wrap_end_else_clauses && kind_is(t, TK::ElseKeyword) && i > 0 && (kind_is(tokens[i - 1], TK::EndKeyword) || kind_is(tokens[i - 1], TK::CloseBrace))) t.mutable_.wrap.must_break_before = true;
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
        for (auto& t : tokens) {
            if (is_close_block(t.lex->kind) || is_outer_close(t.lex->kind)) level = std::max(0, level - 1);
            t.mutable_.indent.base_indent = (is_outer_close(t.lex->kind) ? 0 : level * opts_.indent_size);
            if (t.mutable_.wrap.continuation) t.mutable_.indent.continuation_indent = opts_.indent_size;
            if (is_open_block(t.lex->kind) || (is_outer_open(t.lex->kind) && opts_.default_indent_level_inside_outmost_block > 0)) ++level;
        }
    }
private: const FormatOptions& opts_;
};

// AlignPass owns AlignMetadata.  This initial implementation aligns assignment
// operators per rendered line using token widths, not renderer feedback.
class AlignPass final : public IFormatPass {
public:
    explicit AlignPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "alignment"; }
    void run(TokenStream& tokens) override {
        if (!opts_.statement.align) return;
        int group = 0;
        for (size_t start = 0; start < tokens.size();) {
            size_t end = start + 1;
            while (end < tokens.size() && !tokens[end].mutable_.wrap.must_break_before) ++end;
            for (size_t i = start; i < end; ++i) {
                if (!is_assignment_op(tokens[i].lex->kind) || tokens[i].mutable_.macro.suppress_alignment) continue;
                int lhs = 0;
                for (size_t j = start; j < i; ++j) lhs += token_width(tokens[j]) + (j == start ? 0 : 1);
                int min_col = opts_.statement.lhs_min_width;
                if (opts_.tab_align && opts_.indent_size > 0) min_col = ((min_col + opts_.indent_size - 1) / opts_.indent_size) * opts_.indent_size;
                tokens[i].mutable_.align.enabled = true;
                tokens[i].mutable_.align.target_column = std::max(lhs, min_col);
                tokens[i].mutable_.align.alignment_group = group;
            }
            start = end; ++group;
        }
    }
private: const FormatOptions& opts_;
};

// CommentPass owns CommentMetadata and reads stable syntax comment roles.
class CommentPass final : public IFormatPass { public: const char* name() const override { return "comment"; } void run(TokenStream& tokens) override { for (auto& t : tokens) if (t.lex->is_comment && t.immutable.comment.role == CommentRole::OwnLine) t.mutable_.comment.force_own_line = true; } };

// SpacingPass owns SpaceMetadata.  It reads upstream wrap/comment metadata to
// avoid assigning in-line spaces before tokens that will start a line.
class SpacingPass final : public IFormatPass {
public:
    explicit SpacingPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "spacing"; }
    void run(TokenStream& tokens) override {
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (i == 0 || t.mutable_.wrap.must_break_before || t.mutable_.comment.force_own_line || is_passthrough(t)) { t.mutable_.space.spaces_before = 0; continue; }
            const Tok& L = tokens[i - 1];
            int spaces = 1;
            if (no_space_before(t.lex->kind) || no_space_after(L.lex->kind)) spaces = 0;
            if (kind_is(t, TK::OpenParenthesis) && (kind_is(L, TK::Identifier) || kind_is(L, TK::SystemIdentifier) || kind_is(L, TK::MacroUsage))) spaces = opts_.function.space_before_paren ? 1 : 0;
            if (kind_is(t, TK::OpenParenthesis) && is_control_keyword(L.lex->kind)) spaces = opts_.spacing.control_keyword_space ? 1 : 0;
            if ((kind_is(L, TK::OpenParenthesis) || kind_is(t, TK::CloseParenthesis)) && opts_.spacing.space_inside_parens) spaces = 1;
            if ((kind_is(L, TK::OpenBracket) || kind_is(t, TK::CloseBracket)) && opts_.spacing.space_inside_dimension_brackets) spaces = 1;
            if (is_assignment_op(t.lex->kind)) spaces = wants_before(opts_.spacing.assignment_operator_spacing) ? 1 : 0;
            if (is_assignment_op(L.lex->kind)) spaces = wants_after(opts_.spacing.assignment_operator_spacing) ? 1 : 0;
            bool in_dim = t.immutable.syntax.bracket_depth > 0 || L.immutable.syntax.bracket_depth > 0;
            if (is_binary_op(t.lex->kind)) spaces = wants_before(in_dim ? opts_.spacing.dimension_binary_operator_spacing : opts_.spacing.binary_operator_spacing) ? 1 : 0;
            if (is_binary_op(L.lex->kind)) spaces = wants_after(in_dim ? opts_.spacing.dimension_binary_operator_spacing : opts_.spacing.binary_operator_spacing) ? 1 : 0;
            if (kind_is(t, TK::Colon) && in_dim) spaces = wants_before(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (kind_is(L, TK::Colon) && in_dim) spaces = wants_after(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (kind_is(t, TK::PlusColon) || kind_is(t, TK::MinusColon)) spaces = wants_before(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;
            if (kind_is(L, TK::PlusColon) || kind_is(L, TK::MinusColon)) spaces = wants_after(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;
            if (kind_is(t, TK::At)) spaces = wants_before(opts_.spacing.procedural_event_control_at_spacing) ? 1 : 0;
            if (kind_is(L, TK::At)) spaces = wants_after(opts_.spacing.procedural_event_control_at_spacing) ? 1 : 0;
            t.mutable_.space.spaces_before = std::max(0, spaces);
            t.mutable_.space.suppress_space = spaces == 0;
        }
    }
private: const FormatOptions& opts_;
};

// BlankLinePass owns BlankLineMetadata and clamps source blank lines to config.
class BlankLinePass final : public IFormatPass { public: explicit BlankLinePass(const FormatOptions& opts) : opts_(opts) {} const char* name() const override { return "blank_line"; } void run(TokenStream& tokens) override { for (auto& t : tokens) if (t.immutable.input_trivia.original_newlines_before > 1) t.mutable_.blank.before = std::min(t.immutable.input_trivia.original_newlines_before - 1, std::max(0, opts_.blank_lines_between_items)); } private: const FormatOptions& opts_; };

} // namespace svfmt
