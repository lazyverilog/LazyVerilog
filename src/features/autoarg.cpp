#include "autoarg.hpp"
#include "../syntax_index_shared.hpp"
#include <algorithm>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

using namespace slang;
using namespace slang::syntax;

static bool line_in_node(const SourceManager& sm, const SyntaxNode& node, int line) {
    auto first = node.getFirstToken();
    auto last = node.getLastToken();
    if (!first || !last || !first.location().valid() || !last.location().valid())
        return false;
    int first_line = (int)sm.getFileLineNumber(first.location()) - 1;
    int last_line = (int)sm.getFileLineNumber(last.location()) - 1;
    return first_line <= line && line <= last_line;
}

struct ModuleAtLineFinder : public SyntaxVisitor<ModuleAtLineFinder> {
    const SourceManager& sm;
    int line;
    const ModuleDeclarationSyntax* module{nullptr};

    ModuleAtLineFinder(const SourceManager& sm, int line) : sm(sm), line(line) {}

    void handle(const ModuleDeclarationSyntax& node) {
        if (!module && line_in_node(sm, node, line))
            module = &node;
        visitDefault(node);
    }
};

// The port names declared in @p node's body, and where its header's port list
// sits.  Empty when the module has no port list, no port declaration or no `;`.
static std::optional<AutoargResult> autoarg_result_for(const SourceManager& sm,
                                                       const ModuleDeclarationSyntax& node) {
    if (!node.header || !node.header->ports)
        return std::nullopt;

    std::vector<std::string> port_names;
    for (const auto* member : node.members) {
        if (!member)
            continue;
        const auto* decl = member->as_if<PortDeclarationSyntax>();
        if (!decl)
            continue;
        for (const auto* declarator : decl->declarators) {
            if (declarator) {
                std::string name = std::string(declarator->name.valueText());
                if (!name.empty())
                    port_names.push_back(std::move(name));
            }
        }
    }
    if (port_names.empty() || !node.header->semi)
        return std::nullopt;

    auto [open_line, open_col] = token_pos_line0_col0(sm, node.header->ports->getFirstToken());
    auto [end_line, end_col] = token_pos_line0_col0(sm, node.header->semi);

    AutoargResult result;
    result.port_names = std::move(port_names);
    result.module_name = std::string(node.header->name.valueText());
    result.open_line = open_line;
    result.open_col = open_col;
    result.end_line = end_line;
    result.end_col = end_col + (int)node.header->semi.rawText().size();
    return result;
}

struct AllModulesFinder : public SyntaxVisitor<AllModulesFinder> {
    const SourceManager& sm;
    std::vector<AutoargResult> results;

    AllModulesFinder(const SourceManager& sm_) : sm(sm_) {}

    void handle(const ModuleDeclarationSyntax& node) {
        // Nested modules are visited only under a module that qualified.
        if (auto result = autoarg_result_for(sm, node)) {
            results.push_back(std::move(*result));
            visitDefault(node);
        }
    }
};

std::vector<AutoargResult> autoarg_all_modules(const DocumentState& state) {
    if (!state.tree)
        return {};
    const auto& sm = state.tree->sourceManager();
    AllModulesFinder finder(sm);
    state.tree->root().visit(finder);
    return std::move(finder.results);
}

std::optional<AutoargResult> autoarg_impl(const DocumentState& state, int line) {
    if (!state.tree)
        return std::nullopt;

    const auto& sm = state.tree->sourceManager();
    ModuleAtLineFinder finder(sm, line);
    state.tree->root().visit(finder);
    if (!finder.module)
        return std::nullopt;
    return autoarg_result_for(sm, *finder.module);
}

std::string format_autoarg(
    const AutoargResult& result, const AutoargOptions& options, const FormatOptions& format_options)
{
    std::string indent((size_t)std::max(0, format_options.indent_size), ' ');
    std::string out = "(\n";
    int ports_per_line = format_options.module.non_ansi_port_per_line_enabled
                             ? format_options.module.non_ansi_port_per_line
                             : 1;
    ports_per_line = std::max(1, ports_per_line);

    for (size_t i = 0; i < result.port_names.size(); i += (size_t)ports_per_line) {
        size_t end = std::min(i + (size_t)ports_per_line, result.port_names.size());
        out += indent;
        for (size_t j = i; j < end; ++j) {
            if (j > i)
                out += ", ";
            out += result.port_names[j];
        }
        if (end < result.port_names.size())
            out += ",";
        out += "\n";
    }
    out += ");";
    return out;
}
