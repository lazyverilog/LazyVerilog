#pragma once

#include "formatter_token.hpp"
#include "../config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include <slang/parsing/LexerFacts.h>

namespace svfmt {

// -----------------------------------------------------------------------------
// Formatter logging utilities
// -----------------------------------------------------------------------------
// These helpers are intentionally read-only.  They are not formatting passes,
// they own no metadata, and they must never affect formatter decisions.  Their
// only side effect is writing diagnostic text under FormatOptions::log_path.

inline void write_log(const FormatOptions& opts, const std::string& filename, const std::string& text) {
    if (opts.log_path.empty())
        return;

    std::filesystem::create_directories(opts.log_path);
    std::ofstream out(std::filesystem::path(opts.log_path) / filename);
    out << text;
}

inline const char* comment_role_name(CommentRole role) {
    switch (role) {
    case CommentRole::None: return "None";
    case CommentRole::Leading: return "Leading";
    case CommentRole::Trailing: return "Trailing";
    case CommentRole::OwnLine: return "OwnLine";
    case CommentRole::InterstitialLeading: return "InterstitialLeading";
    case CommentRole::InterstitialTrailing: return "InterstitialTrailing";
    case CommentRole::Detached: return "Detached";
    case CommentRole::PreprocessorAdjacent: return "PreprocessorAdjacent";
    }
    return "<unknown-comment-role>";
}

inline std::string token_kind_name(slang::parsing::TokenKind kind) {
    std::string_view spelling = slang::parsing::LexerFacts::getTokenKindText(kind);
    if (!spelling.empty())
        return std::string(spelling);

    // Slang intentionally does not have human spellings for every token kind
    // (for example identifiers and literals).  Keep the numeric enum value in
    // the log so unknown/new TokenKind values are still debuggable without a
    // giant hand-maintained switch.
    std::ostringstream out;
    out << "TokenKind(" << static_cast<int>(kind) << ")";
    return out.str();
}

inline std::string escaped_for_log(std::string_view text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        case '"': out << "\\\""; break;
        default:
            if (std::isprint(c)) {
                out << static_cast<char>(c);
            }
            else {
                out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(c) << std::dec << std::setfill(' ');
            }
            break;
        }
    }
    return out.str();
}

inline std::string npos_or_index(size_t value) {
    if (value == npos)
        return "npos";
    return std::to_string(value);
}

inline const char* bool01(bool value) {
    return value ? "1" : "0";
}

