#include "autoff.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <regex>
#include <algorithm>

using namespace slang;
using namespace slang::syntax;

static const std::string DEFAULT_REGISTER_PATTERN = "^r_";

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

// ── Parse declaration signals at a given line ─────────────────────────────────

struct DeclAtLineVisitor : public SyntaxVisitor<DeclAtLineVisitor> {
    const SourceManager& sm;
    int target_line; // 0-based
    std::vector<std::string> names;
    bool matched{false};

    DeclAtLineVisitor(const SourceManager& s, int tl) : sm(s), target_line(tl) {}

    void collect_decl(const SyntaxNode& node) {
        auto tok = node.getFirstToken();
        if (!tok.valid() || !tok.location().valid()) return;
        size_t ln = sm.getLineNumber(tok.location());
        int line_0 = (int)(ln > 0 ? ln - 1 : 0);
        if (line_0 != target_line) return;
        matched = true;
        // Collect declarator names
        for (uint32_t i = 0; i < node.getChildCount(); ++i) {
            const SyntaxNode* child = node.childNode(i);
            if (!child) continue;
            if (const auto* d = child->as_if<DeclaratorSyntax>()) {
                std::string name = std::string(d->name.valueText());
                if (!name.empty()) names.push_back(name);
            }
        }
    }

    void handle(const DataDeclarationSyntax& node) {
        collect_decl(node);
        visitDefault(node);
    }
    void handle(const NetDeclarationSyntax& node) {
        collect_decl(node);
        visitDefault(node);
    }
};

static std::vector<std::string> parse_declaration_signals(const DocumentState& state, int line) {
    if (!state.tree)
        throw std::runtime_error("AutoFF: document could not be parsed");

    auto& sm = state.tree->sourceManager();
    DeclAtLineVisitor v(sm, line);
    state.tree->root().visit(v);

    if (!v.matched)
        throw std::runtime_error("AutoFF: cursor line is not a variable declaration");
    if (v.names.size() != 2)
        throw std::runtime_error("AutoFF: declaration must have exactly 2 signals, found " +
                                 std::to_string(v.names.size()));
    return v.names;
}

// ── Pair signals using register pattern ──────────────────────────────────────

static std::pair<std::string, std::string> pair_signals(
    const std::vector<std::string>& names, const std::regex& reg_re)
{
    std::vector<std::string> regs, srcs;
    for (const auto& n : names) {
        if (std::regex_search(n, reg_re))
            regs.push_back(n);
        else
            srcs.push_back(n);
    }
    if (regs.size() == 1 && srcs.size() == 1)
        return {srcs[0], regs[0]};
    // Fallback: last = registered
    return {names[0], names[1]};
}

// ── Check if signal is already assigned ──────────────────────────────────────

static bool check_already_assigned(const std::string& text, const std::string& signal) {
    bool in_ff = false;
    int depth = 0;
    std::regex lhs_re(R"(^\s*)" + std::regex::basic + signal + R"(\s*<=)");
    // Build proper regex
    std::regex lhs_re2("^\\s*" + std::regex_replace(signal, std::regex(R"([.*+?^${}()|[\]\\])"), R"(\$&)") + "\\s*<=");
    std::regex always_ff_re(R"(\balways_ff\b)");
    std::regex begin_re(R"(\bbegin\b)");
    std::regex end_re(R"(\bend\b)");

    for (const auto& raw_line : split_lines(text)) {
        std::string stripped = raw_line;
        size_t s = stripped.find_first_not_of(" \t");
        if (s != std::string::npos) stripped = stripped.substr(s);
        else stripped.clear();

        if (std::regex_search(stripped, always_ff_re)) {
            in_ff = true;
            depth = 0;
        }
        if (in_ff) {
            // Count begin/end for depth tracking
            for (std::sregex_iterator it(stripped.begin(), stripped.end(), begin_re), end; it != end; ++it)
                ++depth;
            for (std::sregex_iterator it(stripped.begin(), stripped.end(), end_re), end; it != end; ++it)
                --depth;
            if (std::regex_search(raw_line, lhs_re2))
                return true;
            if (depth <= 0 && !std::regex_search(stripped, always_ff_re)) {
                in_ff = false;
                depth = 0;
            }
        }
    }
    return false;
}

