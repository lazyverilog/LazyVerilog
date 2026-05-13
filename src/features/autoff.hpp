#pragma once
#include "../document_state.hpp"
#include <optional>
#include <string>
#include <vector>

struct AutoffEdit {
    int line{0};
    int character{0};
    std::string text;
};

struct AutoffPair {
    std::string src;
    std::string dst;
    bool missing_if{false};
    bool missing_else{false};
};

struct AutoffResult {
    std::vector<AutoffEdit> edits;
    std::vector<AutoffPair> pairs;  // for preview
    std::string error;
    bool warn{false};
    bool has_error{false};
    int pending_count{0};
};

AutoffResult autoff(
    const DocumentState& state, int cursor_line, const std::string& register_pattern);

AutoffResult autoff_all(
    const DocumentState& state, const std::string& register_pattern);

AutoffResult preview_autoff(
    const DocumentState& state, int cursor_line, const std::string& register_pattern);

AutoffResult preview_autoff_all(
    const DocumentState& state, const std::string& register_pattern);
