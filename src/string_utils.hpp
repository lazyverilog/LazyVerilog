#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

inline std::string trim_copy(std::string text) {
    auto first = std::find_if_not(text.begin(), text.end(),
                                   [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(),
                                  [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}


inline std::filesystem::path normalize_filesystem_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec)
        absolute = path;
    return absolute.lexically_normal();
}

inline std::string uri_from_path(const std::filesystem::path& path) {
    return "file://" + normalize_filesystem_path(path).string();
}

/// Split source text into logical lines without copying.  The returned
/// string_views refer to the input buffer, so callers must keep the source text
/// alive for as long as they use the views.  The behavior intentionally matches
/// the formatter/index helpers this replaces: a trailing newline produces a
/// final empty line, and an empty input produces one empty line.
inline std::vector<std::string_view> split_lines_view(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty())
        lines.push_back({});
    return lines;
}

/// Owning-string convenience wrapper for callers that edit lines in place or
/// store them independently of the original source buffer.
inline std::vector<std::string> split_lines_owned(std::string_view text) {
    const auto views = split_lines_view(text);
    std::vector<std::string> lines;
    lines.reserve(views.size());
    for (std::string_view line : views)
        lines.emplace_back(line);
    return lines;
}
