#pragma once
#include "../document_state.hpp"
#include "../syntax_index.hpp"
#include "../config.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

struct AutoinstResult {
    std::string module_name;
    std::string instance_name;
    std::vector<std::string> port_names;
    int line_start{0};
    int line_end{0};
};

std::optional<AutoinstResult> autoinst_impl(
    const DocumentState& state, int line, int col, const SyntaxIndex& syntax_index);

std::string format_autoinst(
    const AutoinstResult& result, const std::string& source, const AutoinstOptions& options);

std::map<std::string, std::string> autoinst_parse_connections(
    const std::string& source, int line_start, int line_end);
