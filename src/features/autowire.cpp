#include "autowire.hpp"
#include <algorithm>
#include <set>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

using namespace slang;
using namespace slang::syntax;

// ── Helpers ───────────────────────────────────────────────────────────────────

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

static bool is_simple_id(const std::string& s) {
    if (s.empty())
        return false;
    if (!std::isalpha((unsigned char)s[0]) && s[0] != '_')
        return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_')
            return false;
    return true;
}

// ── AST visitor: collect declared signal names ────────────────────────────────

struct LineRange {
    int start{0};
    int end{0};
};

static int token_line(const SourceManager& sm, const slang::parsing::Token& tok) {
    if (!tok || !tok.location().valid())
        return 0;
    auto line = sm.getLineNumber(tok.location());
    return line > 0 ? (int)line - 1 : 0;
}

struct FirstModuleRangeFinder : public SyntaxVisitor<FirstModuleRangeFinder> {
    const SourceManager& sm;
    std::string name;
    LineRange range;
    bool found{false};

    explicit FirstModuleRangeFinder(const SourceManager& sm) : sm(sm) {}

    void handle(const ModuleDeclarationSyntax& node) {
        if (!found) {
            name = std::string(node.header->name.valueText());
            range.start = token_line(sm, node.getFirstToken());
            range.end = token_line(sm, node.endmodule);
            found = true;
        }
    }
};

static bool in_range(int line, LineRange range) {
    return line >= range.start && line <= range.end;
}

// ── AST visitor: collect declared signal names ────────────────────────────────

struct DeclCollector : public SyntaxVisitor<DeclCollector> {
    const SourceManager& sm;
    LineRange range;
    std::set<std::string> declared;

    DeclCollector(const SourceManager& sm, LineRange range) : sm(sm), range(range) {}

    void handle(const DataDeclarationSyntax& node) {
        if (!in_range(token_line(sm, node.getFirstToken()), range))
            return;
        for (uint32_t i = 0; i < node.declarators.size(); ++i) {
            const auto* d = node.declarators[i];
            if (d)
                declared.insert(std::string(d->name.valueText()));
        }
        visitDefault(node);
    }
    void handle(const NetDeclarationSyntax& node) {
        if (!in_range(token_line(sm, node.getFirstToken()), range))
            return;
        for (uint32_t i = 0; i < node.declarators.size(); ++i) {
            const auto* d = node.declarators[i];
            if (d)
                declared.insert(std::string(d->name.valueText()));
        }
        visitDefault(node);
    }
    void handle(const PortDeclarationSyntax& node) {
        if (!in_range(token_line(sm, node.getFirstToken()), range))
            return;
        for (uint32_t i = 0; i < node.declarators.size(); ++i) {
            const auto* d = node.declarators[i];
            if (d)
                declared.insert(std::string(d->name.valueText()));
        }
        visitDefault(node);
    }
    void handle(const ParameterDeclarationSyntax& node) {
        if (!in_range(token_line(sm, node.getFirstToken()), range))
            return;
        for (const auto* declarator : node.declarators) {
            if (declarator)
                declared.insert(std::string(declarator->name.valueText()));
        }
        visitDefault(node);
    }
    void handle(const ImplicitAnsiPortSyntax& node) {
        if (!in_range(token_line(sm, node.getFirstToken()), range))
            return;
        if (node.declarator)
            declared.insert(std::string(node.declarator->name.valueText()));
        visitDefault(node);
    }
};

// ── AST visitor: collect assign LHS signals ───────────────────────────────────

struct AssignLhsCollector : public SyntaxVisitor<AssignLhsCollector> {
    const SourceManager& sm;
    LineRange range;
    std::vector<std::string> signals;

    AssignLhsCollector(const SourceManager& sm, LineRange range) : sm(sm), range(range) {}

    void handle(const ContinuousAssignSyntax& node) {
        if (!in_range(token_line(sm, node.getFirstToken()), range))
            return;
        for (uint32_t i = 0; i < node.assignments.size(); ++i) {
            const auto* expr = node.assignments[i];
            if (!expr)
                continue;
            const auto* assign = expr->as_if<BinaryExpressionSyntax>();
            if (!assign || assign->kind != SyntaxKind::AssignmentExpression)
                continue;
            const auto* id = assign->left->as_if<IdentifierNameSyntax>();
            if (!id)
                continue;
            std::string name = std::string(id->identifier.valueText());
            if (is_simple_id(name))
                signals.push_back(name);
        }
        visitDefault(node);
    }
};

// ── AST visitor: collect always_comb LHS signals ─────────────────────────────

struct CombLhsCollector : public SyntaxVisitor<CombLhsCollector> {
    const SourceManager& sm;
    LineRange range;
    std::vector<std::string> signals;
    bool in_comb{false};

    CombLhsCollector(const SourceManager& sm, LineRange range) : sm(sm), range(range) {}

