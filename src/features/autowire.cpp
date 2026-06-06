#include "autowire.hpp"
#include "../analyzer.hpp"
#include "../string_utils.hpp"
#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

using namespace slang;
using namespace slang::syntax;

// ── Helpers ───────────────────────────────────────────────────────────────────

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

static bool in_range(int line, LineRange range) {
    return line >= range.start && line <= range.end;
}

static int token_line(const SourceManager& sm, const slang::parsing::Token& tok) {
    if (!tok || !tok.location().valid())
        return 0;
    auto line = sm.getLineNumber(tok.location());
    return line > 0 ? (int)line - 1 : 0;
}

struct TargetModuleRangeFinder : public SyntaxVisitor<TargetModuleRangeFinder> {
    const SourceManager& sm;
    int target_line;
    std::string name;
    LineRange range;
    bool found{false};

    TargetModuleRangeFinder(const SourceManager& sm, int target_line)
        : sm(sm), target_line(target_line) {}

    void handle(const ModuleDeclarationSyntax& node) {
        if (found)
            return;
        LineRange candidate{token_line(sm, node.getFirstToken()), token_line(sm, node.endmodule)};
        if (target_line >= 0 && !in_range(target_line, candidate))
            return;
        name = std::string(node.header->name.valueText());
        range = candidate;
        found = true;
    }
};

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

static std::optional<PortEntry> project_port(const SyntaxIndex& syntax_index,
                                             const std::string& module_name,
                                             const std::string& port_name) {
    auto module_it = syntax_index.module_by_name.find(module_name);
    if (module_it == syntax_index.module_by_name.end() ||
        module_it->second >= syntax_index.modules.size())
        return std::nullopt;
    const auto& module = syntax_index.modules[module_it->second];
    auto port_it = module.port_by_name.find(port_name);
    if (port_it == module.port_by_name.end() || port_it->second >= module.ports.size())
        return std::nullopt;
    return module.ports[port_it->second];
}

static std::optional<PortEntry> project_port(const ProjectIndexSnapshot& snapshot,
                                             const std::string& module_name,
                                             const std::string& port_name) {
    const auto module_it = snapshot.module_by_name.find(module_name);
    if (module_it == snapshot.module_by_name.end() || !module_it->second.shard)
        return std::nullopt;
    const auto& shard = *module_it->second.shard;
    if (module_it->second.module_index >= shard.modules.size())
        return std::nullopt;
    const auto& module = shard.modules[module_it->second.module_index];
    const auto port_it = module.port_by_name.find(port_name);
    if (port_it == module.port_by_name.end() || port_it->second >= module.ports.size())
        return std::nullopt;
    return module.ports[port_it->second];
}

static std::optional<PortEntry> project_port(std::span<const OpenIndexShard> opened_shards,
                                             const std::string& module_name,
                                             const std::string& port_name) {
    for (const auto& shard : opened_shards) {
        if (!shard.index)
            continue;
        if (auto port = project_port(*shard.index, module_name, port_name))
            return port;
    }
    return std::nullopt;
}

static std::unordered_map<std::string, PortEntry>
current_ast_ports_for_module(const DocumentState& state, const std::string& module_name) {
    std::unordered_map<std::string, PortEntry> ports;
    if (!state.tree)
        return ports;

    struct Visitor : public SyntaxVisitor<Visitor> {
        std::string module_name;
        std::unordered_map<std::string, PortEntry>& ports;
        bool in_target{false};
        bool done{false};

        Visitor(std::string module_name, std::unordered_map<std::string, PortEntry>& ports)
            : module_name(std::move(module_name)), ports(ports) {}

        void add(std::string name, std::string direction, std::string type) {
            if (name.empty() || ports.contains(name))
                return;
            ports.emplace(name, PortEntry{.name = std::move(name),
                                          .direction = std::move(direction),
                                          .type = trim_copy(std::move(type))});
        }

        void handle(const ModuleDeclarationSyntax& node) {
            if (done)
                return;
            const bool was = in_target;
            in_target = node.header && node.header->name.valueText() == module_name;
            if (in_target)
                visitDefault(node);
            in_target = was;
            if (!ports.empty())
                done = true;
        }

        void handle(const ImplicitAnsiPortSyntax& node) {
            if (in_target && node.declarator) {
                std::string direction;
                std::string type;
                if (node.header) {
                    if (const auto* variable = node.header->as_if<VariablePortHeaderSyntax>()) {
                        direction = std::string(variable->direction.valueText());
                        type = variable->dataType ? variable->dataType->toString() : "";
                    } else if (const auto* net = node.header->as_if<NetPortHeaderSyntax>()) {
                        direction = std::string(net->direction.valueText());
                        type = net->dataType ? net->dataType->toString() : "";
                    }
                }
                add(std::string(node.declarator->name.valueText()), direction, type);
            }
            if (!done)
                visitDefault(node);
        }

        void handle(const ExplicitAnsiPortSyntax& node) {
            if (in_target)
                add(std::string(node.name.valueText()), std::string(node.direction.valueText()), "");
            if (!done)
                visitDefault(node);
        }

        void handle(const PortDeclarationSyntax& node) {
            if (in_target) {
                std::string direction;
                std::string type;
                if (const auto* variable = node.header->as_if<VariablePortHeaderSyntax>()) {
                    direction = std::string(variable->direction.valueText());
                    type = variable->dataType ? variable->dataType->toString() : "";
                } else if (const auto* net = node.header->as_if<NetPortHeaderSyntax>()) {
                    direction = std::string(net->direction.valueText());
                    type = net->dataType ? net->dataType->toString() : "";
                }
                for (const auto* declarator : node.declarators) {
                    if (declarator)
                        add(std::string(declarator->name.valueText()), direction, type);
                }
            }
            if (!done)
                visitDefault(node);
        }
    };

    Visitor visitor(module_name, ports);
    state.tree->root().visit(visitor);
    return ports;
}

