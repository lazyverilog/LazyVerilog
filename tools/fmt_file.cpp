#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "banner.hpp"
#include "features/formatter.hpp"
#include "config.hpp"

int main(int argc, char* argv[]) {
    print_startup_banner("lazyverilog-fmt", argc, argv);

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
        else if (arg == "--version") {
            std::cout << "lazyverilog-fmt " << LAZYVERILOG_VERSION << "\n";
            return 0;
        }
        else
            path = argv[i];
    }
    if (!path) { std::cerr << "Usage: lazyverilog-fmt [-i|--in-place] [--log <log-dir>] <file>\n"; return 1; }
    // Binary on both ends: text mode on Windows turns every LF written into
    // CRLF, and turns a CRLF file into LF before the formatter ever sees it.
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot open " << path << "\n"; return 1; }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();
    // A leading UTF-8 BOM is not SystemVerilog text: set it aside and put it
    // back, so `-i` changes no byte the formatter did not decide.
    static const std::string kBom = "\xEF\xBB\xBF";
    const bool has_bom = source.compare(0, kBom.size(), kBom) == 0;
    if (has_bom)
        source.erase(0, kBom.size());
    // The file keeps the line ending its first line uses.
    const size_t first_lf = source.find('\n');
    const bool crlf = first_lf != std::string::npos && first_lf > 0 && source[first_lf - 1] == '\r';
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
        std::string result = format_source(source, opts);
        if (crlf) {
            std::string converted;
            converted.reserve(result.size() + result.size() / 32);
            for (size_t i = 0; i < result.size(); ++i) {
                if (result[i] == '\n' && (i == 0 || result[i - 1] != '\r'))
                    converted += '\r';
                converted += result[i];
            }
            result = std::move(converted);
        }
        if (has_bom)
            result.insert(0, kBom);
        if (in_place) {
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                std::cerr << "Cannot write " << path << "\n";
                return 1;
            }
            out << result;
        }
        else {
#ifdef _WIN32
            std::cout.flush();
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            std::cout << result;
        }
        return 0;
    } catch (const SafeModeError& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
