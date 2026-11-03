#pragma once
#include "document_state.hpp"

#include <slang/diagnostics/Diagnostics.h>
#include <slang/text/SourceManager.h>

#include <vector>

/// Fixes for a slang diagnostic: casts for the conversion warnings (width,
/// sign, type, float, boolean).  Empty when none applies.  Must run while the
/// diagnostic's Compilation is alive: it reads the types and symbol it carries.
std::vector<FixInfo> slang_fixes(const slang::SourceManager& sm, const slang::Diagnostic& diag);
