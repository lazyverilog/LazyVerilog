#include "workspace_symbols.hpp"
#include <algorithm>
#include <cctype>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>

using namespace slang;
using namespace slang::syntax;

namespace {

static std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

static int to_lsp_line(int one_based_line) { return one_based_line > 0 ? one_based_line - 1 : 0; }

static lsSymbolKind kind_for(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::ModuleDeclaration:
        return lsSymbolKind::Module;
    case SyntaxKind::InterfaceDeclaration:
        return lsSymbolKind::Interface;
    case SyntaxKind::PackageDeclaration:
        return lsSymbolKind::Package;
    case SyntaxKind::ClassDeclaration:
        return lsSymbolKind::Class;
    case SyntaxKind::ProgramDeclaration:
        return lsSymbolKind::Module;
    default:
        return lsSymbolKind::Unknown;
    }
}

} // namespace

std::vector<lsSymbolInformation> provide_workspace_symbols(const Analyzer& analyzer,
                                                           const WorkspaceSymbolParams& params) {
    const auto query = lower_copy(params.query);
    std::vector<lsSymbolInformation> symbols;

    for (const auto& extra : analyzer.extra_file_snapshots()) {
        if (!extra.state || !extra.state->tree)
            continue;
        const auto& sm = extra.state->tree->sourceManager();

        struct Visitor : public SyntaxVisitor<Visitor> {
            const SourceManager& sm;
            const std::string& uri;
            const std::string& query;
            std::vector<lsSymbolInformation>& symbols;

            Visitor(const SourceManager& sm, const std::string& uri, const std::string& query,
                    std::vector<lsSymbolInformation>& symbols)
                : sm(sm), uri(uri), query(query), symbols(symbols) {}

            void handle(const ModuleDeclarationSyntax& node) {
                const auto kind = kind_for(node.kind);
                if (kind == lsSymbolKind::Unknown)
                    return;
                const std::string name(node.header->name.valueText());
                if (name.empty())
                    return;
                if (!query.empty() && lower_copy(name).find(query) == std::string::npos)
                    return;

                const int line = to_lsp_line((int)sm.getLineNumber(node.header->name.location()));
                const int col = (int)sm.getColumnNumber(node.header->name.location()) - 1;
                lsSymbolInformation symbol;
                symbol.name = name;
                symbol.kind = kind;
                symbol.location.uri.raw_uri_ = uri;
                symbol.location.range.start = lsPosition(line, col);
                symbol.location.range.end = lsPosition(line, col + (int)name.size());
                symbols.push_back(std::move(symbol));
            }

            void handle(const ClassDeclarationSyntax& node) {
                const std::string name(node.name.valueText());
                if (name.empty())
                    return;
                if (!query.empty() && lower_copy(name).find(query) == std::string::npos)
                    return;

                const int line = to_lsp_line((int)sm.getLineNumber(node.name.location()));
                const int col = (int)sm.getColumnNumber(node.name.location()) - 1;
                lsSymbolInformation symbol;
                symbol.name = name;
                symbol.kind = lsSymbolKind::Class;
                symbol.location.uri.raw_uri_ = uri;
                symbol.location.range.start = lsPosition(line, col);
                symbol.location.range.end = lsPosition(line, col + (int)name.size());
                symbols.push_back(std::move(symbol));
            }
        };

        Visitor visitor(sm, extra.uri, query, symbols);
        extra.state->tree->root().visit(visitor);
    }

    return symbols;
}