static std::vector<NamedPortConn>
current_ast_instance_connections(const DocumentState& state, LineRange parent_range,
                                 const std::string& parent_module,
                                 std::vector<std::string>& module_names) {
    std::vector<NamedPortConn> connections;
    if (!state.tree)
        return connections;

    struct Visitor : public SyntaxVisitor<Visitor> {
        const SourceManager& sm;
        LineRange parent_range;
        const std::string& parent_module;
        std::vector<NamedPortConn>& connections;
        std::vector<std::string>& module_names;

        Visitor(const SourceManager& sm, LineRange parent_range, const std::string& parent_module,
                std::vector<NamedPortConn>& connections, std::vector<std::string>& module_names)
            : sm(sm), parent_range(parent_range), parent_module(parent_module),
              connections(connections), module_names(module_names) {}

        void handle(const HierarchyInstantiationSyntax& node) {
            if (!in_range(token_line(sm, node.getFirstToken()), parent_range))
                return;
            const std::string module_name(node.type.valueText());
            for (const auto* inst : node.instances) {
                if (!inst)
                    continue;
                for (const auto* conn : inst->connections) {
                    const auto* named = conn ? conn->as_if<NamedPortConnectionSyntax>() : nullptr;
                    if (!named)
                        continue;
                    std::string signal = trim_copy(named->expr ? named->expr->toString() : "");
                    connections.push_back(NamedPortConn{
                        .port_name = std::string(named->name.valueText()),
                        .signal_name = std::move(signal),
                    });
                    module_names.push_back(module_name);
                }
            }
            visitDefault(node);
        }
    };

    Visitor visitor(state.tree->sourceManager(), parent_range, parent_module, connections,
                    module_names);
    state.tree->root().visit(visitor);
    return connections;
}

static std::vector<InstSignal> collect_inst_signals(const DocumentState& state,
                                                    const SyntaxIndex* opened_index,
                                                    std::span<const OpenIndexShard> opened_shards,
                                                    const ProjectIndexSnapshot* project_index,
                                                    const std::string& parent_module,
                                                    LineRange parent_range) {
    std::vector<InstSignal> results;
    std::set<std::string> seen;
    int order = 0;

    std::vector<std::string> module_names;
    const auto conns = current_ast_instance_connections(state, parent_range, parent_module,
                                                        module_names);
    for (size_t i = 0; i < conns.size(); ++i) {
        const auto& conn = conns[i];
        const std::string& module_name = module_names[i];
            std::string port_name = conn.port_name;
            std::string signal = conn.signal_name;
            if (!is_simple_id(signal))
                continue;
            if (seen.count(signal))
                continue;

            std::string direction;
            std::string type_kw = "logic";
            std::string dimension;

            auto current_ports = current_ast_ports_for_module(state, module_name);
            std::optional<PortEntry> port;
            if (auto it = current_ports.find(port_name); it != current_ports.end())
                port = it->second;
            else if (opened_index)
                port = project_port(*opened_index, module_name, port_name);
            if (!port && !opened_shards.empty())
                port = project_port(opened_shards, module_name, port_name);
            if (!port && project_index)
                port = project_port(*project_index, module_name, port_name);

            if (port) {
                direction = port->direction;
                auto lb = port->type.find('[');
                auto rb = port->type.rfind(']');
                if (lb != std::string::npos && rb != std::string::npos && lb < rb)
                    dimension = port->type.substr(lb, rb - lb + 1);
            }

            // Only include output/inout ports
            if (direction == "input")
                continue;
            if (!direction.empty() && direction != "output" && direction != "inout" &&
                direction != "out" && direction != "Out")
                continue;

            seen.insert(signal);
            results.push_back({signal, module_name, type_kw, dimension, order++});
    }
    return results;
}

// ── Find module body range ────────────────────────────────────────────────────

struct InsertionLineFinder : public SyntaxVisitor<InsertionLineFinder> {
    const SourceManager& sm;
    LineRange range;
    int body_start{0};
    int end_module{0};
    int last_decl = -1;
    int first_inst = -1;
    int first_proc = -1;
    bool found_module{false};

    InsertionLineFinder(const SourceManager& sm, LineRange range) : sm(sm), range(range) {}

