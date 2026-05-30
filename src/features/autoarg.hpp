#pragma once
#include "../document_state.hpp"
#include "../config.hpp"
#include <optional>
#include <string>
#include <vector>

struct AutoargResult {
    std::vector<std::string> port_names;
    std::string module_name;
    int open_line{0};
    int open_col{0};
    int end_line{0};
    int end_col{0};
};

std::optional<AutoargResult> autoarg_impl(
    const DocumentState& state, int line, int col);

std::vector<AutoargResult> autoarg_all_modules(const DocumentState& state);

std::string format_autoarg(
    const AutoargResult& result, const AutoargOptions& options, const FormatOptions& format_options);
