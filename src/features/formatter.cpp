#include "formatter.hpp"
#include "formatter_lexer.hpp"
#include "formatter_renderer.hpp"
#include "formatter_log.hpp"

namespace svfmt {

inline std::string_view strip_trailing_token_whitespace(std::string_view text) {
    // Slang can include line-end padding in token text for comments and
    // preprocessor directives, for example `// note   ` or `` `endif   ``.
    // The renderer trims that padding before emitting the newline. This helper
    // makes the safe-mode comparison ignore only that end-of-token whitespace.
    //
    // Keep the normalization narrow: token interior text is still compared
    // exactly, and TokenKind / directive / comment metadata must still match.
    while (!text.empty()) {
        const unsigned char c = static_cast<unsigned char>(text.back());
        if (!std::isspace(c))
            break;
        text.remove_suffix(1);
    }
    return text;
}

inline std::string_view comparable_token_text(const Tok& token) {
    // Safe mode cares whether formatting changed the tokenization or
    // non-whitespace token content.  Slang includes some line-end padding in
    // token text for comments and directives, and the renderer trims that
    // padding before emitting the newline.  Normalize trailing whitespace for
    // every token so harmless end-of-token padding cleanup does not fail the
    // token-stream check.
    return strip_trailing_token_whitespace(token.lex.text);
}

// A block comment spanning lines has its later lines moved with its first
// one, so the blanks that lead each of those lines are layout, like the
// indentation of any other line.  Everything else in it must be unchanged.
static bool same_block_comment_but_indent(const Tok& x, const Tok& y) {
    if (x.lex.comment_kind != CommentLexemeKind::Block || y.lex.comment_kind != CommentLexemeKind::Block)
        return false;
    const std::string_view a = comparable_token_text(x), b = comparable_token_text(y);
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] != b[j])
            return false;
        const bool line_end = a[i] == '\n';
        ++i;
        ++j;
        if (line_end) {
            while (i < a.size() && (a[i] == ' ' || a[i] == '\t')) ++i;
            while (j < b.size() && (b[j] == ' ' || b[j] == '\t')) ++j;
        }
    }
    return i == a.size() && j == b.size();
}

static bool token_stream_same(const TokenStream& a, const TokenStream& b) {
    // The formatter safety check compares the *lexed* token stream before and
    // after formatting.
    // That means we intentionally ignore whitespace-derived positions and all
    // mutable formatting decisions; the goal is to catch semantic/tokenization
    // changes such as a formatter accidentally deleting or rewriting tokens.
    //
    // One important exception: trailing whitespace at the end of token text is
    // ignored. Slang includes such padding in some token spellings (notably
    // comments and directives), while the renderer trims it at physical line
    // boundaries. Treat that as harmless whitespace cleanup, not a token-stream
    // change.
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.lex.kind != y.lex.kind)
            return false;
        if (comparable_token_text(x) != comparable_token_text(y) &&
            !same_block_comment_but_indent(x, y))
            return false;
        if (x.lex.comment_kind != y.lex.comment_kind)
            return false;
        if (x.lex.is_directive != y.lex.is_directive)
            return false;
        if (x.lex.directive_kind != y.lex.directive_kind)
            return false;
        if (x.lex.is_whitespace_sensitive != y.lex.is_whitespace_sensitive)
            return false;
        if (x.lex.is_escaped_identifier != y.lex.is_escaped_identifier)
            return false;
        // Separating a `?` digit from its literal leaves every token intact
        // and turns `4'b1???` into a conditional; only this fact sees it.
        if (x.lex.continues_vector_literal != y.lex.continues_vector_literal)
            return false;
        if (x.lex.is_table_row != y.lex.is_table_row)
            return false;
        // slang lexes `\` and the newline after it as one token.  One the
        // lexer did not freeze inside a `` `define `` must still end the line
        // it ended, with the next line right after it: a break before the
        // `\` or a blank line after it ends the macro there, and the token
        // stream above cannot tell.
        if (x.lex.kind == slang::parsing::TokenKind::LineContinuation) {
            if (x.immutable.input_trivia.starts_original_line != y.immutable.input_trivia.starts_original_line)
                return false;
            if (i + 1 < a.size() && a[i + 1].immutable.input_trivia.original_newlines_before !=
                                        b[i + 1].immutable.input_trivia.original_newlines_before)
                return false;
        }
    }
    return true;
}

