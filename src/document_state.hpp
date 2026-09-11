#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "include_resolution.hpp"
#include "syntax_index.hpp"
#include <slang/text/SourceManager.h>

// Forward declarations from slang
namespace slang::syntax {
class SyntaxTree;
}
/// Pre-formatted diagnostic info extracted at parse time.
/// Avoids copying slang::Diagnostic (whose ConstantValue args are
/// not safely copyable — internal arena pointers become dangling).
struct ParseDiagInfo {
    int line{0};     // 0-based
    int col{0};      // 0-based
    int severity{3}; // lsDiagnosticSeverity: 1=Error,2=Warn,3=Info,4=Hint
    std::string message;
    std::string uri; // file URI for the diagnostic location; empty = owning document
};

/// Drop exact duplicates from @p diags, keeping the first of each.
///
/// slang reports a preprocessor problem once per macro *expansion*, and every
/// copy maps back to the same invocation location, so one bad line inside a
/// macro body becomes N identical diagnostics stacked on one position.  The
/// user has one line to fix, so they are told once.  Two different problems at
/// the same position, and the same message at different positions, both stay:
/// the key is the whole (position, severity, message) triple.
inline void dedup_parse_diagnostics(std::vector<ParseDiagInfo>& diags) {
    if (diags.size() < 2)
        return;
    std::unordered_set<std::string> seen;
    seen.reserve(diags.size());
    std::vector<ParseDiagInfo> unique;
    unique.reserve(diags.size());
    for (auto& diag : diags) {
        std::string key = std::to_string(diag.line);
        key += ':';
        key += std::to_string(diag.col);
        key += ':';
        key += std::to_string(diag.severity);
        key += ':';
        key += diag.message;
        if (!seen.insert(std::move(key)).second)
            continue;
        unique.push_back(std::move(diag));
    }
    diags.swap(unique);
}

/// Immutable snapshot of a single open document.
/// Handlers receive a shared_ptr<const DocumentState>; didChange atomically
/// swaps in a new instance. No per-document locking needed on the read path.
struct DocumentState {
    std::string uri;
    std::string text;
    // Normalized filesystem path for this URI.  Store it once per immutable
    // snapshot so request/change paths do not repeatedly call filesystem
    // normalization while holding Analyzer::map_mutex_.
    std::string normalized_path;
    // Cached LSP end position for whole-document text edits.  The snapshot text
    // is immutable, so computing this once during parse avoids a full file scan
    // every time a save/code-action response replaces the complete document.
    int end_line{0};
    int end_character{0};
    // source_manager must outlive tree (SyntaxTree holds SourceManager&).
    std::unique_ptr<slang::SourceManager> source_manager;
    std::shared_ptr<slang::syntax::SyntaxTree> tree;
    // Pre-formatted diagnostics extracted in make_state() while the
    // SyntaxTree and its arena allocators are still alive.
    std::vector<ParseDiagInfo> parse_diagnostics;
    // Normalized file:// URIs of files included while parsing this document.
    // Used to reparse open dependents when an included open buffer changes.
    // Keep both ordered vector data for published indexes/snapshots and a set
    // for O(1) dependency checks on the didChange path while map_mutex_ is held.
    std::vector<std::string> include_dependencies;
    std::unordered_set<std::string> include_dependency_set;
    // How every `include this parse saw resolved, including the ones that
    // resolved to nothing.  See IncludeResolution.
    std::vector<IncludeResolution> include_resolutions;
    // 128-bit content digests of the bytes this parse actually read, keyed by
    // file:// URI: this file's own buffer and every header it loaded from disk.
    //
    // The on-disk shard cache keys a shard on what it was built from, and
    // re-reading the file to hash it after the parse is not that: a file edited
    // in between yields a shard built from one set of bytes and keyed on
    // another, which is a false hit that survives every future launch.  A
    // header slang served from the burst's projection cache is deliberately
    // absent -- the projection is a directives-only reduction, not the file --
    // and its digest comes from the parse that first read it in full.
    //
    // Stored as a plain pair so document_state.hpp stays independent of
    // index_cache.hpp; IndexCache::Digest is the same two words.
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> parsed_digests;
    // Derived syntax index built once per immutable document snapshot.
    SyntaxIndex index;
    // Lazy structural index cache — populated on first call to get_structural_index().
    // mutable so const DocumentState& callers can warm it without a full index rebuild.
    mutable std::once_flag structural_index_once_;
    mutable SyntaxIndex structural_index_cache_;
    // Lazy dynamic/open-buffer index cache — populated on first call to
    // get_dynamic_index().
    //
    // This is intentionally separate from the structural cache above:
    //
    //   structural index:
    //       module/interface/package declarations, instances, ports, values,
    //       references, and other broad syntax facts derived from the live AST.
    //
    //   dynamic index:
    //       structural index plus open-buffer-only project facts such as imports
    //       and macro completion metadata.
    //
    // Completion, code actions, and other request handlers often need the
    // dynamic shards for "other open files".  Without this per-snapshot cache,
    // each request copies the structural shard and re-walks imports/macros for
    // every open buffer.  DocumentState is immutable and didChange replaces the
    // whole instance, so this cache needs no explicit invalidation: a new edit
    // gets a new DocumentState and therefore a fresh once_flag/cache pair.
    mutable std::once_flag dynamic_index_once_;
    mutable SyntaxIndex dynamic_index_cache_;
    uint64_t doc_version{0};
    DocumentState() = default;
    DocumentState(std::string uri, std::string text,
                  std::shared_ptr<slang::syntax::SyntaxTree> tree)
        : uri(std::move(uri)), text(std::move(text)), tree(std::move(tree)) {}
};
