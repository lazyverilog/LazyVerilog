#pragma once
#include "LibLsp/lsp/textDocument/hover.h"
#include "LibLsp/lsp/lsTextDocumentPositionParams.h"
#include "analyzer.hpp"

std::optional<TextDocumentHover::Result> provide_hover(const Analyzer& analyzer,
                                                       const lsTextDocumentPositionParams& params);
