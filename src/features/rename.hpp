#pragma once
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "analyzer.hpp"

#include <stdexcept>

/// A rename the server will not do, with the reason to show the user.
struct RenameRefused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A symbol whose declaration cannot be found (a library member the server
/// never indexed) throws RenameRefused: its rename could only be partial.
std::optional<PrepareRenameResult> prepare_rename(const Analyzer& analyzer,
                                                  const lsTextDocumentPositionParams& params);

/// @p implicit_project: the file asking is in an implicit project, whose file
/// set nobody chose.  Renaming a module, interface or package declared more
/// than once there would rewrite every copy, so it throws std::runtime_error
/// with a message for the user instead.
lsWorkspaceEdit provide_rename(const Analyzer& analyzer, const TextDocumentRename::Params& params,
                               bool implicit_project = false);
