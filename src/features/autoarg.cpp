#include "autoarg.hpp"
#include <algorithm>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

using namespace slang;
using namespace slang::syntax;

static std::pair<int, int> token_pos(const SourceManager& sm, const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {0, 0};
    const auto line = sm.getLineNumber(token.location());
    const auto col = sm.getColumnNumber(token.location());
    return {line > 0 ? (int)line - 1 : 0, col > 0 ? (int)col - 1 : 0};
}

static bool line_in_node(const SourceManager& sm, const SyntaxNode& node, int line) {
    auto first = node.getFirstToken();
    auto last = node.getLastToken();
    if (!first || !last || !first.location().valid() || !last.location().valid())
        return false;
    int first_line = (int)sm.getLineNumber(first.location()) - 1;
    int last_line = (int)sm.getLineNumber(last.location()) - 1;
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

struct AllModulesFinder : public SyntaxVisitor<AllModulesFinder> {
    const SourceManager& sm;
    std::vector<AutoargResult> results;

    AllModulesFinder(const SourceManager& sm_) : sm(sm_) {}

    void handle(const ModuleDeclarationSyntax& node) {
        if (!node.header || !node.header->ports)
            return;

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
        if (port_names.empty())
            return;

        auto [open_line, open_col] = token_pos(sm, node.header->ports->getFirstToken());
        if (!node.header->semi)
            return;
        auto [end_line, end_col] = token_pos(sm, node.header->semi);

        AutoargResult result;
        result.port_names = std::move(port_names);
        result.module_name = std::string(node.header->name.valueText());
        result.open_line = open_line;
        result.open_col = open_col;
        result.end_line = end_line;
        result.end_col = end_col + (int)node.header->semi.rawText().size();
        results.push_back(std::move(result));
        visitDefault(node);
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

std::optional<AutoargResult> autoarg_impl(const DocumentState& state, int line, int /*col*/) {
    if (!state.tree)
        return std::nullopt;

    const auto& sm = state.tree->sourceManager();
    ModuleAtLineFinder finder(sm, line);
    state.tree->root().visit(finder);
    const auto* module = finder.module;
    if (!module || !module->header || !module->header->ports)
        return std::nullopt;

    std::vector<std::string> port_names;
    for (const auto* member : module->members) {
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
    if (port_names.empty())
        return std::nullopt;

    auto [open_line, open_col] = token_pos(sm, module->header->ports->getFirstToken());
    auto [end_line, end_col] = token_pos(sm, module->header->semi);
    if (!module->header->semi)
        return std::nullopt;

    AutoargResult result;
    result.port_names = std::move(port_names);
    result.module_name = std::string(module->header->name.valueText());
    result.open_line = open_line;
    result.open_col = open_col;
    result.end_line = end_line;
    result.end_col = end_col + (int)module->header->semi.rawText().size();
    return result;
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
