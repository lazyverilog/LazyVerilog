#pragma once

#include "LibLsp/lsp/textDocument/inlayHint.h"
#include "analyzer.hpp"

#include <string>
#include <vector>

std::vector<lsInlayHint> provide_inlay_hints(const Analyzer& analyzer, const std::string& uri,
                                             int range_start_line, int range_end_line);
