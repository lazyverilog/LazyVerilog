#include "autoarg.hpp"
#include <regex>
#include <algorithm>

// ── Constants ─────────────────────────────────────────────────────────────────

static const std::regex MODULE_RE(R"(^\s*module\b)", std::regex::icase);
static const std::regex ENDMOD_RE(R"(\bendmodule\b)", std::regex::icase);
static const std::regex MOD_NAME_RE(R"(^\s*module\s+(\w+))", std::regex::icase);
static const std::regex PORT_DIR_RE(R"(^\s*(?:input|output|inout)\b)", std::regex::icase);
static const std::regex ANSI_DIR_RE(R"(\b(?:input|output|inout|ref)\b)", std::regex::icase);

static const std::vector<std::string> TYPE_KWS = {
    "wire", "uwire", "reg", "logic", "bit", "byte",
    "shortint", "int", "longint", "integer", "time",
    "tri", "tri0", "tri1", "wand", "triand", "wor", "trior", "trireg",
    "supply0", "supply1", "signed", "unsigned", "var",
};

static bool is_type_kw(const std::string& tok) {
    std::string lower = tok;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& kw : TYPE_KWS)
        if (lower == kw) return true;
    return false;
}

static bool is_ident(const std::string& tok) {
    if (tok.empty()) return false;
    if (!std::isalpha((unsigned char)tok[0]) && tok[0] != '_') return false;
    for (char c : tok)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// ── Split lines ───────────────────────────────────────────────────────────────

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty())
        lines.push_back({});
    return lines;
}

// ── Scan port names (text-based, matching Python's _scan_port_names) ──────────

static std::vector<std::string> scan_port_names(const std::string& text, int mod_line) {
    auto lines = split_lines(text);
    std::vector<std::string> names;
    std::vector<std::string> seen;

    auto already_seen = [&](const std::string& n) {
        for (const auto& s : seen)
            if (s == n) return true;
        return false;
    };

    for (int i = mod_line; i < (int)lines.size(); ++i) {
        const std::string& raw = lines[i];
        if (std::regex_search(raw, ENDMOD_RE))
            break;
        if (!std::regex_search(raw, PORT_DIR_RE))
            continue;

        // Strip line comment
        std::string code = raw;
        auto comment_pos = code.find("//");
        if (comment_pos != std::string::npos)
            code = code.substr(0, comment_pos);
        // Strip trailing semicolon
        while (!code.empty() && (code.back() == ';' || code.back() == ' ' || code.back() == '\t'))
            code.pop_back();

        // Tokenize: find [...] groups and word tokens
        std::vector<std::string> tokens;
        size_t pos = 0;
        while (pos < code.size()) {
            char ch = code[pos];
            if (std::isspace((unsigned char)ch)) {
                ++pos;
                continue;
            }
            if (ch == '[') {
                size_t end = code.find(']', pos);
                if (end == std::string::npos)
                    end = code.size() - 1;
                tokens.push_back(code.substr(pos, end - pos + 1));
                pos = end + 1;
                continue;
            }
            if (ch == '=') {
                tokens.push_back("=");
                ++pos;
                continue;
            }
            if (ch == ',') {
                tokens.push_back(",");
                ++pos;
                continue;
            }
            if (std::isalnum((unsigned char)ch) || ch == '_') {
                size_t end = pos;
                while (end < code.size() && (std::isalnum((unsigned char)code[end]) || code[end] == '_'))
                    ++end;
                tokens.push_back(code.substr(pos, end - pos));
                pos = end;
                continue;
            }
            ++pos;
        }

        int idx = 0;
        int n = (int)tokens.size();

        // Skip direction keyword
        if (idx < n) {
            std::string lw = tokens[idx];
            std::transform(lw.begin(), lw.end(), lw.begin(), ::tolower);
            if (lw == "input" || lw == "output" || lw == "inout")
                ++idx;
        }

        // Skip built-in type keywords
        while (idx < n && is_type_kw(tokens[idx]))
            ++idx;

        // Skip packed dimensions
        while (idx < n && !tokens[idx].empty() && tokens[idx][0] == '[')
            ++idx;

        // Skip user-defined type: if current ident is followed by another ident
        if (idx < n && is_ident(tokens[idx]) && !is_type_kw(tokens[idx])) {
            int peek = idx + 1;
            while (peek < n && !tokens[peek].empty() && tokens[peek][0] == '[')
                ++peek;
            if (peek < n && is_ident(tokens[peek]) && !is_type_kw(tokens[peek])) {
                ++idx; // skip user-defined type
                while (idx < n && !tokens[idx].empty() && tokens[idx][0] == '[')
                    ++idx;
            }
        }

        // Collect port names
        while (idx < n) {
            const std::string& tok = tokens[idx];
            if (is_ident(tok) && !is_type_kw(tok)) {
                if (!already_seen(tok)) {
                    seen.push_back(tok);
                    names.push_back(tok);
                }
                ++idx;
                // Skip any trailing dimensions
                while (idx < n && !tokens[idx].empty() && tokens[idx][0] == '[')
                    ++idx;
                // Skip default value: = ...
                if (idx < n && tokens[idx] == "=") {
                    ++idx;
                    while (idx < n && tokens[idx] != ",")
                        ++idx;
                }
            } else if (tok == ",") {
                ++idx;
            } else {
                ++idx;
            }
        }
    }
    return names;
}

