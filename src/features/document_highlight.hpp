#pragma once
#include "LibLsp/lsp/textDocument/highlight.h"
#include "analyzer.hpp"

/// Same-file occurrences of the symbol under the cursor, declaration included.
std::vector<lsDocumentHighlight> provide_document_highlight(const Analyzer& analyzer,
                                                           const lsTextDocumentPositionParams& params);
