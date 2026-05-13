#pragma once
#include "LibLsp/lsp/extention/jdtls/WorkspaceSymbolParams.h"
#include "LibLsp/lsp/workspace/symbol.h"
#include "analyzer.hpp"

std::vector<lsSymbolInformation> provide_workspace_symbols(const Analyzer& analyzer,
                                                           const WorkspaceSymbolParams& params);
