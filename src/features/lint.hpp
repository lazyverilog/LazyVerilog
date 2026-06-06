#pragma once
#include "document_state.hpp"
#include "config.hpp"
#include <vector>

/// Run all enabled lint rules against state.
/// Returns ParseDiagInfo items merged with parse diagnostics by the caller.
std::vector<ParseDiagInfo> run_lint(const DocumentState& state, const LintConfig& config,
                                    const SyntaxIndex* current_index = nullptr,
                                    const SyntaxIndex* project_index = nullptr);