    void handle(const ProceduralBlockSyntax& node) {
        if (!in_range(token_line(sm, node.getFirstToken()), range))
            return;
        if (node.kind == SyntaxKind::AlwaysCombBlock) {
            bool was = in_comb;
            in_comb = true;
            visitDefault(node);
            in_comb = was;
        } else {
            visitDefault(node);
        }
    }
    void handle(const BinaryExpressionSyntax& node) {
        if (in_comb && node.kind == SyntaxKind::AssignmentExpression) {
            const auto* id = node.left->as_if<IdentifierNameSyntax>();
            if (id) {
                std::string name = std::string(id->identifier.valueText());
                if (is_simple_id(name))
                    signals.push_back(name);
            }
        }
        visitDefault(node);
    }
};

// ── Collect instantiation connections from SyntaxIndex ───────────────────────

struct InstSignal {
    std::string signal;
    std::string module_name;
    std::string type_kw;
    std::string dimension;
    int order;
};

static std::vector<InstSignal> collect_inst_signals(const SyntaxIndex& syntax_index,
                                                 const std::string& parent_module) {
    std::vector<InstSignal> results;
    std::set<std::string> seen;
    int order = 0;

    for (const auto& inst : syntax_index.instances) {
        if (inst.parent_module != parent_module)
            continue;
        // Find module definition for port info
        const ModuleEntry* mod_entry = nullptr;
        auto module_it = syntax_index.module_by_name.find(inst.module_name);
        if (module_it != syntax_index.module_by_name.end() &&
            module_it->second < syntax_index.modules.size())
            mod_entry = &syntax_index.modules[module_it->second];

        for (const auto& conn : inst.connections) {
            std::string port_name = conn.port_name;
            std::string signal = conn.signal_name;
            if (!is_simple_id(signal))
                continue;
            if (seen.count(signal))
                continue;

            // Look up port direction
            std::string direction;
            std::string type_kw = "logic";
            std::string dimension;
            if (mod_entry) {
                auto port_it = mod_entry->port_by_name.find(port_name);
                if (port_it != mod_entry->port_by_name.end() &&
                    port_it->second < mod_entry->ports.size()) {
                    const auto& p = mod_entry->ports[port_it->second];
                    direction = p.direction;
                    auto lb = p.type.find('[');
                    auto rb = p.type.rfind(']');
                    if (lb != std::string::npos && rb != std::string::npos && lb < rb)
                        dimension = p.type.substr(lb, rb - lb + 1);
                }
            }

            // Only include output/inout ports
            if (direction == "input")
                continue;
            if (!direction.empty() && direction != "output" && direction != "inout" &&
                direction != "out" && direction != "Out")
                continue;

            seen.insert(signal);
            results.push_back({signal, inst.module_name, type_kw, dimension, order++});
        }
    }
    return results;
}

// ── Find module body range ────────────────────────────────────────────────────

struct InsertionLineFinder : public SyntaxVisitor<InsertionLineFinder> {
    const SourceManager& sm;
    int body_start{0};
    int end_module{0};
    int last_decl = -1;
    int first_inst = -1;
    int first_proc = -1;
    bool found_module{false};

    explicit InsertionLineFinder(const SourceManager& sm) : sm(sm) {}

    void handle(const ModuleDeclarationSyntax& node) {
        if (found_module)
            return;
        found_module = true;
        if (node.header)
            body_start = token_line(sm, node.header->semi) + 1;
        end_module = token_line(sm, node.endmodule);
        visitDefault(node);
    }
    void handle(const DataDeclarationSyntax& node) {
        last_decl = std::max(last_decl, token_line(sm, node.getFirstToken()));
        visitDefault(node);
    }
    void handle(const NetDeclarationSyntax& node) {
        last_decl = std::max(last_decl, token_line(sm, node.getFirstToken()));
        visitDefault(node);
    }
    void handle(const HierarchyInstantiationSyntax& node) {
        int line = token_line(sm, node.getFirstToken());
        if (first_inst < 0 || line < first_inst)
            first_inst = line;
        visitDefault(node);
    }
    void handle(const ProceduralBlockSyntax& node) {
        int line = token_line(sm, node.keyword);
        if (first_proc < 0 || line < first_proc)
            first_proc = line;
        visitDefault(node);
    }
};

static int find_insertion_line(const DocumentState& state) {
    if (!state.tree)
        return 0;
    InsertionLineFinder finder(state.tree->sourceManager());
    state.tree->root().visit(finder);
    if (finder.last_decl >= 0)
        return finder.last_decl + 1;
    if (finder.first_inst >= 0)
        return finder.first_inst;
    if (finder.first_proc >= 0)
        return finder.first_proc;
    return finder.body_start;
}

// ── Format declarations ───────────────────────────────────────────────────────

