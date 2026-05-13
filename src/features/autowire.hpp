#pragma once
#include "../document_state.hpp"
#include "../syntax_index.hpp"
#include "../config.hpp"
#include <string>
#include <vector>

std::string autowire_apply(
    const DocumentState& state, const SyntaxIndex& syntax_index,
    const AutowireOptions& options);

std::vector<std::string> autowire_preview(
    const DocumentState& state, const SyntaxIndex& syntax_index,
    const AutowireOptions& options);