    void handle(const ModuleDeclarationSyntax& node) {
        if (found_module)
            return;
        LineRange candidate{token_line(sm, node.getFirstToken()), token_line(sm, node.endmodule)};
        if (candidate.start != range.start || candidate.end != range.end)
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

static int find_insertion_line(const DocumentState& state, LineRange range) {
    if (!state.tree)
        return 0;
    InsertionLineFinder finder(state.tree->sourceManager(), range);
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

static std::string format_declarations(std::vector<SignalDecl> signals, const AutowireOptions& options) {
    if (signals.empty())
        return {};

    if (options.sort_by_name) {
        std::sort(signals.begin(), signals.end(), [](const SignalDecl& a, const SignalDecl& b) {
            if (a.module_name != b.module_name)
                return a.module_name < b.module_name;
            return a.name < b.name;
        });
    }

    size_t max_dim_len = 0;
    for (const auto& s : signals)
        max_dim_len = std::max(max_dim_len, s.dimension.size());

    std::string out;
    std::string last_module;
    for (size_t i = 0; i < signals.size(); ++i) {
        if (i > 0)
            out += "\n";
        if (options.group_by_instance && signals[i].module_name != last_module) {
            if (i > 0)
                out += "\n";
            out += "// " + signals[i].module_name + "\n";
            last_module = signals[i].module_name;
        }
        out += format_one_decl(signals[i], max_dim_len);
    }
    return out;
}

// ── Main entry points ─────────────────────────────────────────────────────────

static std::vector<SignalDecl> compute_new_signals(const DocumentState& state,
                                                   const SyntaxIndex* opened_index,
                                                   std::span<const OpenIndexShard> opened_shards,
                                                   const ProjectIndexSnapshot* project_index,
                                                   int target_line) {
    if (!state.tree)
        return {};

    TargetModuleRangeFinder module_finder(state.tree->sourceManager(), target_line);
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
    auto inst_sigs = collect_inst_signals(state, opened_index, opened_shards, project_index,
                                          module_finder.name, module_finder.range);

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

static std::string apply_signal_declarations(const DocumentState& state,
                                             std::vector<SignalDecl> new_sigs,
                                             const AutowireOptions& options,
                                             int target_line) {
    if (new_sigs.empty())
        return state.text;

    auto lines = split_lines_owned(state.text);
    std::string decl_text = format_declarations(new_sigs, options);
    TargetModuleRangeFinder module_finder(state.tree->sourceManager(), target_line);
    state.tree->root().visit(module_finder);
    int insert_line = module_finder.found ? find_insertion_line(state, module_finder.range) : 0;

    std::vector<std::string> out_lines;
    out_lines.insert(out_lines.end(), lines.begin(), lines.begin() + insert_line);
    for (const auto& dl : split_lines_owned(decl_text))
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

static std::vector<std::string> preview_signal_declarations(std::vector<SignalDecl> new_sigs,
                                                            const AutowireOptions& options) {
    std::vector<std::string> out;
    if (!new_sigs.empty()) {
        out.push_back("Will add:");
        if (options.sort_by_name) {
            std::sort(new_sigs.begin(), new_sigs.end(), [](const SignalDecl& a, const SignalDecl& b) {
                return a.name < b.name;
            });
        }
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

std::string autowire_apply(const DocumentState& state, const SyntaxIndex* opened_index,
                           const ProjectIndexSnapshot* project_index,
                           const AutowireOptions& options, int target_line) {
    auto new_sigs = compute_new_signals(state, opened_index, {}, project_index, target_line);
    return apply_signal_declarations(state, std::move(new_sigs), options, target_line);
}

std::string autowire_apply(const DocumentState& state,
                           std::span<const OpenIndexShard> opened_shards,
                           const ProjectIndexSnapshot* project_index,
                           const AutowireOptions& options, int target_line) {
    auto new_sigs = compute_new_signals(state, nullptr, opened_shards, project_index, target_line);
    return apply_signal_declarations(state, std::move(new_sigs), options, target_line);
}

std::vector<std::string> autowire_preview(const DocumentState& state,
                                          const SyntaxIndex* opened_index,
                                          const ProjectIndexSnapshot* project_index,
                                          const AutowireOptions& options, int target_line) {
    auto new_sigs = compute_new_signals(state, opened_index, {}, project_index, target_line);
    return preview_signal_declarations(std::move(new_sigs), options);
}

std::vector<std::string> autowire_preview(const DocumentState& state,
                                          std::span<const OpenIndexShard> opened_shards,
                                          const ProjectIndexSnapshot* project_index,
                                          const AutowireOptions& options, int target_line) {
    auto new_sigs = compute_new_signals(state, nullptr, opened_shards, project_index, target_line);
    return preview_signal_declarations(std::move(new_sigs), options);
}

std::string autowire_apply(const DocumentState& state, const SyntaxIndex& opened_index,
                           const AutowireOptions& options, int target_line) {
    return autowire_apply(state, &opened_index, nullptr, options, target_line);
}

std::vector<std::string> autowire_preview(const DocumentState& state,
                                          const SyntaxIndex& opened_index,
                                          const AutowireOptions& options, int target_line) {
    return autowire_preview(state, &opened_index, nullptr, options, target_line);
}