struct SignalDecl {
    std::string name;
    std::string type_kw;
    std::string dimension;
    std::string module_name;
    int order;
};

static std::string format_one_decl(const SignalDecl& s, size_t max_dim_len) {
    if (!s.dimension.empty()) {
        std::string dim_part = s.dimension;
        while (dim_part.size() < max_dim_len)
            dim_part += ' ';
        return s.type_kw + " " + dim_part + " " + s.name + ";";
    } else if (max_dim_len > 0) {
        std::string pad(max_dim_len, ' ');
        return s.type_kw + " " + pad + " " + s.name + ";";
    } else {
        return s.type_kw + " " + s.name + ";";
    }
}

static std::string format_declarations(const std::vector<SignalDecl>& signals) {
    if (signals.empty())
        return {};
    size_t max_dim_len = 0;
    for (const auto& s : signals)
        max_dim_len = std::max(max_dim_len, s.dimension.size());

    std::string out;
    for (size_t i = 0; i < signals.size(); ++i) {
        if (i > 0)
            out += "\n";
        out += format_one_decl(signals[i], max_dim_len);
    }
    return out;
}

// ── Main entry points ─────────────────────────────────────────────────────────

static std::vector<SignalDecl> compute_new_signals(const DocumentState& state,
                                                   const SyntaxIndex& syntax_index) {
    if (!state.tree)
        return {};

    FirstModuleRangeFinder module_finder(state.tree->sourceManager());
    state.tree->root().visit(module_finder);
    if (!module_finder.found)
        return {};

    // Collect declared signals in the module where AutoWire inserts declarations.
    DeclCollector decl_coll(state.tree->sourceManager(), module_finder.range);
    state.tree->root().visit(decl_coll);
    // Collect assign LHS in the same module.
    AssignLhsCollector assign_coll(state.tree->sourceManager(), module_finder.range);
    state.tree->root().visit(assign_coll);

    // Collect always_comb LHS in the same module.
    CombLhsCollector comb_coll(state.tree->sourceManager(), module_finder.range);
    state.tree->root().visit(comb_coll);

    // Collect instantiation signals in the same parent module.
    auto inst_sigs = collect_inst_signals(syntax_index, module_finder.name);

    // Build result: filter out already declared
    std::set<std::string> seen;
    std::vector<SignalDecl> result;
    int order = 0;

    for (const auto& is : inst_sigs) {
        if (seen.count(is.signal) || decl_coll.declared.count(is.signal)) {
            seen.insert(is.signal);
            continue;
        }
        seen.insert(is.signal);
        result.push_back({is.signal, is.type_kw, is.dimension, is.module_name, order++});
    }
    for (const auto& name : assign_coll.signals) {
        if (seen.count(name) || decl_coll.declared.count(name)) {
            seen.insert(name);
            continue;
        }
        seen.insert(name);
        result.push_back({name, "logic", "", "__assign__", order++});
    }
    for (const auto& name : comb_coll.signals) {
        if (seen.count(name) || decl_coll.declared.count(name)) {
            seen.insert(name);
            continue;
        }
        seen.insert(name);
        result.push_back({name, "logic", "", "__always_comb__", order++});
    }

    // Sort by order
    std::sort(result.begin(), result.end(),
              [](const SignalDecl& a, const SignalDecl& b) { return a.order < b.order; });
    return result;
}

std::string autowire_apply(const DocumentState& state, const SyntaxIndex& syntax_index,
                           const AutowireOptions& /*options*/) {
    auto new_sigs = compute_new_signals(state, syntax_index);
    if (new_sigs.empty())
        return state.text;

    auto lines = split_lines(state.text);
    std::string decl_text = format_declarations(new_sigs);
    int insert_line = find_insertion_line(state);

    std::vector<std::string> out_lines;
    out_lines.insert(out_lines.end(), lines.begin(), lines.begin() + insert_line);
    for (const auto& dl : split_lines(decl_text))
        out_lines.push_back(dl);
    out_lines.push_back(""); // blank line after
    out_lines.insert(out_lines.end(), lines.begin() + insert_line, lines.end());

    std::string result;
    for (size_t i = 0; i < out_lines.size(); ++i) {
        if (i > 0)
            result += "\n";
        result += out_lines[i];
    }
    return result;
}

std::vector<std::string> autowire_preview(const DocumentState& state,
                                          const SyntaxIndex& syntax_index,
                                          const AutowireOptions& /*options*/) {
    auto new_sigs = compute_new_signals(state, syntax_index);
    std::vector<std::string> out;
    if (!new_sigs.empty()) {
        out.push_back("Will add:");
        for (const auto& s : new_sigs) {
            std::string line = "  " + s.type_kw;
            if (!s.dimension.empty())
                line += " " + s.dimension;
            line += " " + s.name + ";";
            out.push_back(line);
        }
    }
    return out;
}
