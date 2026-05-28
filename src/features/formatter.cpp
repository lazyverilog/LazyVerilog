#include "formatter.hpp"
#include "formatter_lexer.hpp"
#include "formatter_renderer.hpp"
#include "formatter_log.hpp"

namespace svfmt {

inline void verify_safe_mode_unchanged(const std::string& source, const std::string& formatted,
                                       const FormatOptions& opts) {
    if (!opts.safe_mode) return;
    if (strip_ws(source) != strip_ws(formatted))
        throw SafeModeError("Formatter safe-mode: non-whitespace content changed — formatting aborted");
}

} // namespace svfmt

std::string format_source(const std::string& source, const FormatOptions& opts) {
    svfmt::write_log(opts, "format_source_input.sv", source);

    svfmt::TokenStream tokens = svfmt::TokenCollector(source, opts).collect();
    svfmt::write_log(opts, "00_input.sv", source);
    svfmt::write_log(opts, "01_token_stream_collected.log", tokens);

    // Required pass DAG.  Each pass has exclusive write ownership of one
    // metadata family and may only read lexemes plus upstream metadata.
    svfmt::SyntaxPass syntax; syntax.run(tokens);
    svfmt::MacroPass macro; macro.run(tokens);
    svfmt::WrapPass wrap(opts); wrap.run(tokens);
    svfmt::IndentPass indent(opts); indent.run(tokens);
    svfmt::AlignPass align(opts); align.run(tokens);
    svfmt::CommentPass comment; comment.run(tokens);
    svfmt::SpacingPass spacing(opts); spacing.run(tokens);
    svfmt::BlankLinePass blank(opts); blank.run(tokens);

    svfmt::write_log(opts, "98_token_stream_final.log", tokens);
    std::string out = svfmt::render_tokens(tokens);
    svfmt::write_log(opts, "99_output.sv", out);
    svfmt::verify_safe_mode_unchanged(source, out, opts);
    return out;
}
