#include "autoinst.hpp"
#include <algorithm>
#include <unordered_set>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

using namespace slang;
using namespace slang::syntax;

// ── Split source into lines ───────────────────────────────────────────────────

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

// ── Collect HierarchyInstantiation candidates ─────────────────────────────────

struct InstCandidate {
    int first_line; // 0-based
    const HierarchyInstantiationSyntax* node;
};

struct InstCollector : public SyntaxVisitor<InstCollector> {
    const SourceManager& sm;
    std::vector<InstCandidate> candidates;

    explicit InstCollector(const SourceManager& s) : sm(s) {}

    void handle(const HierarchyInstantiationSyntax& node) {
        auto tok = node.getFirstToken();
        if (tok.valid() && tok.location().valid()) {
            size_t ln = sm.getLineNumber(tok.location());
            int line_0 = (int)(ln > 0 ? ln - 1 : 0);
            candidates.push_back({line_0, &node});
        }
        visitDefault(node);
    }
};

// ── Find end line of instantiation (scan for ';') ────────────────────────────

static int find_inst_end(const std::vector<std::string>& lines, int start_line) {
    for (int i = start_line; i < (int)lines.size(); ++i) {
        if (lines[i].find(';') != std::string::npos)
            return i;
    }
    return (int)lines.size() - 1;
}

static std::string simple_identifier_from_expr(const PropertyExprSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* prop = expr->as_if<SimplePropertyExprSyntax>()) {
        if (const auto* seq = prop->expr->as_if<SimpleSequenceExprSyntax>()) {
            if (const auto* id = seq->expr->as_if<IdentifierNameSyntax>())
                return std::string(id->identifier.valueText());
        }
    }
    return {};
}

static void push_unique_port(std::vector<std::string>& ports, std::unordered_set<std::string>& seen,
                             std::string name) {
    if (!name.empty() && seen.insert(name).second)
        ports.push_back(std::move(name));
}

static std::vector<std::string> ports_for_module_in_current_ast(const DocumentState& state,
                                                                std::string_view module_type) {
    std::vector<std::string> ports;
    if (!state.tree)
        return ports;

    struct Visitor : public SyntaxVisitor<Visitor> {
        std::string_view module_type;
        std::vector<std::string>& ports;
        std::unordered_set<std::string> seen;
        bool in_target{false};
        bool done{false};

        Visitor(std::string_view module_type, std::vector<std::string>& ports)
            : module_type(module_type), ports(ports) {}

        void handle(const ModuleDeclarationSyntax& node) {
            if (done)
                return;
            const bool was = in_target;
            in_target = node.header && node.header->name.valueText() == module_type;
            if (in_target)
                visitDefault(node);
            in_target = was;
            if (!ports.empty())
                done = true;
        }

        void handle(const ImplicitAnsiPortSyntax& node) {
            if (in_target && node.declarator)
                push_unique_port(ports, seen, std::string(node.declarator->name.valueText()));
            if (!done)
                visitDefault(node);
        }

        void handle(const ExplicitAnsiPortSyntax& node) {
            if (in_target)
                push_unique_port(ports, seen, std::string(node.name.valueText()));
            if (!done)
                visitDefault(node);
        }

        void handle(const PortDeclarationSyntax& node) {
            if (in_target) {
                for (const auto* declarator : node.declarators) {
                    if (declarator)
                        push_unique_port(ports, seen, std::string(declarator->name.valueText()));
                }
            }
            if (!done)
                visitDefault(node);
        }
    };

    Visitor visitor(module_type, ports);
    state.tree->root().visit(visitor);
    return ports;
}

static std::vector<std::string> ports_for_module_in_project_index(const SyntaxIndex& syntax_index,
                                                                  const std::string& module_type) {
    const ModuleEntry* mod_entry = nullptr;
    auto module_it = syntax_index.module_by_name.find(module_type);
    if (module_it != syntax_index.module_by_name.end() &&
        module_it->second < syntax_index.modules.size())
        mod_entry = &syntax_index.modules[module_it->second];
    if (!mod_entry)
        return {};

    std::vector<std::string> port_names;
    port_names.reserve(mod_entry->ports.size());
    for (const auto& p : mod_entry->ports)
        port_names.push_back(p.name);
    return port_names;
}

