#pragma once
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "analyzer.hpp"

#include <filesystem>
#include <stdexcept>

/// A rename the server will not do, with the reason to show the user.
struct RenameRefused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @p project_root: root of the project the asking file is in, empty for none.
/// A symbol declared outside it -- library code such as uvm-core -- throws
/// RenameRefused instead of offering a rename that cannot be carried out.
std::optional<PrepareRenameResult> prepare_rename(const Analyzer& analyzer,
                                                  const lsTextDocumentPositionParams& params,
                                                  const std::filesystem::path& project_root = {});

/// @p implicit_project: the file asking is in an implicit project, whose file
/// set nobody chose.  Renaming a module, interface or package declared more
/// than once there would rewrite every copy, so it throws std::runtime_error
/// with a message for the user instead.  @p project_root as for prepare_rename().
lsWorkspaceEdit provide_rename(const Analyzer& analyzer, const TextDocumentRename::Params& params,
                               bool implicit_project = false,
                               const std::filesystem::path& project_root = {});
