#pragma once
#include "document_state.hpp"
#include <string>
#include <string_view>
#include <vector>

struct FixApplyResult {
    std::string text;
    size_t applied{0};
    size_t skipped{0};
};

/// Apply @p fixes (priority order) to @p text.  A fix is taken whole or skipped
/// whole: skipped when an edit overlaps an accepted replacement/deletion, or
/// when its replacement strictly contains / overlaps an accepted edit.  Pure
/// insertions at one offset never conflict; a later fix wraps outside an earlier
/// one.  A fix identical to an accepted one counts once and is not skipped.
FixApplyResult apply_fixes(std::string_view text, const std::vector<const FixInfo*>& fixes);