// ── Main implementation ───────────────────────────────────────────────────────

std::optional<AutoargResult> autoarg_impl(const DocumentState& state, int line, int /*col*/) {
    if (!state.tree)
        return std::nullopt;

    auto lines = split_lines(state.text);
    int n_lines = (int)lines.size();

    // Scan backward from cursor to find 'module'
    int mod_line = -1;
    for (int i = line; i >= 0; --i) {
        if (std::regex_search(lines[i], MODULE_RE)) {
            mod_line = i;
            break;
        }
    }
    if (mod_line < 0)
        return std::nullopt;

    // Scan forward from cursor to find 'endmodule'
    int end_mod_line = -1;
    for (int i = line; i < n_lines; ++i) {
        if (std::regex_search(lines[i], ENDMOD_RE)) {
            end_mod_line = i;
            break;
        }
    }
    if (end_mod_line < 0)
        return std::nullopt;

    // Extract module name
    std::smatch m;
    if (!std::regex_search(lines[mod_line], m, MOD_NAME_RE))
        return std::nullopt;
    std::string module_name = m[1].str();

    // Scan port names
    auto port_names = scan_port_names(state.text, mod_line);
    if (port_names.empty())
        return std::nullopt;

    // Find '(' opening the port list, skipping #(...) parameter block
    int open_line = -1;
    int open_col = -1;
    int skip_depth = 0;
    bool found_hash = false;

    for (int i = mod_line; i <= end_mod_line; ++i) {
        for (int j = 0; j < (int)lines[i].size(); ++j) {
            char ch = lines[i][j];
            if (skip_depth > 0) {
                if (ch == '(') ++skip_depth;
                else if (ch == ')') --skip_depth;
            } else if (found_hash) {
                if (ch == '(') {
                    skip_depth = 1;
                    found_hash = false;
                } else if (ch != ' ' && ch != '\t') {
                    found_hash = false;
                }
            } else {
                if (ch == '#') {
                    found_hash = true;
                } else if (ch == '(') {
                    open_line = i;
                    open_col = j;
                    break;
                }
            }
        }
        if (open_line >= 0) break;
    }
    if (open_line < 0)
        return std::nullopt;

    // Find matching ')'
    int depth = 0;
    int close_line = -1;
    int close_col = -1;
    for (int i = open_line; i < n_lines; ++i) {
        int start_col = (i == open_line) ? open_col : 0;
        for (int j = start_col; j < (int)lines[i].size(); ++j) {
            char ch = lines[i][j];
            if (ch == '(') ++depth;
            else if (ch == ')') {
                --depth;
                if (depth <= 0) {
                    close_line = i;
                    close_col = j;
                    break;
                }
            }
        }
        if (close_line >= 0) break;
    }
    if (close_line < 0)
        return std::nullopt;

    // Skip ANSI-style modules (port list contains direction keywords)
    std::string port_list_text;
    for (int i = open_line; i <= close_line; ++i) {
        int s = (i == open_line) ? open_col : 0;
        int e = (i == close_line) ? close_col + 1 : (int)lines[i].size();
        port_list_text += lines[i].substr(s, e - s);
        if (i < close_line) port_list_text += "\n";
    }
    if (std::regex_search(port_list_text, ANSI_DIR_RE))
        return std::nullopt;

    // Find end range (include the ';' after ')')
    int end_line = close_line;
    int end_col = close_col + 1;
    auto semi_pos = lines[close_line].find(';', close_col);
    if (semi_pos != std::string::npos) {
        end_col = (int)semi_pos + 1;
    } else if (close_line + 1 < n_lines) {
        auto semi2 = lines[close_line + 1].find(';');
        if (semi2 != std::string::npos) {
            end_line = close_line + 1;
            end_col = (int)semi2 + 1;
        }
    }

    AutoargResult result;
    result.port_names = std::move(port_names);
    result.module_name = module_name;
    result.open_line = open_line;
    result.open_col = open_col;
    result.end_line = end_line;
    result.end_col = end_col;
    return result;
}

// ── Format autoarg ────────────────────────────────────────────────────────────

std::string format_autoarg(const AutoargResult& result, const AutoargOptions& /*options*/) {
    std::string indent = "  ";
    const auto& port_names = result.port_names;
    std::string out = "(\n";
    for (size_t i = 0; i < port_names.size(); ++i) {
        std::string comma = (i + 1 < port_names.size()) ? "," : "";
        out += indent + port_names[i] + comma + "\n";
    }
    out += ");";
    return out;
}
