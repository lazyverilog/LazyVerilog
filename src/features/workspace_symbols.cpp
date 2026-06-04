#include "workspace_symbols.hpp"
#include "../dynamic_file_index.hpp"
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

static bool matches_query(const std::string& name, const std::string& query) {
    return !name.empty() && (query.empty() || lower_copy(name).find(query) != std::string::npos);
}

static void append_index_symbols(const SyntaxIndex& index, const std::string& uri,
                                 const std::string& query,
                                 std::vector<lsSymbolInformation>& symbols) {
    for (const auto& module : index.modules) {
        if (!matches_query(module.name, query))
            continue;

        lsSymbolKind kind = lsSymbolKind::Module;
        if (index.interface_names.contains(module.name))
            kind = lsSymbolKind::Interface;
        else if (index.package_names.contains(module.name))
            kind = lsSymbolKind::Package;

        const int line = to_lsp_line(module.line);
        lsSymbolInformation symbol;
        symbol.name = module.name;
        symbol.kind = kind;
        const auto module_uri = index.source_uri(module.file_id);
        symbol.location.uri.raw_uri_ = module_uri.empty() ? uri : module_uri;
        symbol.location.range.start = lsPosition(line, module.col);
        symbol.location.range.end = lsPosition(line, module.col + (int)module.name.size());
        symbols.push_back(std::move(symbol));
    }

    for (const auto& cls : index.classes) {
        if (!matches_query(cls.name, query))
            continue;
        const int line = to_lsp_line(cls.line);
        lsSymbolInformation symbol;
        symbol.name = cls.name;
        symbol.kind = lsSymbolKind::Class;
        const auto class_uri = index.source_uri(cls.file_id);
        symbol.location.uri.raw_uri_ = class_uri.empty() ? uri : class_uri;
        symbol.location.range.start = lsPosition(line, cls.col);
        symbol.location.range.end = lsPosition(line, cls.col + (int)cls.name.size());
        symbols.push_back(std::move(symbol));
    }
}
} // namespace

std::vector<lsSymbolInformation> provide_workspace_symbols(const Analyzer& analyzer,
                                                           const WorkspaceSymbolParams& params) {
    const auto query = lower_copy(params.query);
    std::vector<lsSymbolInformation> symbols;

    analyzer.for_each_state([&](const std::string& uri,
                                const std::shared_ptr<const DocumentState>& state) {
        if (state && state->tree)
            append_index_symbols(build_current_ast_structural_index(*state), uri, query, symbols);
    });

    for (const auto& extra : analyzer.extra_index_snapshots())
        append_index_symbols(extra.index, extra.uri, query, symbols);

    return symbols;
}
