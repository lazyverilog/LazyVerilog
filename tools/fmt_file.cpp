#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "features/formatter.hpp"
#include "config.hpp"

int main(int argc, char* argv[]) {
    const char* log_path = nullptr;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log") {
            if (i + 1 >= argc) {
                std::cerr << "Usage: lazyverilog-fmt [--log <log-dir>] <file>\n";
                return 1;
            }
            log_path = argv[++i];
        }
        else
            path = argv[i];
    }
    if (!path) { std::cerr << "Usage: lazyverilog-fmt [--log <log-dir>] <file>\n"; return 1; }
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
        std::cout << result;
        return 0;
    } catch (const SafeModeError& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
