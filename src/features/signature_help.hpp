#pragma once
#include "LibLsp/lsp/textDocument/signature_help.h"
#include "analyzer.hpp"

std::optional<lsSignatureHelp> provide_signature_help(const Analyzer& analyzer,
                                                      const lsTextDocumentPositionParams& params);
