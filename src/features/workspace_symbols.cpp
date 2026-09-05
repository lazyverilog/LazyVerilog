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

// A project-wide query answers from every shard, so an empty or one-letter
// query matches essentially the whole design.  Clients page nothing: they
// render what arrives.  Cap the response the way clangd does (its
// `--limit-results` defaults to 100) and spend the budget on the best matches
// instead of on whichever shard happened to be walked first.
constexpr size_t kResultLimit = 100;

static std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

static int to_lsp_line(int one_based_line) { return one_based_line > 0 ? one_based_line - 1 : 0; }

// A query may name the container as well as the symbol: `my_pkg::build_phase`
// or `base_c::bump`.  Split on the last `::` so the left half filters on
// containerName and the right half on the symbol name.  A leading `::` means
// the container was spelled in full and must match exactly.
struct Query {
    std::string scope;     // lowercased, empty when the query has no `::`
    std::string name;      // lowercased
    bool scope_is_exact{false};
};

static Query split_query(const std::string& raw) {
    Query q;
    std::string text = lower_copy(raw);
    if (text.rfind("::", 0) == 0) {
        q.scope_is_exact = true;
        text = text.substr(2);
    }
    const size_t sep = text.rfind("::");
    if (sep == std::string::npos) {
        q.name = std::move(text);
        return q;
    }
    q.scope = text.substr(0, sep);
    q.name = text.substr(sep + 2);
    return q;
}

// Higher is better.  -1 means "no match".
//
// The ordering is the one a name query implies: what you typed exactly, then a
// prefix, then the start of a `_`-separated word, then anything else.  Shorter
// names win ties because they are the more specific hit for the same substring.
static int name_score(const std::string& name, const std::string& query) {
    if (name.empty())
        return -1;
    if (query.empty())
        return 0;
    const std::string lowered = lower_copy(name);
    const size_t at = lowered.find(query);
    if (at == std::string::npos)
        return -1;
    if (lowered.size() == query.size())
        return 400;
    if (at == 0)
        return 300;
    if (lowered[at - 1] == '_')
        return 200;
    return 100;
}

static bool scope_matches(const Query& query, const std::string& container) {
    if (query.scope.empty())
        return true;
    const std::string lowered = lower_copy(container);
    return query.scope_is_exact ? lowered == query.scope
                                : lowered.find(query.scope) != std::string::npos;
}

// Declarations a design is navigated by outrank the members hanging off them,
// so `uart` finds `module uart` before `uart_pkg::uart_cfg::uart_id`.
static int kind_rank(lsSymbolKind kind) {
    switch (kind) {
        case lsSymbolKind::Module:
        case lsSymbolKind::Interface:
        case lsSymbolKind::Package:
            return 3;
        case lsSymbolKind::Class:
            return 2;
        default:
            return 1;
    }
}

struct Scored {
    int score{0};
    lsSymbolInformation symbol;
};

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
                                 const Query& query, std::vector<Scored>& symbols,
                                 std::unordered_set<std::string>& seen) {
    const auto emit = [&](std::string name, lsSymbolKind kind, SourceFileID file_id, int line,
                          int col, std::string container) {
        const int score = name_score(name, query.name);
        if (score < 0 || !scope_matches(query, container))
            return;

        lsSymbolInformation symbol;
        symbol.name = std::move(name);
        symbol.kind = kind;
        const auto symbol_uri = index.source_uri(file_id);
        symbol.location.uri.raw_uri_ = symbol_uri.empty() ? uri : symbol_uri;
        const int lsp_line = to_lsp_line(line);
        symbol.location.range.start = lsPosition(lsp_line, col);
        symbol.location.range.end = lsPosition(lsp_line, col + (int)symbol.name.size());
        if (!container.empty())
            symbol.containerName = std::move(container);
        if (seen.insert(symbol_key(symbol)).second)
            symbols.push_back(Scored{score, std::move(symbol)});
    };

    for (const auto& module : index.modules) {
        lsSymbolKind kind = lsSymbolKind::Module;
        if (index.interface_names.contains(module.name))
            kind = lsSymbolKind::Interface;
        else if (index.package_names.contains(module.name))
            kind = lsSymbolKind::Package;
        emit(module.name, kind, module.file_id, module.line, module.col, {});
    }

    for (const auto& cls : index.classes) {
        emit(cls.name, lsSymbolKind::Class, cls.file_id, cls.line, cls.col, cls.parent_scope);

        // Methods are the names a UVM codebase is actually navigated by, and the
        // outline already lists them; without this the project-wide jump can
        // find a class but never any of its tasks or functions.
        for (const auto& method : cls.methods) {
            emit(method.name, method.is_task ? lsSymbolKind::Method : lsSymbolKind::Function,
                 method.file_id, method.line, method.col, cls.name);
        }
    }

    // Package and module subroutines, and typedefs.
    //
    // These live in `values` / `typedefs`, which are far larger than `modules` +
    // `classes` — on a UVM- or cva6-scale project they are the biggest
    // collections in a shard.  Walking them for an empty query would build a
    // vector of every value in the design only to throw all but kResultLimit of
    // it away, and a client sends an empty query the moment the symbol picker
    // opens.  So: only for a real query, and `emit()` scores before it allocates,
    // which keeps a non-match down to one score call.
    if (query.name.empty())
        return;

    for (const auto& value : index.values) {
        if (value.kind != "function" && value.kind != "task")
            continue;
        emit(value.name, value.kind == "task" ? lsSymbolKind::Method : lsSymbolKind::Function,
             value.file_id, value.line, value.col, value.parent_scope);
    }

    for (const auto& td : index.typedefs) {
        const lsSymbolKind kind = td.is_struct  ? lsSymbolKind::Struct
                                  : td.is_enum  ? lsSymbolKind::Enum
                                                : lsSymbolKind::TypeParameter;
        emit(td.name, kind, td.file_id, td.line, td.col, td.parent_scope);
    }
}
} // namespace

std::vector<lsSymbolInformation> provide_workspace_symbols(const Analyzer& analyzer,
                                                           const WorkspaceSymbolParams& params) {
    const auto query = split_query(params.query);
    std::vector<Scored> scored;
    std::unordered_set<std::string> seen;

    analyzer.for_each_state([&](const std::string& uri,
                                const std::shared_ptr<const DocumentState>& state) {
        if (state && state->tree)
            append_index_symbols(get_structural_index(*state), uri, query, scored, seen);
    });

    for (const auto& extra : *analyzer.extra_index_snapshot_ptr())
        append_index_symbols(extra.index_ref(), extra.uri, query, scored, seen);

    const size_t keep = std::min(scored.size(), kResultLimit);
    std::partial_sort(scored.begin(), scored.begin() + (long)keep, scored.end(),
                      [](const Scored& a, const Scored& b) {
                          if (a.score != b.score)
                              return a.score > b.score;
                          const int ra = kind_rank(a.symbol.kind), rb = kind_rank(b.symbol.kind);
                          if (ra != rb)
                              return ra > rb;
                          if (a.symbol.name.size() != b.symbol.name.size())
                              return a.symbol.name.size() < b.symbol.name.size();
                          return a.symbol.name < b.symbol.name;
                      });

    std::vector<lsSymbolInformation> symbols;
    symbols.reserve(keep);
    for (size_t i = 0; i < keep; ++i)
        symbols.push_back(std::move(scored[i].symbol));
    return symbols;
}
