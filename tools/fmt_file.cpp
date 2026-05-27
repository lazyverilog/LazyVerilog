#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "features/formatter.hpp"
#include "config.hpp"

int main(int argc, char* argv[]) {
    const char* log_path = nullptr;
    const char* path = nullptr;
    bool in_place = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log") {
            if (i + 1 >= argc) {
                std::cerr << "Usage: lazyverilog-fmt [-i|--in-place] [--log <log-dir>] <file>\n";
                return 1;
            }
            log_path = argv[++i];
        }
        else if (arg == "-i" || arg == "--in-place") {
            in_place = true;
        }
        else
            path = argv[i];
    }
    if (!path) { std::cerr << "Usage: lazyverilog-fmt [-i|--in-place] [--log <log-dir>] <file>\n"; return 1; }
    std::ifstream f(path);
    if (!f) { std::cerr << "Cannot open " << path << "\n"; return 1; }
    std::ostringstream ss;
    ss << f.rdbuf();
    // Walk up from file's directory to find lazyverilog.toml
    auto dir = std::filesystem::absolute(std::filesystem::path(path)).parent_path();
    FormatOptions opts;
    for (auto d = dir; !d.empty() && d != d.parent_path(); d = d.parent_path()) {
        if (std::filesystem::exists(d / "lazyverilog.toml")) {
            Config cfg = load_config(d, nullptr, nullptr);
            opts = cfg.format;
            break;
        }
    }
    if (log_path)
        opts.log_path = log_path;
    try {
        std::string result = format_source(ss.str(), opts);
        if (in_place) {
            std::ofstream out(path);
            if (!out) {
                std::cerr << "Cannot write " << path << "\n";
                return 1;
            }
            out << result;
        }
        else {
            std::cout << result;
        }
        return 0;
    } catch (const SafeModeError& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
