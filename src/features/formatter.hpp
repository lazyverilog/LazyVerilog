#pragma once

#include "../config.hpp"
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

class SafeModeError : public std::runtime_error {
public:
    explicit SafeModeError(const std::string& message) : std::runtime_error(message) {}
};

std::string format_source(const std::string& source, const FormatOptions& opts);

// Where `source[begin, end)` went in `formatted`, the format_source() output
// of the whole of `source`.  Formatting changes only whitespace, so both hold
// the same non-blank characters in order, and the two spans run between the
// same ones: the range's lines, from the first line's indent to the last
// line's end.  A range edit that sliced `formatted` at the request's own line
// numbers replaced the lines asked for with whatever had moved there: a class
// header broken over three more lines deleted the last three lines of the
// range.  Empty when the range holds no non-blank text.
struct FormattedSpan {
    size_t source_begin{0};
    size_t source_end{0};
    size_t formatted_begin{0};
    size_t formatted_end{0};
};
std::optional<FormattedSpan> formatted_span(const std::string& source, const std::string& formatted,
                                            size_t begin, size_t end);