inline std::string token_stream_to_log(const TokenStream& tokens) {
    std::ostringstream out;

    out << "TokenStream dump\n";
    out << "token_count: " << tokens.size() << "\n";
    out << "note: this file is diagnostic only; renderer output must not depend on it.\n\n";

    for (size_t i = 0; i < tokens.size(); ++i) {
        const Tok& tok = tokens[i];
        const LexemeFacts& lex = *tok.lex;
        const ImmutableData& imm = tok.immutable;
        const MutableData& mut = tok.mutable_;

        out << "[" << i << "]\n";

        // LexemeFacts: immutable lexer identity.
        out << "  lex.kind: " << token_kind_name(lex.kind) << "\n";
        out << "  lex.kind_value: " << static_cast<int>(lex.kind) << "\n";
        out << "  lex.text: \"" << escaped_for_log(lex.text) << "\"\n";
        out << "  lex.lower_text: \"" << escaped_for_log(lex.lower_text) << "\"\n";
        out << "  lex.range: " << lex.range.start().offset() << ".." << lex.range.end().offset() << "\n";
        out << "  lex.is_comment: " << bool01(lex.is_comment) << "\n";
        out << "  lex.is_directive: " << bool01(lex.is_directive) << "\n";
        out << "  lex.is_whitespace_sensitive: " << bool01(lex.is_whitespace_sensitive) << "\n";

        // SyntaxFacts: parser-ish stable facts.
        out << "  immutable.syntax.matching_token: " << npos_or_index(imm.syntax.matching_token) << "\n";
        out << "  immutable.syntax.stmt_begin: " << npos_or_index(imm.syntax.stmt_begin) << "\n";
        out << "  immutable.syntax.stmt_end: " << npos_or_index(imm.syntax.stmt_end) << "\n";
        out << "  immutable.syntax.parent_construct: " << npos_or_index(imm.syntax.parent_construct) << "\n";
        out << "  immutable.syntax.depths: paren=" << imm.syntax.paren_depth
            << " bracket=" << imm.syntax.bracket_depth << " brace=" << imm.syntax.brace_depth << "\n";
        out << "  immutable.syntax.in_function_decl: " << bool01(imm.syntax.in_function_decl) << "\n";
        out << "  immutable.syntax.in_task_decl: " << bool01(imm.syntax.in_task_decl) << "\n";
        out << "  immutable.syntax.in_class_decl: " << bool01(imm.syntax.in_class_decl) << "\n";
        out << "  immutable.syntax.in_covergroup: " << bool01(imm.syntax.in_covergroup) << "\n";
        out << "  immutable.syntax.in_modport: " << bool01(imm.syntax.in_modport) << "\n";

        // TopologyFacts: stable graph-ish labels.
        out << "  immutable.topology.begins_line_construct: " << bool01(imm.topology.begins_line_construct) << "\n";
        out << "  immutable.topology.ends_line_construct: " << bool01(imm.topology.ends_line_construct) << "\n";
        out << "  immutable.topology.opens_indent_scope: " << bool01(imm.topology.opens_indent_scope) << "\n";
        out << "  immutable.topology.closes_indent_scope: " << bool01(imm.topology.closes_indent_scope) << "\n";
        out << "  immutable.topology.starts_argument_list: " << bool01(imm.topology.starts_argument_list) << "\n";
        out << "  immutable.topology.ends_argument_list: " << bool01(imm.topology.ends_argument_list) << "\n";
        out << "  immutable.topology.starts_parameter_list: " << bool01(imm.topology.starts_parameter_list) << "\n";
        out << "  immutable.topology.starts_port_list: " << bool01(imm.topology.starts_port_list) << "\n";

        // CommentFacts: early-frozen comment role and anchor.
        out << "  immutable.comment.role: " << comment_role_name(imm.comment.role) << "\n";
        out << "  immutable.comment.anchor_token: " << npos_or_index(imm.comment.anchor_token) << "\n";
        out << "  immutable.comment.inside_expression: " << bool01(imm.comment.inside_expression) << "\n";
        out << "  immutable.comment.inside_arg_list: " << bool01(imm.comment.inside_arg_list) << "\n";

        // InputTriviaFacts: observation of original whitespace only.
        out << "  immutable.input_trivia.original_spaces_before: " << imm.input_trivia.original_spaces_before << "\n";
        out << "  immutable.input_trivia.original_newlines_before: " << imm.input_trivia.original_newlines_before << "\n";
        out << "  immutable.input_trivia.original_indent: " << imm.input_trivia.original_indent << "\n";
        out << "  immutable.input_trivia.starts_original_line: " << bool01(imm.input_trivia.starts_original_line) << "\n";
        out << "  immutable.input_trivia.original_column: " << imm.input_trivia.original_column << "\n";

        // MutableData: semantic formatting intent, emitted as diagnostics only.
        out << "  mutable.wrap: can_before=" << bool01(mut.wrap.can_break_before)
            << " must_before=" << bool01(mut.wrap.must_break_before)
            << " can_after=" << bool01(mut.wrap.can_break_after)
            << " must_after=" << bool01(mut.wrap.must_break_after)
            << " continuation=" << bool01(mut.wrap.continuation)
            << " group=" << mut.wrap.wrap_group << "\n";
        out << "  mutable.indent: base=" << mut.indent.base_indent
            << " continuation=" << mut.indent.continuation_indent
            << " anchor=" << npos_or_index(mut.indent.anchor_token) << "\n";
        out << "  mutable.align: enabled=" << bool01(mut.align.enabled)
            << " target_column=" << mut.align.target_column
            << " group=" << mut.align.alignment_group << "\n";
        out << "  mutable.space: spaces_before=" << mut.space.spaces_before
            << " suppress=" << bool01(mut.space.suppress_space) << "\n";
        out << "  mutable.comment: preserve_internal_indent=" << bool01(mut.comment.preserve_internal_indent)
            << " force_own_line=" << bool01(mut.comment.force_own_line)
            << " relative_indent=" << mut.comment.relative_indent << "\n";
        out << "  mutable.blank: before=" << mut.blank.before << "\n";
        out << "  mutable.macro: passthrough=" << bool01(mut.macro.passthrough)
            << " suppress_alignment=" << bool01(mut.macro.suppress_alignment)
            << " suppress_wrapping=" << bool01(mut.macro.suppress_wrapping) << "\n";
        out << "\n";
    }

    return out.str();
}

inline void write_log(const FormatOptions& opts, const std::string& filename, const TokenStream& tokens) {
    if (opts.log_path.empty())
        return;
    write_log(opts, filename, token_stream_to_log(tokens));
}

} // namespace svfmt
