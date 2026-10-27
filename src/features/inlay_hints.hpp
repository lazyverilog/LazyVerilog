#pragma once

#include "LibLsp/lsp/textDocument/inlayHint.h"
#include "analyzer.hpp"

#include <chrono>
#include <string>
#include <vector>

/// Waits up to @p parse_wait for an in-flight parse (none by default), then
/// answers from the last tree that parsed.  @p answered_stale is set when that
/// tree is not the document's current text, so the caller can ask the client
/// to re-request once the parse lands.
std::vector<lsInlayHint> provide_inlay_hints(
    const Analyzer& analyzer, const std::string& uri, int range_start_line, int range_end_line,
    bool* answered_stale = nullptr,
    std::chrono::milliseconds parse_wait = std::chrono::milliseconds(0));