// ── Parse always_ff if/else structure ────────────────────────────────────────

struct FfBlock {
    int if_begin_line{-1};
    int if_insert_line{-1};
    std::string if_indent;
    int else_begin_line{-1};
    int else_insert_line{-1};
    std::string else_indent;
};

static void depth_tokens(const std::string& line, int& depth) {
    std::regex begin_end_re(R"(\b(begin|end)\b)");
    for (std::sregex_iterator it(line.begin(), line.end(), begin_end_re), end_it; it != end_it; ++it) {
        if ((*it)[1].str() == "begin") ++depth;
        else --depth;
    }
}

static FfBlock parse_ff_block(const std::vector<std::string>& lines, int ff_start) {
    int n = (int)lines.size();

    // Find outer begin
    int outer_begin = -1;
    std::regex begin_re(R"(\bbegin\b)");
    for (int i = ff_start; i < n; ++i) {
        if (std::regex_search(lines[i], begin_re)) {
            outer_begin = i;
            break;
        }
    }
    if (outer_begin < 0)
        throw std::runtime_error("AutoFF: always_ff block missing 'begin'");

    // Find 'if (' within depth=1
    int depth = 1;
    int if_line = -1;
    std::regex if_re(R"(\bif\s*\()");
    for (int i = outer_begin + 1; i < n; ++i) {
        if (depth == 1 && std::regex_search(lines[i], if_re)) {
            if_line = i;
            break;
        }
        depth_tokens(lines[i], depth);
        if (depth <= 0)
            throw std::runtime_error("AutoFF: always_ff block closed before finding 'if'");
    }
    if (if_line < 0)
        throw std::runtime_error("AutoFF: no 'if' statement found inside always_ff block");

    // Find begin of if-block
    int if_begin = -1;
    for (int i = if_line; i < n; ++i) {
        if (std::regex_search(lines[i], begin_re)) {
            if_begin = i;
            break;
        }
    }
    if (if_begin < 0)
        throw std::runtime_error("AutoFF: if-block inside always_ff is missing 'begin'");

    // Find end of if-block
    int if_depth = 1;
    int if_end = -1;
    for (int i = if_begin + 1; i < n; ++i) {
        depth_tokens(lines[i], if_depth);
        if (if_depth <= 0) {
            if_end = i;
            break;
        }
    }
    if (if_end < 0)
        throw std::runtime_error("AutoFF: if-block 'end' not found");

    // Extract if indent
    std::smatch im;
    std::regex indent_re(R"(^(\s*))");
    std::string if_indent = "    ";
    if (std::regex_search(lines[if_begin], im, indent_re))
        if_indent = im[1].str() + "    ";

    // Find else begin
    int else_begin = -1;
    std::regex else_re(R"(\belse\b)");
    for (int i = if_end; i < std::min(if_end + 5, n); ++i) {
        if (std::regex_search(lines[i], else_re)) {
            for (int j = i; j < std::min(i + 4, n); ++j) {
                if (std::regex_search(lines[j], begin_re)) {
                    else_begin = j;
                    break;
                }
            }
            break;
        }
    }
    if (else_begin < 0)
        throw std::runtime_error("AutoFF: always_ff block has no 'else begin' after the if-block");

    // Find end of else-block
    int else_depth = 1;
    int else_end = -1;
    for (int i = else_begin + 1; i < n; ++i) {
        depth_tokens(lines[i], else_depth);
        if (else_depth <= 0) {
            else_end = i;
            break;
        }
    }
    if (else_end < 0)
        throw std::runtime_error("AutoFF: else-block 'end' not found");

    // Extract else indent
    std::string else_indent = "    ";
    if (std::regex_search(lines[else_begin], im, indent_re))
        else_indent = im[1].str() + "    ";

    FfBlock block;
    block.if_begin_line = if_begin;
    block.if_insert_line = if_end;
    block.if_indent = if_indent;
    block.else_begin_line = else_begin;
    block.else_insert_line = else_end;
    block.else_indent = else_indent;
    return block;
}

