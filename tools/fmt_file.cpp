#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "features/formatter.hpp"
#include "config.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) { std::cerr << "Usage: fmt_file <file>\n"; return 1; }
    std::ifstream f(argv[1]);
    if (!f) { std::cerr << "Cannot open " << argv[1] << "\n"; return 1; }
    std::ostringstream ss;
    ss << f.rdbuf();
    // Walk up from file's directory to find lazyverilog.toml
    auto dir = std::filesystem::absolute(std::filesystem::path(argv[1])).parent_path();
    FormatOptions opts;
    for (auto d = dir; !d.empty() && d != d.parent_path(); d = d.parent_path()) {
        if (std::filesystem::exists(d / "lazyverilog.toml")) {
            Config cfg = load_config(d, nullptr, nullptr);
            opts = cfg.format;
            break;
        }
    }
    try {
        std::string result = format_source(ss.str(), opts);
        std::cout << result;
        return 0;
    } catch (const SafeModeError& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
