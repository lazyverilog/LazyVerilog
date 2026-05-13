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

    bool on_target_line(const SyntaxNode& node) {
        auto tok = node.getFirstToken();
        if (!tok.valid() || !tok.location().valid()) return false;
        size_t ln = sm.getLineNumber(tok.location());
        return (int)(ln > 0 ? ln - 1 : 0) == target_line;
    }

    void handle(const DataDeclarationSyntax& node) {
        if (on_target_line(node)) {
            matched = true;
            for (uint32_t i = 0; i < node.declarators.size(); ++i) {
                if (node.declarators[i]) {
                    std::string n = std::string(node.declarators[i]->name.valueText());
                    if (!n.empty()) names.push_back(n);
                }
            }
        }
        visitDefault(node);
    }
    void handle(const NetDeclarationSyntax& node) {
        if (on_target_line(node)) {
            matched = true;
            for (uint32_t i = 0; i < node.declarators.size(); ++i) {
                if (node.declarators[i]) {
                    std::string n = std::string(node.declarators[i]->name.valueText());
                    if (!n.empty()) names.push_back(n);
                }
            }
        }
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

// ── Parse always_ff if/else structure ────────────────────────────────────────

struct FfBlock {
    int if_begin_line{-1};
    int if_insert_line{-1};
    std::string if_indent;
    int else_begin_line{-1};
    int else_insert_line{-1};
    std::string else_indent;
    const BlockStatementSyntax* if_block{nullptr};
    const BlockStatementSyntax* else_block{nullptr};
};

static int token_line(const SourceManager& sm, const slang::parsing::Token& tok) {
    if (!tok || !tok.location().valid()) return 0;
    auto line = sm.getLineNumber(tok.location());
    return line > 0 ? (int)line - 1 : 0;
}

static int token_col(const SourceManager& sm, const slang::parsing::Token& tok) {
    if (!tok || !tok.location().valid()) return 0;
    auto col = sm.getColumnNumber(tok.location());
    return col > 0 ? (int)col - 1 : 0;
}

struct FfFinder : public SyntaxVisitor<FfFinder> {
    const SourceManager& sm;
    std::optional<FfBlock> block;
    std::string error;

    explicit FfFinder(const SourceManager& sm) : sm(sm) {}

    void handle(const ProceduralBlockSyntax& node) {
        if (block || node.kind != SyntaxKind::AlwaysFFBlock) {
            visitDefault(node);
            return;
        }

        const auto* outer = node.statement->as_if<BlockStatementSyntax>();
        if (!outer) {
            error = "AutoFF: always_ff block missing 'begin'";
            visitDefault(node);
            return;
        }

        const ConditionalStatementSyntax* conditional = nullptr;
        for (const auto* item : outer->items) {
            if (!item) continue;
            if (const auto* stmt = item->as_if<ConditionalStatementSyntax>()) {
                conditional = stmt;
                break;
            }
        }
        if (!conditional) {
            error = "AutoFF: no 'if' statement found inside always_ff block";
            visitDefault(node);
            return;
        }

        const auto* if_block = conditional->statement->as_if<BlockStatementSyntax>();
        if (!if_block) {
            error = "AutoFF: if-block inside always_ff is missing 'begin'";
            visitDefault(node);
            return;
        }
        if (!conditional->elseClause) {
            error = "AutoFF: always_ff block has no 'else begin' after the if-block";
            visitDefault(node);
            return;
        }
        const auto* else_block = conditional->elseClause->clause->as_if<BlockStatementSyntax>();
        if (!else_block) {
            error = "AutoFF: always_ff block has no 'else begin' after the if-block";
            visitDefault(node);
            return;
        }

        FfBlock ff;
        ff.if_begin_line = token_line(sm, if_block->begin);
        ff.if_insert_line = token_line(sm, if_block->end);
        ff.if_indent = std::string((size_t)token_col(sm, if_block->begin) + 4, ' ');
        ff.else_begin_line = token_line(sm, else_block->begin);
        ff.else_insert_line = token_line(sm, else_block->end);
        ff.else_indent = std::string((size_t)token_col(sm, else_block->begin) + 4, ' ');
        ff.if_block = if_block;
        ff.else_block = else_block;
        block = ff;
        visitDefault(node);
    }
};

static std::pair<FfBlock, bool> find_always_ff_if_else(const DocumentState& state) {
    if (!state.tree)
        return {{}, false};
    FfFinder finder(state.tree->sourceManager());
    state.tree->root().visit(finder);
    if (finder.block)
        return {*finder.block, true};
    if (!finder.error.empty())
        throw std::runtime_error(finder.error);
    return {{}, false};
}

struct AssignmentFinder : public SyntaxVisitor<AssignmentFinder> {
    std::string signal;
    bool found{false};

    explicit AssignmentFinder(std::string signal) : signal(std::move(signal)) {}

    void handle(const BinaryExpressionSyntax& node) {
        if (found)
            return;
        if (node.kind == SyntaxKind::NonblockingAssignmentExpression ||
            node.kind == SyntaxKind::AssignmentExpression) {
            if (const auto* id = node.left->as_if<IdentifierNameSyntax>())
                found = std::string(id->identifier.valueText()) == signal;
        }
        visitDefault(node);
    }
};

static bool check_assigned_in_block(const BlockStatementSyntax* block, const std::string& signal) {
    if (!block)
        return false;
    AssignmentFinder finder(signal);
    block->visit(finder);
    return finder.found;
}

// ── Find all (src, dst) pairs in AST ─────────────────────────────────────────

struct PairCollector : public SyntaxVisitor<PairCollector> {
    const SourceManager& sm;
    const std::regex& reg_re;
    std::vector<std::pair<std::string, std::string>> pairs;

    PairCollector(const SourceManager& s, const std::regex& r) : sm(s), reg_re(r) {}

    template <typename T>
    void collect(const T& node) {
        std::vector<std::string> names;
        for (uint32_t i = 0; i < node.declarators.size(); ++i) {
            if (node.declarators[i]) {
                std::string n = std::string(node.declarators[i]->name.valueText());
                if (!n.empty()) names.push_back(n);
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
    const FfBlock& ff)
{
    std::string reset_text, capture_text;
    int pending_count = 0;

    for (const auto& [src, dst] : pairs) {
        bool in_if = check_assigned_in_block(ff.if_block, dst);
        bool in_else = check_assigned_in_block(ff.else_block, dst);
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
        auto [block, f] = find_always_ff_if_else(state);
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

    auto [edits, _pending] = build_ff_edits({{src, dst}}, ff);

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
        auto [block, f] = find_always_ff_if_else(state);
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

    auto [edits, pending_count] = build_ff_edits(pc.pairs, ff);

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
        auto [block, f] = find_always_ff_if_else(state);
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

    bool in_if = check_assigned_in_block(ff.if_block, dst);
    bool in_else = check_assigned_in_block(ff.else_block, dst);

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
        auto [block, f] = find_always_ff_if_else(state);
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

    for (const auto& [src, dst] : pc.pairs) {
        bool in_if = check_assigned_in_block(ff.if_block, dst);
        bool in_else = check_assigned_in_block(ff.else_block, dst);
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
