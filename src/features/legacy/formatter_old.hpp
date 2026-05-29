#pragma once
#include "config.hpp"
#include <stdexcept>
#include <string>

class SafeModeError : public std::runtime_error {
  public:
    explicit SafeModeError(const std::string& message) : std::runtime_error(message) {}
};

/// Format SystemVerilog source text (token-based, no CST/SyntaxTree needed).
std::string format_source(const std::string& source, const FormatOptions& opts);
