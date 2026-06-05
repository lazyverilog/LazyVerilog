#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

inline std::string trim_copy(std::string text) {
    auto first = std::find_if_not(text.begin(), text.end(),
                                   [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(),
                                  [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

inline std::string uri_from_path(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).lexically_normal().string();
}
