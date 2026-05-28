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

inline std::string render_tokens(const TokenStream& tokens) {
    std::string out;
    int col = 0;
    bool at_line_start = true;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Tok& tok = tokens[i];
        const RenderDecision d = compose(tok);

        // Passthrough tokens are emitted verbatim.  This is used for disabled
        // regions and whitespace-sensitive macro bodies.  The renderer still
        // owns the surrounding newline decision.
        if (d.passthrough) {
            if (!at_line_start) { trim_trailing_spaces(out); out.push_back('\n'); }
            for (int b = 0; b < d.blank_lines; ++b) out.push_back('\n');
            out += tok.lex->text;
            col = static_cast<int>(tok.lex->text.size());
            at_line_start = false;
            continue;
        }

        if (i > 0 && (d.newline_before || d.blank_lines > 0) && !at_line_start) {
            trim_trailing_spaces(out);
            out.push_back('\n');
            for (int b = 0; b < d.blank_lines; ++b) out.push_back('\n');
            col = 0;
            at_line_start = true;
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

inline std::string strip_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) if (!std::isspace(c)) out.push_back(static_cast<char>(c));
    return out;
}

} // namespace svfmt
