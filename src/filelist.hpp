#pragma once

#include "config.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct VcodeResult {
    std::vector<std::string> files;
    std::vector<std::string> include_dirs;
};

std::string resolve_vcode_path(const std::filesystem::path& root, const Config& config);
VcodeResult load_vcode(const std::filesystem::path& root, const Config& config);

