#pragma once
#include "LibLsp/lsp/lsTextDocumentIdentifier.h"
#include "LibLsp/lsp/textDocument/references.h"
#include "analyzer.hpp"

std::vector<lsLocation> provide_references(const Analyzer& analyzer,
                                           const TextDocumentReferences::Params& params);
