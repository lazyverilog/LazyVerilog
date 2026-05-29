#pragma once

#include "formatter_token.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <slang/diagnostics/Diagnostics.h>
#include <slang/parsing/Lexer.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxKind.h>
#include <slang/text/SourceManager.h>
#include <slang/util/BumpAllocator.h>

namespace svfmt {

inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

inline std::string lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

// Pattern matching is deliberately implemented as deterministic text scanning.
// The documented defaults are pattern-like strings, but this lexer never invokes
// a pattern engine.  Standard markers are recognized directly, and simple
// literal custom patterns are honored by stripping common escaping punctuation.
inline std::string simplified_marker_pattern(std::string_view pattern) {
    std::string out;
    bool escape = false;
    for (char c : pattern) {
        if (escape) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '/') out.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') { escape = true; continue; }
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '/') out.push_back(c);
    }
    return lower_ascii(out);
}

inline bool contains_simple_pattern(std::string_view comment, std::string_view pattern) {
    std::string pat = simplified_marker_pattern(pattern);
    if (pat.empty()) return false;
    return lower_ascii(comment).find(pat) != std::string::npos;
}

inline bool is_format_marker(std::string_view comment, std::string_view configured_pattern) {
    const std::string c = lower_ascii(comment);
    if (contains_simple_pattern(comment, configured_pattern)) return true;
    return false;
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
            if (token.kind == slang::parsing::TokenKind::EndOfFile)
                break;

            // Slang comments and preprocessor directives can arrive as leading
            // trivia.  Promote comments to ordinary formatter tokens so every
            // pass sees one immutable linear token stream.
            for (const auto& trivia : token.trivia())
                collect_trivia(trivia);

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
<<<<<<< HEAD
=======

>>>>>>> fdf6ad2 (rewrite formatter)
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
                   bool is_comment, bool is_directive, bool whitespace_sensitive) {
        auto lex = std::make_shared<LexemeFacts>();
        lex->kind = kind;
        lex->text.assign(text);
        lex->lower_text = lower_ascii(text);
        lex->range = slang::SourceRange(slang::SourceLocation(slang::BufferID::getPlaceholder(), pos),
                                        slang::SourceLocation(slang::BufferID::getPlaceholder(), pos + text.size()));
        lex->is_comment = is_comment;
        lex->is_directive = is_directive;
        lex->is_whitespace_sensitive = whitespace_sensitive;

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

    void collect_trivia(const slang::parsing::Trivia& trivia) {
        using TK = slang::parsing::TokenKind;
        using TV = slang::parsing::TriviaKind;
        std::string_view raw = trivia.getRawText();
        if (raw.empty()) return;
        size_t pos = find_from_cursor(raw);
        consume_gap_to(pos);

        if (trivia.kind == TV::LineComment || trivia.kind == TV::BlockComment) {
            add_token(TK::Unknown, raw, pos, true, false, disabled_);
            if (is_format_marker(raw, opts_.format_off_comment_pattern)) disabled_ = true;
            if (is_format_marker(raw, opts_.format_on_comment_pattern)) disabled_ = false;
        }
        // Whitespace trivia is not a token.  It only contributes immutable source
        // layout facts used by passes such as WrapPass and BlankLinePass.
        consume_text(raw, true);
    }

    void add_slang_token(const slang::parsing::Token& token) {
        std::string_view raw = token.rawText();
        size_t pos = token.location().valid() ? token.location().offset() : find_from_cursor(raw);
        consume_gap_to(pos);
        bool directive = token.kind == slang::parsing::TokenKind::Directive;
        add_token(token.kind, raw, pos, false, directive, false);
        consume_text(raw, false);
    }

    void add_raw_until_token(const slang::parsing::Token& token) {
        std::string_view raw = token.rawText();
        size_t pos = token.location().valid() ? token.location().offset() : find_from_cursor(raw);
        consume_gap_to(pos);
        add_token(token.kind, raw, pos, false, token.kind == slang::parsing::TokenKind::Directive, true);
        consume_text(raw, false);
    }
};

} // namespace svfmt
