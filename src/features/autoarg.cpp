#include "autoarg.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <algorithm>

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

std::optional<AutoargResult> autoarg_impl(const DocumentState& state, int line, int /*col*/) {
    if (!state.tree)
        return std::nullopt;

    const auto& sm = state.tree->sourceManager();
    ModuleAtLineFinder finder(sm, line);
    state.tree->root().visit(finder);
    const auto* module = finder.module;
    if (!module || !module->header || !module->header->ports)
        return std::nullopt;

    // Match lazyverilogpy: autoarg updates only non-ANSI / empty headers; ANSI
    // headers already carry directions and must not be rewritten to plain names.
    if (module->header->ports->as_if<AnsiPortListSyntax>())
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

std::string format_autoarg(const AutoargResult& result, const AutoargOptions& options) {
    std::string indent((size_t)std::max(0, options.indent_size), ' ');
    std::string out = "(\n";
    for (size_t i = 0; i < result.port_names.size(); ++i) {
        out += indent;
        out += result.port_names[i];
        if (i + 1 < result.port_names.size())
            out += ",";
        out += "\n";
    }
    out += ");";
    return out;
}
