#pragma once

#include "LibLsp/lsp/textDocument/document_symbol.h"
#include "analyzer.hpp"
#include <vector>

std::vector<lsDocumentSymbol> provide_document_symbols(const Analyzer& analyzer,
                                                        const lsDocumentSymbolParams& params);
