#pragma once
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "analyzer.hpp"

std::optional<PrepareRenameResult> prepare_rename(const Analyzer& analyzer,
                                                  const lsTextDocumentPositionParams& params);

lsWorkspaceEdit provide_rename(const Analyzer& analyzer, const TextDocumentRename::Params& params);
