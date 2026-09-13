#pragma once

#include "LibLsp/lsp/textDocument/foldingRange.h"
#include "analyzer.hpp"
#include <vector>

/// Compute folding ranges for the requested document.
///
/// Folds come from a token scan of the document text, never from the syntax
/// tree.  Editors re-request folds from the `didChange` notification itself, so
/// the request reliably lands while the parse that notification started is still
/// running; deriving folds from the text alone means there is nothing to wait
/// for and nothing to serve stale.  The cost is that folds SystemVerilog cannot
/// resolve lexically are not produced -- instance regions in particular, since
/// `my_type_t state;` and `my_child u_inst (...);` have the same token shape.
std::vector<FoldingRange> provide_folding_range(const Analyzer& analyzer,
                                                const FoldingRangeRequestParams& params);

/// The same answer, as the shared vector the document snapshot holds.
///
/// Folds for a snapshot are computed once and then handed to every request that
/// asks for them, so the caller that only wants to keep a reference -- to serve
/// a superseded request later -- should not copy thousands of ranges to do it.
/// Null only when the document is not open.
std::shared_ptr<const std::vector<FoldingRange>>
provide_folding_range_shared(const Analyzer& analyzer, const FoldingRangeRequestParams& params);