// Returns {block, found}: found=false if no always_ff, exception if malformed
static std::pair<FfBlock, bool> find_always_ff_if_else(const std::string& text) {
    auto lines = split_lines(text);
    std::regex always_ff_re(R"(\balways_ff\b)");

    std::vector<int> ff_starts;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (std::regex_search(lines[i], always_ff_re))
            ff_starts.push_back(i);
    }
    if (ff_starts.empty())
        return {{}, false};

    std::string last_err;
    for (int ff_start : ff_starts) {
        try {
            return {parse_ff_block(lines, ff_start), true};
        } catch (const std::exception& e) {
            last_err = e.what();
        }
    }
    throw std::runtime_error(last_err.empty() ? "AutoFF: no valid always_ff if/else block found" : last_err);
}

// ── Check assigned in range ───────────────────────────────────────────────────

static bool check_assigned_in_range(
    const std::vector<std::string>& lines,
    const std::string& signal, int begin_line, int end_line)
{
    std::string esc_signal;
    for (char c : signal) {
        if (std::string(".*+?^${}()|[]\\").find(c) != std::string::npos)
            esc_signal += '\\';
        esc_signal += c;
    }
    std::regex lhs_re("^\\s*" + esc_signal + "\\s*<=");
    for (int i = begin_line + 1; i < end_line && i < (int)lines.size(); ++i) {
        if (std::regex_search(lines[i], lhs_re))
            return true;
    }
    return false;
}

// ── Find all (src, dst) pairs in AST ─────────────────────────────────────────

struct PairCollector : public SyntaxVisitor<PairCollector> {
    const SourceManager& sm;
    const std::regex& reg_re;
    std::vector<std::pair<std::string, std::string>> pairs;

    PairCollector(const SourceManager& s, const std::regex& r) : sm(s), reg_re(r) {}

    void collect(const SyntaxNode& node) {
        std::vector<std::string> names;
        for (uint32_t i = 0; i < node.getChildCount(); ++i) {
            const SyntaxNode* child = node.childNode(i);
            if (!child) continue;
            if (const auto* d = child->as_if<DeclaratorSyntax>()) {
                std::string name = std::string(d->name.valueText());
                if (!name.empty()) names.push_back(name);
            }
        }
        if (names.size() != 2) return;
        std::vector<std::string> regs, srcs;
        for (const auto& n : names) {
            if (std::regex_search(n, reg_re)) regs.push_back(n);
            else srcs.push_back(n);
        }
        if (regs.size() == 1 && srcs.size() == 1)
            pairs.push_back({srcs[0], regs[0]});
    }

    void handle(const DataDeclarationSyntax& node) {
        collect(node);
        visitDefault(node);
    }
    void handle(const NetDeclarationSyntax& node) {
        collect(node);
        visitDefault(node);
    }
};

// ── Build ff edits ────────────────────────────────────────────────────────────

static std::pair<std::vector<AutoffEdit>, int> build_ff_edits(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    const FfBlock& ff,
    const std::vector<std::string>& lines)
{
    std::string reset_text, capture_text;
    int pending_count = 0;

    for (const auto& [src, dst] : pairs) {
        bool in_if = check_assigned_in_range(lines, dst, ff.if_begin_line, ff.if_insert_line);
        bool in_else = check_assigned_in_range(lines, dst, ff.else_begin_line, ff.else_insert_line);
        if (in_if && in_else) continue;
        ++pending_count;
        if (!in_if)
            reset_text += ff.if_indent + dst + " <= '0;\n";
        if (!in_else)
            capture_text += ff.else_indent + dst + " <= " + src + ";\n";
    }

    std::vector<AutoffEdit> edits;
    if (!capture_text.empty())
        edits.push_back({ff.else_insert_line, 0, capture_text});
    if (!reset_text.empty())
        edits.push_back({ff.if_insert_line, 0, reset_text});
    // Sort in reverse line order
    std::sort(edits.begin(), edits.end(), [](const AutoffEdit& a, const AutoffEdit& b) {
        return a.line > b.line;
    });
    return {edits, pending_count};
}