// ── Main implementation ───────────────────────────────────────────────────────

std::optional<AutoinstResult> autoinst_impl(const DocumentState& state, int line, int /*col*/,
                                            const SyntaxIndex& syntax_index) {
    if (!state.tree)
        return std::nullopt;

    auto& sm = state.tree->sourceManager();
    InstCollector collector(sm);
    state.tree->root().visit(collector);

    auto& candidates = collector.candidates;
    if (candidates.empty())
        return std::nullopt;

    // Sort descending by first_line
    std::sort(
        candidates.begin(), candidates.end(),
        [](const InstCandidate& a, const InstCandidate& b) { return a.first_line > b.first_line; });

    auto lines = split_lines(state.text);

    // Find the first candidate where target line <= end_line
    for (const auto& cand : candidates) {
        if (cand.first_line > line)
            continue;
        int end_line = find_inst_end(lines, cand.first_line);
        if (line > end_line)
            continue;

        // Found the instantiation
        const auto* node = cand.node;
        std::string module_type = std::string(node->type.valueText());

        // Get instance name
        std::string inst_name = module_type;
        for (uint32_t i = 0; i < node->instances.size(); ++i) {
            const auto* inst = node->instances[i];
            if (inst && inst->decl) {
                inst_name = std::string(inst->decl->name.valueText());
                break;
            }
        }

        // AST-first lookup: if the instantiated module is declared in the
        // current file, extract its ports directly from the live SyntaxTree.
        // Only fall back to the project index when same-file AST lookup fails.
        auto port_names = ports_for_module_in_current_ast(state, module_type);
        if (port_names.empty())
            port_names = ports_for_module_in_project_index(syntax_index, module_type);

        if (port_names.empty())
            return std::nullopt;

        AutoinstResult result;
        result.module_name = module_type;
        result.instance_name = inst_name;
        result.port_names = std::move(port_names);
        result.line_start = cand.first_line;
        result.line_end = end_line;
        for (const auto* inst : node->instances) {
            if (!inst)
                continue;
            for (const auto* conn : inst->connections) {
                if (const auto* named = conn ? conn->as_if<NamedPortConnectionSyntax>() : nullptr) {
                    std::string signal = simple_identifier_from_expr(named->expr);
                    if (!signal.empty())
                        result.existing_connections[std::string(named->name.valueText())] = signal;
                }
            }
            break;
        }
        return result;
    }
    return std::nullopt;
}

// ── Parse existing port connections ──────────────────────────────────────────

std::map<std::string, std::string> autoinst_parse_connections(const std::string& source,
                                                              int line_start, int line_end) {
    (void)source;
    (void)line_start;
    (void)line_end;
    return {};
}

// ── Format autoinst ───────────────────────────────────────────────────────────

std::string format_autoinst(const AutoinstResult& result, const std::string& source,
                            const AutoinstOptions& options) {
    (void)options;
    auto lines = split_lines(source);

    // Detect base indent from original line
    std::string base_indent;
    if (result.line_start < (int)lines.size()) {
        const std::string& orig = lines[result.line_start];
        size_t first_non_space = orig.find_first_not_of(" \t");
        if (first_non_space != std::string::npos)
            base_indent = orig.substr(0, first_non_space);
    }
    std::string port_indent = base_indent + "    ";

    // Parse existing connections
    auto existing = result.existing_connections;

    // Find longest port name for alignment
    size_t max_name_len = 0;
    for (const auto& p : result.port_names)
        max_name_len = std::max(max_name_len, p.size());

    std::string out;
    out += base_indent + result.module_name + " " + result.instance_name + " (\n";

    for (size_t i = 0; i < result.port_names.size(); ++i) {
        const auto& name = result.port_names[i];
        // Pad port name
        std::string padded = name;
        while (padded.size() < max_name_len)
            padded += ' ';
        std::string comma = (i + 1 < result.port_names.size()) ? "," : "";
        std::string conn = name; // default: connect to same-name signal
        auto it = existing.find(name);
        if (it != existing.end() && !it->second.empty())
            conn = it->second;
        out += port_indent + "." + padded + " (" + conn + ")" + comma + "\n";
    }
    out += base_indent + ");";
    return out;
}
