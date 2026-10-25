#pragma once
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "analyzer.hpp"

std::optional<PrepareRenameResult> prepare_rename(const Analyzer& analyzer,
                                                  const lsTextDocumentPositionParams& params);

/// @p implicit_project: the file asking is in an implicit project, whose file
/// set nobody chose.  Renaming a module, interface or package declared more
/// than once there would rewrite every copy, so it throws std::runtime_error
/// with a message for the user instead.
lsWorkspaceEdit provide_rename(const Analyzer& analyzer, const TextDocumentRename::Params& params,
                               bool implicit_project = false);