// ── Public API ────────────────────────────────────────────────────────────────

AutoffResult autoff(const DocumentState& state, int cursor_line, const std::string& register_pattern) {
    AutoffResult result;
    if (state.text.empty()) {
        result.has_error = true;
        result.error = "AutoFF: document is empty";
        return result;
    }
    std::string pat = register_pattern.empty() ? DEFAULT_REGISTER_PATTERN : register_pattern;
    std::regex reg_re;
    try {
        reg_re = std::regex(pat);
    } catch (...) {
        result.has_error = true;
        result.error = "AutoFF: invalid register_pattern '" + pat + "'";
        return result;
    }

    std::vector<std::string> names;
    try {
        names = parse_declaration_signals(state, cursor_line);
    } catch (const std::exception& e) {
        result.has_error = true;
        result.error = e.what();
        return result;
    }

    auto [src, dst] = pair_signals(names, reg_re);

    FfBlock ff;
    bool found;
    try {
        auto [block, f] = find_always_ff_if_else(state.text);
        ff = block; found = f;
    } catch (const std::exception& e) {
        result.has_error = true;
        result.error = e.what();
        return result;
    }
    if (!found) {
        result.has_error = true;
        result.error = "AutoFF: no always_ff block found in file";
        return result;
    }

    auto lines = split_lines(state.text);
    auto [edits, _pending] = build_ff_edits({{src, dst}}, ff, lines);

    if (edits.empty()) {
        result.has_error = false;
        result.warn = true;
        result.error = "AutoFF: '" + dst + "' is already assigned in both blocks — skipped";
        return result;
    }
    result.edits = std::move(edits);
    return result;
}

AutoffResult autoff_all(const DocumentState& state, const std::string& register_pattern) {
    AutoffResult result;
    if (state.text.empty()) {
        result.has_error = true;
        result.error = "AutoFF: document is empty";
        return result;
    }
    std::string pat = register_pattern.empty() ? DEFAULT_REGISTER_PATTERN : register_pattern;
    std::regex reg_re;
    try {
        reg_re = std::regex(pat);
    } catch (...) {
        result.has_error = true;
        result.error = "AutoFF: invalid register_pattern '" + pat + "'";
        return result;
    }

    if (!state.tree) {
        result.has_error = true;
        result.error = "AutoFF: document could not be parsed";
        return result;
    }

    PairCollector pc(state.tree->sourceManager(), reg_re);
    state.tree->root().visit(pc);
    if (pc.pairs.empty()) {
        result.has_error = false;
        result.warn = true;
        result.error = "AutoFF: no two-signal declarations matching register pattern found";
        return result;
    }

    FfBlock ff;
    bool found;
    try {
        auto [block, f] = find_always_ff_if_else(state.text);
        ff = block; found = f;
    } catch (const std::exception& e) {
        result.has_error = true;
        result.error = e.what();
        return result;
    }
    if (!found) {
        result.has_error = true;
        result.error = "AutoFF: no always_ff block found in file";
        return result;
    }

    auto lines = split_lines(state.text);
    auto [edits, pending_count] = build_ff_edits(pc.pairs, ff, lines);

    if (edits.empty()) {
        result.has_error = false;
        result.warn = true;
        result.error = "AutoFF: all matching signals already assigned in both blocks";
        return result;
    }
    result.edits = std::move(edits);
    result.pending_count = pending_count;
    return result;
}

