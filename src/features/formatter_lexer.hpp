#pragma once

#include "formatter_token.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <string_view>
#include <slang/diagnostics/Diagnostics.h>
#include <slang/parsing/Lexer.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxKind.h>
#include <slang/text/SourceManager.h>
#include <slang/util/BumpAllocator.h>

namespace svfmt {

inline std::string lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

inline bool is_format_marker(std::string_view comment, const std::string& configured_pattern) {
    if (configured_pattern.empty()) return false;
    try {
        std::regex re(configured_pattern, std::regex::ECMAScript | std::regex::icase);
        return std::regex_search(std::string(comment), re);
    } catch (const std::regex_error&) {
        // Malformed pattern — fall back to literal substring search
        return std::string(comment).find(configured_pattern) != std::string::npos;
    }
}

class TokenCollector {
public:
    TokenCollector(const std::string& source, const FormatOptions& opts) : source_(source), opts_(opts) {}

    TokenStream collect() {
        slang::SourceManager sm;
        slang::BumpAllocator alloc;
        slang::Diagnostics diagnostics;
        auto buffer = sm.assignText(source_);
        slang::parsing::Lexer lexer(buffer, alloc, diagnostics, sm);

        while (true) {
            slang::parsing::Token token = lexer.lex();
            if (token.kind == slang::parsing::TokenKind::EndOfFile) {
                // Slang can attach trivia that appears after the final real
                // token to the EOF token.  This matters for common end labels:
                //
                //   endmodule  // my_module
                //
                // If we break immediately on EOF, that trailing comment is
                // never promoted into the formatter token stream and safe mode
                // correctly reports a non-whitespace deletion.  EOF itself is
                // not rendered, but its trivia must be collected first.
                size_t token_pos = token.location().valid() ? token.location().offset()
                                                            : source_.size();
                token_pos = std::min(token_pos, source_.size());
                size_t trivia_total = 0;
                for (const auto& trivia : token.trivia())
                    trivia_total += trivia.getRawText().size();
                size_t trivia_pos = token_pos >= trivia_total ? token_pos - trivia_total : cursor_;
                for (const auto& trivia : token.trivia()) {
                    collect_trivia(trivia, trivia_pos);
                    trivia_pos += trivia.getRawText().size();
                }
                consume_gap_to(source_.size());
                break;
            }

            // Skip tokens that are inside a multiline define block already
            // consumed as a single passthrough token.
            if (token.location().valid() &&
                token.location().offset() < passthrough_end_)
                continue;

            // Slang comments and preprocessor directives can arrive as leading
            // trivia.  Promote comments to ordinary formatter tokens so every
            // pass sees one immutable linear token stream.
            //
            // Important: trivia does not always carry an explicit location, but
            // it is stored immediately before the parent token in source order.
            // Derive each trivia offset from the parent token offset instead of
            // searching for the raw text.  Searching is ambiguous for repeated
            // comments and, worse, can make cursor accounting drift so rendered
            // output drops or duplicates non-whitespace text.
            size_t token_pos = token.location().valid() ? token.location().offset()
                                                        : find_from_cursor(token.rawText());
            size_t trivia_total = 0;
            for (const auto& trivia : token.trivia())
                trivia_total += trivia.getRawText().size();
            size_t trivia_pos = token_pos >= trivia_total ? token_pos - trivia_total : cursor_;
            for (const auto& trivia : token.trivia()) {
                collect_trivia(trivia, trivia_pos);
                trivia_pos += trivia.getRawText().size();
            }

            if (disabled_) {
                add_raw_until_token(token);
            } else {
                add_slang_token(token);
            }
        }
        return tokens_;
    }

private:
    const std::string& source_;
    const FormatOptions& opts_;
    TokenStream tokens_;
    size_t cursor_{0};
    bool disabled_{false};
    bool just_entered_disabled_region_{false};
    size_t passthrough_end_{0}; // end of a frozen multiline define block
    int line_{0};
    int col_{0};
    int pending_spaces_{0};
    int pending_newlines_{0};
    int pending_indent_{0};

    void consume_text(std::string_view text, bool trivia) {
        for (char c : text) {
            ++cursor_;
            if (c == '\n') {
                ++line_;
                col_ = 0;
                if (trivia) {
                    ++pending_newlines_;
                    pending_spaces_ = 0;
                    pending_indent_ = 0;
                }
            }
            else {
                ++col_;
                if (trivia && (c == ' ' || c == '\t')) {
                    ++pending_spaces_;
                    ++pending_indent_;
                }
            }
        }
    }