inline void verify_token_stream_safety_unchanged(const TokenStream& before, const std::string& formatted,
                                        const FormatOptions& opts) {
    TokenStream after = TokenCollector(formatted, opts).collect();
    if (!token_stream_same(before, after))
        throw SafeModeError("Formatter safety: token stream changed — formatting aborted");
}

} // namespace svfmt

std::string format_source(const std::string& source, const FormatOptions& opts) {
    svfmt::write_log(opts, "format_source_input.sv", source);

    svfmt::TokenStream tokens = svfmt::TokenCollector(source, opts).collect();
    const svfmt::TokenStream before_tokens = tokens;
    svfmt::write_log(opts, "00_input.sv", source);
    svfmt::write_log(opts, "01_token_stream_collected.log", tokens);

    svfmt::run_format_passes(tokens, opts);

    svfmt::write_log(opts, "98_token_stream_final.log", tokens);
    std::string out = svfmt::render_tokens(tokens);
    svfmt::write_log(opts, "99_output.sv", out);
    svfmt::verify_token_stream_safety_unchanged(before_tokens, out, opts);
    return out;
}

std::optional<FormattedSpan> formatted_span(const std::string& source, const std::string& formatted,
                                            size_t begin, size_t end) {
    auto blank = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    auto line_start = [](const std::string& s, size_t at) {
        const size_t nl = at == 0 ? std::string::npos : s.rfind('\n', at - 1);
        return nl == std::string::npos ? size_t{0} : nl + 1;
    };
    // Offset of the `n`th non-blank character, counted from 0, or npos.
    auto nth = [&](const std::string& s, size_t n) {
        for (size_t i = 0; i < s.size(); ++i)
            if (!blank(s[i]) && n-- == 0)
                return i;
        return std::string::npos;
    };
    auto past_trailing_blanks = [](const std::string& s, size_t at) {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r'))
            ++at;
        return at;
    };
    // Whole lines: from the first one's start to the last one's end.
    end = std::min(end, source.size());
    begin = line_start(source, std::min(begin, end));
    if (line_start(source, end) != end) {
        const size_t nl = source.find('\n', end);
        end = nl == std::string::npos ? source.size() : nl + 1;
    }
    size_t before = 0, inside = 0;
    for (size_t i = 0; i < end; ++i)
        if (!blank(source[i]))
            ++(i < begin ? before : inside);
    if (inside == 0)
        return std::nullopt;
    const size_t s_first = nth(source, before), f_first = nth(formatted, before);
    const size_t s_last = nth(source, before + inside - 1), f_last = nth(formatted, before + inside - 1);
    if (f_first == std::string::npos || f_last == std::string::npos)
        return std::nullopt;
    FormattedSpan span{s_first, s_last + 1, f_first, f_last + 1};
    // The first line's indent is the range's too, unless formatting joined
    // that line onto the one before it, which the range leaves alone.
    const size_t f_line = line_start(formatted, f_first);
    if (std::all_of(formatted.begin() + static_cast<std::ptrdiff_t>(f_line),
                    formatted.begin() + static_cast<std::ptrdiff_t>(f_first), blank)) {
        span.source_begin = line_start(source, s_first);
        span.formatted_begin = f_line;
    }
    // Likewise the last line's trailing blanks, unless the next line was
    // joined onto it.
    const size_t f_tail = past_trailing_blanks(formatted, f_last + 1);
    if (f_tail == formatted.size() || formatted[f_tail] == '\n') {
        span.source_end = past_trailing_blanks(source, s_last + 1);
        span.formatted_end = f_tail;
    }
    return span;
}