AutoffResult preview_autoff(const DocumentState& state, int cursor_line, const std::string& register_pattern) {
    AutoffResult result;
    if (state.text.empty()) {
        result.has_error = true;
        result.error = "AutoFF: document is empty";
        return result;
    }
    std::string pat = register_pattern.empty() ? DEFAULT_REGISTER_PATTERN : register_pattern;
    std::regex reg_re;
    try {
        reg_re = std::regex(pat);
    } catch (...) {
        result.has_error = true;
        result.error = "AutoFF: invalid register_pattern '" + pat + "'";
        return result;
    }

    std::vector<std::string> names;
    try {
        names = parse_declaration_signals(state, cursor_line);
    } catch (const std::exception& e) {
        result.has_error = true;
        result.error = e.what();
        return result;
    }

    auto [src, dst] = pair_signals(names, reg_re);

    FfBlock ff;
    bool found;
    try {
        auto [block, f] = find_always_ff_if_else(state.text);
        ff = block; found = f;
    } catch (const std::exception& e) {
        result.has_error = true;
        result.error = e.what();
        return result;
    }
    if (!found) {
        result.has_error = true;
        result.error = "AutoFF: no always_ff block found in file";
        return result;
    }

    auto lines = split_lines(state.text);
    bool in_if = check_assigned_in_range(lines, dst, ff.if_begin_line, ff.if_insert_line);
    bool in_else = check_assigned_in_range(lines, dst, ff.else_begin_line, ff.else_insert_line);

    if (in_if && in_else) {
        result.warn = true;
        result.error = "AutoFF: '" + dst + "' already assigned in both blocks";
        return result;
    }

    AutoffPair pair;
    pair.src = src;
    pair.dst = dst;
    pair.missing_if = !in_if;
    pair.missing_else = !in_else;
    result.pairs.push_back(pair);
    return result;
}

AutoffResult preview_autoff_all(const DocumentState& state, const std::string& register_pattern) {
    AutoffResult result;
    if (state.text.empty()) {
        result.has_error = true;
        result.error = "AutoFF: document is empty";
        return result;
    }
    std::string pat = register_pattern.empty() ? DEFAULT_REGISTER_PATTERN : register_pattern;
    std::regex reg_re;
    try {
        reg_re = std::regex(pat);
    } catch (...) {
        result.has_error = true;
        result.error = "AutoFF: invalid register_pattern '" + pat + "'";
        return result;
    }

    if (!state.tree) {
        result.has_error = true;
        result.error = "AutoFF: document could not be parsed";
        return result;
    }

    PairCollector pc(state.tree->sourceManager(), reg_re);
    state.tree->root().visit(pc);
    if (pc.pairs.empty()) {
        result.warn = true;
        result.error = "AutoFF: no matching two-signal declarations found";
        return result;
    }

    FfBlock ff;
    bool found;
    try {
        auto [block, f] = find_always_ff_if_else(state.text);
        ff = block; found = f;
    } catch (const std::exception& e) {
        result.has_error = true;
        result.error = e.what();
        return result;
    }
    if (!found) {
        result.has_error = true;
        result.error = "AutoFF: no always_ff block found in file";
        return result;
    }

    auto lines = split_lines(state.text);
    for (const auto& [src, dst] : pc.pairs) {
        bool in_if = check_assigned_in_range(lines, dst, ff.if_begin_line, ff.if_insert_line);
        bool in_else = check_assigned_in_range(lines, dst, ff.else_begin_line, ff.else_insert_line);
        if (!in_if || !in_else) {
            AutoffPair pair;
            pair.src = src;
            pair.dst = dst;
            pair.missing_if = !in_if;
            pair.missing_else = !in_else;
            result.pairs.push_back(pair);
        }
    }

    if (result.pairs.empty()) {
        result.warn = true;
        result.error = "AutoFF: all signals already assigned in both blocks";
    }
    return result;
}