    size_t find_from_cursor(std::string_view text) const {
        if (text.empty()) return cursor_;
        size_t found = source_.find(std::string(text), cursor_);
        return found == std::string::npos ? cursor_ : found;
    }

    void consume_gap_to(size_t pos) {
        if (pos <= cursor_) return;
        consume_text(std::string_view(source_).substr(cursor_, pos - cursor_), true);
    }

    void add_token(slang::parsing::TokenKind kind, std::string_view text, size_t pos,
                   bool is_comment, bool is_directive, bool whitespace_sensitive,
                   CommentLexemeKind comment_kind = CommentLexemeKind::None,
                   bool is_format_off_marker = false,
                   bool is_format_on_marker = false) {
        auto lex = std::make_shared<LexemeFacts>();
        lex->kind = kind;
        lex->text.assign(text);
        lex->lower_text = lower_ascii(text);
        lex->range = slang::SourceRange(slang::SourceLocation(slang::BufferID::getPlaceholder(), pos),
                                        slang::SourceLocation(slang::BufferID::getPlaceholder(), pos + text.size()));
        lex->is_comment = is_comment;
        lex->is_directive = is_directive;
        lex->is_whitespace_sensitive = whitespace_sensitive;
        lex->comment_kind = comment_kind;
        lex->is_format_off_marker = is_format_off_marker;
        lex->is_format_on_marker = is_format_on_marker;

        Tok tok;
        tok.lex = std::move(lex);
        tok.immutable.input_trivia.original_spaces_before = pending_newlines_ > 0 ? pending_indent_ : pending_spaces_;
        tok.immutable.input_trivia.original_newlines_before = pending_newlines_;
        tok.immutable.input_trivia.original_indent = pending_newlines_ > 0 ? pending_indent_ : col_;
        tok.immutable.input_trivia.starts_original_line = pending_newlines_ > 0 || tokens_.empty();
        tok.immutable.input_trivia.original_column = col_;
        tokens_.push_back(std::move(tok));

        pending_spaces_ = 0;
        pending_newlines_ = 0;
        pending_indent_ = 0;
    }

    void collect_trivia(const slang::parsing::Trivia& trivia, size_t pos) {
        using TK = slang::parsing::TokenKind;
        using TV = slang::parsing::TriviaKind;
        std::string_view raw = trivia.getRawText();
        if (raw.empty()) return;
        if (pos + raw.size() <= cursor_)
            return;
        if (pos < cursor_) {
            raw.remove_prefix(cursor_ - pos);
            pos = cursor_;
            if (raw.empty()) return;
        }

        if (disabled_) {
            size_t end = pos + raw.size();
            consume_disabled_region_boundary_newline(end);
            std::string_view raw_chunk(source_.data() + cursor_, end - cursor_);
            const bool is_comment = trivia.kind == TV::LineComment || trivia.kind == TV::BlockComment;
            const CommentLexemeKind comment_kind =
                trivia.kind == TV::LineComment ? CommentLexemeKind::Line :
                trivia.kind == TV::BlockComment ? CommentLexemeKind::Block :
                CommentLexemeKind::None;
            const bool format_on = is_comment && is_format_marker(raw, opts_.format_on_comment_pattern);
            if (!raw_chunk.empty())
                add_token(TK::Unknown, raw_chunk, cursor_, is_comment, false, true,
                          comment_kind, false, format_on);
            if (trivia.kind == TV::LineComment || trivia.kind == TV::BlockComment) {
                if (format_on)
                    disabled_ = false;
            }
            consume_text(raw_chunk, false);
            return;
        }

        consume_gap_to(pos);

        if (trivia.kind == TV::LineComment || trivia.kind == TV::BlockComment) {
            bool format_off = is_format_marker(raw, opts_.format_off_comment_pattern);
            bool format_on = is_format_marker(raw, opts_.format_on_comment_pattern);
            const CommentLexemeKind comment_kind =
                trivia.kind == TV::LineComment ? CommentLexemeKind::Line : CommentLexemeKind::Block;
            add_token(TK::Unknown, raw, pos, true, false, disabled_ || format_off || format_on,
                      comment_kind, format_off, format_on);
            if (format_off) {
                disabled_ = true;
                just_entered_disabled_region_ = true;
            }
            if (format_on) disabled_ = false;
        }
        // Whitespace trivia is not a token.  It only contributes immutable source
        // layout facts used by passes such as WrapPass and BlankLinePass.
        consume_text(raw, true);
    }

