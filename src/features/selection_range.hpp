#pragma once
#include "LibLsp/lsp/textDocument/foldingRange.h"
#include "LibLsp/lsp/textDocument/selectionRange.h"
#include "analyzer.hpp"
#include <list>
#include <vector>

/// A selectionRange answer.  LspCpp's `SelectionRange::parent` is a raw
/// pointer, so the parent nodes live here, in a list whose node addresses
/// survive moving the whole answer.
struct SelectionRanges {
    std::vector<SelectionRange> result;
    std::list<SelectionRange> parents;
};

/// One chain per requested position, innermost first: the token under the
/// cursor, then each enclosing syntax node, then the whole document.  While a
/// parse is still running (no tree yet) it falls back to
/// `selection_range_without_tree()`.
SelectionRanges provide_selection_range(const Analyzer& analyzer, const SelectionRangeParams& params);

/// Tree-free chain for one position: the identifier under the cursor, then each
/// fold containing its line, then the whole document.
SelectionRanges selection_range_without_tree(const std::string& text,
                                             const std::vector<FoldingRange>& folds, lsPosition pos);
