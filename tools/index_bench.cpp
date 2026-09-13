// Measures project-index startup: the work the LSP server does between loading
// lazyverilog.toml and having a usable ProjectIndexSnapshot.
//
//   index-bench <project-root> [repeat] [--cache on|off]
//
// <project-root> must contain lazyverilog.toml.  Set LAZYVERILOG_TRACE_PERF=1
// for per-file timings.
//
// The on-disk shard cache is on unless [index].cache says otherwise or --cache
// off is passed, and it persists between runs -- so with it on, only the first
// run of a *fresh* cache is a cold start and every run after it is warm.  The
// repeat loop here does not clear anything between iterations; use --cache off
// to measure the parse itself, or tools/startup_bench.py, which clears the
// cache per run unless asked for --warm.
#include "analyzer.hpp"
#include "config.hpp"
#include "filelist.hpp"
#include "perf_trace.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "index-bench " << LAZYVERILOG_VERSION << "\n";
            return 0;
        }
    }
    // --cache overrides [index].cache; absent, the config decides.  A bench that
    // ignored the config wrote a .cache directory into a project that had
    // asked, in that project's own config, for it not to be.
    std::optional<bool> cache_override;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cache" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value != "on" && value != "off") {
                std::cerr << "index-bench: --cache takes 'on' or 'off'\n";
                return 2;
            }
            cache_override = value == "on";
            continue;
        }
        positional.push_back(arg);
    }
    if (positional.empty()) {
        std::cerr << "usage: index-bench <project-root> [repeat] [--cache on|off]\n";
        return 2;
    }
    const std::filesystem::path root = std::filesystem::absolute(positional[0]);
    const int repeat = positional.size() > 1 ? std::stoi(positional[1]) : 1;

    std::string warn;
    Config config = load_config(root, &warn);
    if (!warn.empty())
        std::cerr << "[config] " << warn << "\n";
    auto vcode = load_vcode(root, config);
    const bool cache_enabled = cache_override.value_or(config.index.cache);
    // Empty root is what runs the analyzer uncached, the same way server.cpp
    // spells it.
    const std::string cache_root = cache_enabled ? root.string() : std::string{};

    std::cout << "files=" << vcode.files.size() << " incdirs=" << vcode.include_dirs.size()
              << " defines=" << config.design.define.size()
              << " cache=" << (cache_enabled ? "on" : "off") << "\n";

    for (int i = 0; i < repeat; ++i) {
        Analyzer analyzer;
        analyzer.set_project_index_publish_debounce_ms(0);
        const auto start = std::chrono::steady_clock::now();
        analyzer.set_project_config(config.design.define, vcode.include_dirs, vcode.files,
                                    resolve_vcode_path(root, config), cache_root);
        analyzer.wait_for_background_index_idle();
        const auto snapshot = analyzer.project_index_snapshot();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        std::cout << "run " << i << ": " << ms << " ms, shards="
                  << (snapshot ? snapshot->shards.size() : 0) << ", modules="
                  << (snapshot ? snapshot->module_by_name.size() : 0) << "\n";
    }
    // Phase totals, when LAZYVERILOG_TRACE_PERF asked for them.  These nest:
    // `occurrences` is part of `index_build`, and each is summed over every
    // worker, so they exceed the wall time above on a multi-core slice.
    std::cerr << perf_trace::summary();
    return 0;
}