    // Returns the source offset one-past-the-end of a multiline `define block
    // that starts at 'start', or 0 if the define is a single-line define.
    size_t find_multiline_define_end(size_t start) const {
        size_t src_size = source_.size();
        // Find end of first line and check for backslash continuation.
        size_t eol = source_.find('\n', start);
        if (eol == std::string::npos) return 0;
        size_t check = eol;
        while (check > start && (source_[check - 1] == ' ' || source_[check - 1] == '\t'))
            --check;
        if (check == start || source_[check - 1] != '\\') return 0;

        // Walk continuation lines.
        size_t pos = eol + 1;
        while (pos < src_size) {
            size_t next_eol = source_.find('\n', pos);
            size_t line_end = (next_eol == std::string::npos) ? src_size : next_eol;
            size_t chk = line_end;
            while (chk > pos && (source_[chk - 1] == ' ' || source_[chk - 1] == '\t'))
                --chk;
            bool has_cont = (chk > pos && source_[chk - 1] == '\\');
            pos = (next_eol == std::string::npos) ? src_size : next_eol + 1;
            if (!has_cont) break;
        }
        return pos;
    }

    void add_slang_token(const slang::parsing::Token& token) {
        std::string_view raw = token.rawText();
        size_t pos = token.location().valid() ? token.location().offset() : find_from_cursor(raw);
        consume_gap_to(pos);

        slang::parsing::TokenKind kind = token.kind;
        bool directive = (kind == slang::parsing::TokenKind::Directive);

        // Freeze multiline `define blocks as a single passthrough token so
        // no formatting pass can rewrite internal spacing or newlines.
        if (directive && token.directiveKind() == slang::syntax::SyntaxKind::DefineDirective) {
            size_t define_end = find_multiline_define_end(pos);
            if (define_end > pos) {
                std::string_view define_raw(source_.data() + pos, define_end - pos);
                add_token(slang::parsing::TokenKind::Directive, define_raw, pos,
                          false, true, /*whitespace_sensitive=*/true);
                consume_text(define_raw, false);
                passthrough_end_ = define_end;
                return;
            }
        }

        // Remap user macro invocations (kind==Directive, directiveKind==MacroUsage) to
        // TokenKind::MacroUsage so downstream passes can distinguish them from PP
        // directives (ifdef/endif/else/define/…) that must stay on their own line.
        if (directive && token.directiveKind() == slang::syntax::SyntaxKind::MacroUsage) {
            kind = slang::parsing::TokenKind::MacroUsage;
            directive = false;
        }

        // Keep preprocessor directive lines atomic.  Slang lexes directive
        // keywords and directive operands as separate tokens; formatting them
        // independently can change non-whitespace content (`ifdef FOO` ->
        // ``ifdef\nFOO`).  Macro usages were remapped above and intentionally
        // remain ordinary tokens.
        if (directive) {
            size_t line_end = source_.find('\n', pos);
            if (line_end == std::string::npos)
                line_end = source_.size();
            std::string_view directive_raw(source_.data() + pos, line_end - pos);
            add_token(kind, directive_raw, pos, false, true, false);
            consume_text(directive_raw, false);
            passthrough_end_ = line_end;
            return;
        }

        add_token(kind, raw, pos, false, directive, false);
        consume_text(raw, false);
    }

    void add_raw_until_token(const slang::parsing::Token& token) {
        std::string_view raw = token.rawText();
        size_t pos = token.location().valid() ? token.location().offset() : find_from_cursor(raw);
        size_t end = pos + raw.size();
        if (end < cursor_)
            return;
        consume_disabled_region_boundary_newline(end);
        std::string_view raw_chunk(source_.data() + cursor_, end - cursor_);
        if (raw_chunk.empty())
            return;
        add_token(token.kind, raw_chunk, cursor_, false,
                  token.kind == slang::parsing::TokenKind::Directive, true);
        consume_text(raw_chunk, false);
    }

    void consume_disabled_region_boundary_newline(size_t end) {
        if (!just_entered_disabled_region_ || cursor_ >= end)
            return;

        // The format-off marker comment itself is still formatted normally and
        // WrapPass emits the line break after that comment.  The disabled raw
        // passthrough region starts immediately after the marker's source text,
        // so the first physical line ending belongs to the marker/region
        // boundary rather than to the raw disabled payload.  Consume exactly one
        // such boundary newline here, at collection time, instead of teaching
        // the renderer to inspect neighboring token semantics and delete bytes
        // from immutable token text.
        if (source_[cursor_] == '\r' && cursor_ + 1 < end && source_[cursor_ + 1] == '\n')
            consume_text(std::string_view(source_).substr(cursor_, 2), false);
        else if (source_[cursor_] == '\n')
            consume_text(std::string_view(source_).substr(cursor_, 1), false);
        just_entered_disabled_region_ = false;
    }
};

} // namespace svfmt
