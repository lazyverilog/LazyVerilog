#include "workspace_symbols.hpp"
#include "../dynamic_file_index.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
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

// One declaration is reachable from many indexes: every open buffer that
// includes or imports it carries it in its own structural index, and the
// project shards do too.  They all describe the same declaration at the same
// place, so key on identity and keep the first.
static std::string symbol_key(const lsSymbolInformation& symbol) {
    return symbol.location.uri.raw_uri_ + '\0' + std::to_string(symbol.location.range.start.line) +
           '\0' + std::to_string(symbol.location.range.start.character) + '\0' +
           std::to_string((int)symbol.kind) + '\0' + symbol.name;
}

static void append_index_symbols(const SyntaxIndex& index, const std::string& uri,
                                 const std::string& query,
                                 std::vector<lsSymbolInformation>& symbols,
                                 std::unordered_set<std::string>& seen) {
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
        if (seen.insert(symbol_key(symbol)).second)
            symbols.push_back(std::move(symbol));
    }

    for (const auto& cls : index.classes) {
        if (matches_query(cls.name, query)) {
            const int line = to_lsp_line(cls.line);
            lsSymbolInformation symbol;
            symbol.name = cls.name;
            symbol.kind = lsSymbolKind::Class;
            const auto class_uri = index.source_uri(cls.file_id);
            symbol.location.uri.raw_uri_ = class_uri.empty() ? uri : class_uri;
            symbol.location.range.start = lsPosition(line, cls.col);
            symbol.location.range.end = lsPosition(line, cls.col + (int)cls.name.size());
            if (seen.insert(symbol_key(symbol)).second)
                symbols.push_back(std::move(symbol));
        }

        // Methods are the names a UVM codebase is actually navigated by, and the
        // outline already lists them; without this the project-wide jump can
        // find a class but never any of its tasks or functions.
        for (const auto& method : cls.methods) {
            if (!matches_query(method.name, query))
                continue;
            const int method_line = to_lsp_line(method.line);
            lsSymbolInformation member;
            member.name = method.name;
            member.kind = method.is_task ? lsSymbolKind::Method : lsSymbolKind::Function;
            const auto method_uri = index.source_uri(method.file_id);
            member.location.uri.raw_uri_ = method_uri.empty() ? uri : method_uri;
            member.location.range.start = lsPosition(method_line, method.col);
            member.location.range.end =
                lsPosition(method_line, method.col + (int)method.name.size());
            member.containerName = cls.name;
            if (seen.insert(symbol_key(member)).second)
                symbols.push_back(std::move(member));
        }
    }
}
} // namespace

std::vector<lsSymbolInformation> provide_workspace_symbols(const Analyzer& analyzer,
                                                           const WorkspaceSymbolParams& params) {
    const auto query = lower_copy(params.query);
    std::vector<lsSymbolInformation> symbols;
    std::unordered_set<std::string> seen;

    analyzer.for_each_state([&](const std::string& uri,
                                const std::shared_ptr<const DocumentState>& state) {
        if (state && state->tree)
            append_index_symbols(get_structural_index(*state), uri, query, symbols, seen);
    });

    for (const auto& extra : *analyzer.extra_index_snapshot_ptr())
        append_index_symbols(extra.index_ref(), extra.uri, query, symbols, seen);

    return symbols;
}
