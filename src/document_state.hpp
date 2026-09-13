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
// The LSP fold type.  Declared rather than included: folding is the only
// consumer, and the cache below holds it behind a shared_ptr, so this core
// header does not have to pull in the protocol headers to name it.
struct FoldingRange;
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
    //
    // Only for bytes that are *not* the file as slang read it -- today, a header
    // served from the burst's directives-only projection, whose real digest the
    // text cache carries.  Everything slang read itself is left as text in
    // parsed_texts below and hashed at most once per file per generation.
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> parsed_digests;
    // Bytes slang read, by file:// URI, pointing into this state's
    // SourceManager -- so they stay valid as long as the state does.
    //
    // Deliberately not hashed here.  A shared header is read by one parse and
    // then reached by every other file in the burst; hashing at the parse would
    // put the whole O(files x header) term back, which is the cost the digest
    // memo exists to remove.  Analyzer::remember_parsed_digests() hashes only
    // what its memo does not already hold.
    std::unordered_map<std::string, std::string_view> parsed_texts;
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
    // Folds derived from this snapshot's text, computed at most once.
    //
    // Whole-file foldingRange is the most expensive request on the edit path,
    // the editor issues one per didChange, and requests are answered one at a
    // time.  Edits that arrive faster than the server answers therefore leave
    // several fold requests queued that all resolve to whatever snapshot is
    // current when they finally run -- and after the last keystroke that is one
    // snapshot for the whole remaining queue.  Recomputing the identical answer
    // for each of them is the entire tail the user waits through.
    //
    // The slot sits behind a shared_ptr so that two snapshots of the *same*
    // text can share one.  A keystroke makes two: the text-only placeholder
    // enqueue_parse() installs, and the parsed state that replaces it.  Folds
    // are derived from the text and from nothing else, so their answers cannot
    // differ -- and the editor asks on both sides of that commit.
    //
    // Sharing rather than copying the result across is what makes it race-free.
    // The request thread computes on the placeholder while the parse worker
    // commits the new state, so a copy taken at commit time finds the slot
    // still empty about two edits in three (measured on a 57 890-line buffer),
    // and the work is done twice anyway.  One shared slot is filled by whoever
    // finishes first, whichever snapshot they were holding.
    //
    // The text is immutable, so this needs no invalidation: an edit that
    // changes the text gets a new DocumentState and an unshared, empty slot.
    struct FoldingRangeCache {
        std::mutex                                       mutex;
        std::shared_ptr<const std::vector<FoldingRange>> folds;
    };
    std::shared_ptr<FoldingRangeCache> folding_cache_ = std::make_shared<FoldingRangeCache>();

    /// The folds computed from this snapshot's text, or null if none have been.
    std::shared_ptr<const std::vector<FoldingRange>> folding_ranges() const {
        std::lock_guard<std::mutex> lock(folding_cache_->mutex);
        return folding_cache_->folds;
    }
    void set_folding_ranges(std::shared_ptr<const std::vector<FoldingRange>> folds) const {
        if (!folds)
            return;
        std::lock_guard<std::mutex> lock(folding_cache_->mutex);
        // First writer wins.  Two threads racing here computed the same answer
        // from the same text, so keeping the established pointer means callers
        // that already hold it keep comparing equal.
        if (!folding_cache_->folds)
            folding_cache_->folds = std::move(folds);
    }
    /// Answer fold requests from the same slot as @p other.
    ///
    /// Only ever called with a snapshot of byte-identical text; the caller owns
    /// that check.  Must be called before this snapshot is published, which is
    /// what keeps the assignment itself unsynchronized.
    void share_folding_cache_with(const DocumentState& other) {
        folding_cache_ = other.folding_cache_;
    }
    // The most recent snapshot of this document that had a tree, when this one
    // does not.  didChange installs a text-only placeholder and hands the parse
    // to a worker, and the editor issues its requests from that same
    // notification -- so a handler that needs an AST is normally asked during
    // the window where there is none.
    //
    // Answering nothing there is not a neutral failure: the client renders the
    // empty result, so the feature blinks out for as long as the user keeps
    // typing.  Holding the previous parse lets such a handler answer from a
    // document one keystroke old instead, which is what the user was already
    // looking at.
    //
    // Never more than one deep: a placeholder inherits its predecessor's
    // predecessor rather than pointing at another placeholder, so this retains
    // exactly one extra snapshot per open buffer and only while a reparse is in
    // flight.  Null once the parse lands, since `tree` is then this snapshot's own.
    std::shared_ptr<const DocumentState> previous_parsed;
    uint64_t doc_version{0};
    DocumentState() = default;
    DocumentState(std::string uri, std::string text,
                  std::shared_ptr<slang::syntax::SyntaxTree> tree)
        : uri(std::move(uri)), text(std::move(text)), tree(std::move(tree)) {}
};
