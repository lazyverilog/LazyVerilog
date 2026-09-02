// Measures cold project-index startup: the work the LSP server does between
// loading lazyverilog.toml and having a usable ProjectIndexSnapshot.
//
//   index-bench <project-root> [repeat]
//
// <project-root> must contain lazyverilog.toml.  Set LAZYVERILOG_TRACE_PERF=1
// for per-file timings.
#include "analyzer.hpp"
#include "config.hpp"
#include "filelist.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "index-bench " << LAZYVERILOG_VERSION << "\n";
            return 0;
        }
    }
    if (argc < 2) {
        std::cerr << "usage: index-bench <project-root> [repeat]\n";
        return 2;
    }
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    const int repeat = argc > 2 ? std::stoi(argv[2]) : 1;

    std::string warn;
    Config config = load_config(root, &warn);
    if (!warn.empty())
        std::cerr << "[config] " << warn << "\n";
    auto vcode = load_vcode(root, config);
    std::cout << "files=" << vcode.files.size() << " incdirs=" << vcode.include_dirs.size()
              << " defines=" << config.design.define.size() << "\n";

    for (int i = 0; i < repeat; ++i) {
        Analyzer analyzer;
        analyzer.set_project_index_publish_debounce_ms(0);
        const auto start = std::chrono::steady_clock::now();
        analyzer.set_project_config(config.design.define, vcode.include_dirs, vcode.files,
                                    resolve_vcode_path(root, config));
        analyzer.wait_for_background_index_idle();
        const auto snapshot = analyzer.project_index_snapshot();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        std::cout << "run " << i << ": " << ms << " ms, shards="
                  << (snapshot ? snapshot->shards.size() : 0) << ", modules="
                  << (snapshot ? snapshot->module_by_name.size() : 0) << "\n";
    }
    return 0;
}
