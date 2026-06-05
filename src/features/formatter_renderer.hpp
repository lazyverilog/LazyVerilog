#pragma once

#include "formatter_passes.hpp"
#include <cctype>
#include <stdexcept>

namespace svfmt {

struct RenderDecision {
    bool newline_before{false};
    int indent{0};
    int spaces_before{1};
    int blank_lines{0};
    bool passthrough{false};
    int align_target{-1};
};

// Compose is the only place where semantic formatting metadata becomes concrete
// whitespace intent.  Individual passes never append spaces or newlines.
inline RenderDecision compose(const Tok& tok) {
    RenderDecision out;
    out.newline_before = tok.mutable_.wrap.must_break_before || tok.mutable_.comment.force_own_line;
    out.indent = tok.mutable_.indent.base_indent + tok.mutable_.indent.continuation_indent + tok.mutable_.comment.relative_indent;
    out.spaces_before = tok.mutable_.space.suppress_space ? 0 : tok.mutable_.space.spaces_before;
    out.blank_lines = tok.mutable_.blank.before;
    out.passthrough = is_passthrough(tok);
    out.align_target = tok.mutable_.align.enabled ? tok.mutable_.align.target_column : -1;
    return out;
}

inline void trim_trailing_spaces(std::string& out) {
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
}

inline size_t last_newline_offset(std::string_view text) {
    for (size_t n = text.size(); n > 0; --n)
        if (text[n - 1] == '\n')
            return n - 1;
    return std::string_view::npos;
}

inline std::string render_tokens(const TokenStream& tokens) {
    std::string out;
    size_t token_text_bytes = 0;
    for (const Tok& tok : tokens)
        token_text_bytes += tok.lex->text.size();
    // Rendering appends every immutable token text plus formatter-owned
    // whitespace.  Large generated register files can easily contain hundreds
    // of thousands of small tokens; without a reserve(), std::string growth
    // repeatedly reallocates and copies the partially-rendered output.  Reserve
    // the exact token-text payload plus a conservative whitespace allowance so
    // rendering remains linear in the final output size.
    out.reserve(token_text_bytes + tokens.size());
    int col = 0;
    bool at_line_start = true;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Tok& tok = tokens[i];
        const RenderDecision d = compose(tok);

        if (i > 0 && d.newline_before && !at_line_start) {
            trim_trailing_spaces(out);
            out.push_back('\n');
            col = 0;
            at_line_start = true;
        }
        if (i > 0 && d.blank_lines > 0) {
            if (!at_line_start) {
                trim_trailing_spaces(out);
                out.push_back('\n');
                col = 0;
                at_line_start = true;
            }
            for (int b = 0; b < d.blank_lines; ++b) out.push_back('\n');
        }

        // Passthrough tokens are emitted verbatim.  This is used for disabled
        // regions and whitespace-sensitive macro bodies.  The renderer still
        // owns the surrounding newline decision.
        if (d.passthrough) {
            std::string_view text(tok.lex->text);
            out.append(text.data(), text.size());
            size_t last_nl = last_newline_offset(text);
            if (last_nl == std::string::npos) {
                col += static_cast<int>(text.size());
                at_line_start = false;
            } else {
                col = static_cast<int>(text.size() - last_nl - 1);
                at_line_start = col == 0;
            }
            if (tok.mutable_.wrap.must_break_after && !at_line_start) {
                trim_trailing_spaces(out);
                out.push_back('\n');
                col = 0;
                at_line_start = true;
            }
            continue;
        }

        if (at_line_start) {
            out.append(std::max(0, d.indent), ' ');
            col = std::max(0, d.indent);
            at_line_start = false;
        } else {
            int spaces = d.spaces_before;
            if (d.align_target >= 0 && col < d.align_target) spaces = std::max(spaces, d.align_target - col);
            out.append(spaces, ' ');
            col += spaces;
        }

        out += tok.lex->text;
        col += static_cast<int>(tok.lex->text.size());

        if (tok.mutable_.wrap.must_break_after) {
            trim_trailing_spaces(out);
            out.push_back('\n');
            col = 0;
            at_line_start = true;
        }
    }

    trim_trailing_spaces(out);
    while (!out.empty() && out.back() == '\n') out.pop_back();
    out.push_back('\n');
    return out;
}

// Non-allocating whitespace-ignoring equality check.
inline bool ws_equal(const std::string& a, const std::string& b) {
    auto ia = a.begin(), ib = b.begin();
    for (;;) {
        while (ia != a.end() && std::isspace(static_cast<unsigned char>(*ia))) ++ia;
        while (ib != b.end() && std::isspace(static_cast<unsigned char>(*ib))) ++ib;
        if (ia == a.end() && ib == b.end()) return true;
        if (ia == a.end() || ib == b.end()) return false;
        if (*ia != *ib) return false;
        ++ia; ++ib;
    }
}

} // namespace svfmt
