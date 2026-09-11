#pragma once

#include "LibLsp/lsp/textDocument/foldingRange.h"
#include "analyzer.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/// The folds last produced for each document, so an edit in flight does not
/// make the editor throw every fold in the file away.
///
/// `didChange` installs a text-only `DocumentState` and parses on a worker
/// thread, so a `foldingRange` that arrives in that window finds no syntax tree
/// to fold -- and editors ask in exactly that window, because they re-request
/// folds from the `didChange` notification itself.  Answering "no folds" makes
/// Neovim apply an empty fold set to the whole buffer, and it re-requests only
/// on the next change, so the folds stay gone until the user types again.
///
/// Serving the previous snapshot's folds instead leaves them one reparse behind.
/// For the single-character edits that open this window the fold lines are
/// unchanged; for an inserted line they trail by one until the parse lands,
/// which is what the editor already does to its own cached fold levels.
class FoldingRangeCache {
public:
    /// Folds remembered for @p uri, if they were produced from a snapshot older
    /// than @p doc_version.  Empty when there is nothing usable.
    std::vector<FoldingRange> lookup(const std::string& uri, uint64_t doc_version) const;

    /// Remember @p folds as this document's answer at @p doc_version.
    void store(const std::string& uri, uint64_t doc_version, std::vector<FoldingRange> folds);

private:
    struct Entry {
        uint64_t                  doc_version{0};
        uint64_t                  used{0};
        std::vector<FoldingRange> folds;
    };

    /// One entry per buffer being edited.  Capped rather than pruned on close:
    /// a fold list is plain line numbers, and a session that visits many files
    /// should not accumulate one for every file it ever opened.
    static constexpr size_t kMaxEntries = 8;

    mutable std::mutex                             mutex_;
    mutable uint64_t                               clock_{0};
    mutable std::unordered_map<std::string, Entry> entries_;
};

/// Compute folding ranges for the requested document.
///
/// A document whose parse has not landed is still answered: from @p cache when
/// it holds an earlier result for this document, and otherwise from the token
/// scan, which needs no syntax tree.  @p cache is optional -- without one, every
/// request for such a document falls to the token scan.
std::vector<FoldingRange> provide_folding_range(const Analyzer& analyzer,
                                                const FoldingRangeRequestParams& params,
                                                FoldingRangeCache* cache = nullptr);
