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
        if (comparable_token_text(x) != comparable_token_text(y))
            return false;
        if (x.lex.comment_kind != y.lex.comment_kind)
            return false;
        if (x.lex.is_directive != y.lex.is_directive)
            return false;
        if (x.lex.directive_kind != y.lex.directive_kind)
            return false;
        if (x.lex.is_whitespace_sensitive != y.lex.is_whitespace_sensitive)
            return false;
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

    // Required pass DAG.  Each pass has exclusive write ownership of one
    // metadata family and may only read lexemes plus upstream metadata.
    svfmt::SyntaxPass syntax; syntax.run(tokens);
    svfmt::MacroPass macro(opts); macro.run(tokens);
    svfmt::WrapPass wrap(opts); wrap.run(tokens);
    svfmt::CommentPass comment; comment.run(tokens);
    svfmt::SpacingPass spacing(opts); spacing.run(tokens);
    svfmt::IndentPass indent(opts); indent.run(tokens);
    svfmt::AlignPass align(opts); align.run(tokens);
    svfmt::BlankLinePass blank(opts); blank.run(tokens);

    svfmt::write_log(opts, "98_token_stream_final.log", tokens);
    std::string out = svfmt::render_tokens(tokens);
    svfmt::write_log(opts, "99_output.sv", out);
    svfmt::verify_token_stream_safety_unchanged(before_tokens, out, opts);
    return out;
}
