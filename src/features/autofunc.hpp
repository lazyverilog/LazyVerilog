#pragma once
#include "../analyzer.hpp"
#include "../config.hpp"
#include "LibLsp/lsp/lsWorkspaceEdit.h"
#include <optional>
#include <string>

std::optional<lsWorkspaceEdit> autofunc(
    const Analyzer& analyzer, const std::string& uri,
    int line, int col, const AutoFuncOptions& options);
