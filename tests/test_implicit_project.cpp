#include "implicit_project.hpp"
#include "string_utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// Unique directory per test, removed on scope exit.
struct ScanTree {
    fs::path root;

    explicit ScanTree(const std::string& name) {
        root = fs::temp_directory_path() / ("lazyverilog-implicit-" + name);
        fs::remove_all(root);
        fs::create_directories(root);
        root = fs::canonical(root);
    }
    ~ScanTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    ScanTree(const ScanTree&) = delete;
    ScanTree& operator=(const ScanTree&) = delete;

    fs::path write(const std::string& relative, std::string_view contents = "") const {
        auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << contents;
        return path;
    }

    /// The spelling a scan reports for @p relative.
    std::string indexed(const std::string& relative) const {
        return normalize_filesystem_path(root / relative).string();
    }
};

bool has(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

TEST_CASE("implicit scan collects sources and skips tool output", "[implicit-project]") {
    ScanTree tree("skip");
    tree.write("rtl/top.sv");
    tree.write("rtl/legacy.v");
    tree.write("rtl/notes.txt");
    tree.write(".git/hooks/x.sv");
    tree.write(".cache/lazyverilog/y.sv");
    tree.write("build/gen.sv");
    tree.write("build-debug/gen.sv");
    tree.write("sim/obj_dir/Vtop.sv");
    tree.write("sim/xcelium.d/w.sv");
    tree.write("fpga/proj.runs/impl.v");
    tree.write("node_modules/pkg/a.sv");

    auto scan = scan_implicit_root(tree.root);

    CHECK_FALSE(scan.over_limit);
    CHECK(scan.files == std::vector<std::string>{tree.indexed("rtl/legacy.v"),
                                                 tree.indexed("rtl/top.sv")});
    CHECK(scan.file_sizes.size() == scan.files.size());
}

TEST_CASE("implicit scan prunes a subtree with its own lazyverilog.toml", "[implicit-project]") {
    ScanTree tree("prune");
    tree.write("rtl/top.sv");
    tree.write("ip/fifo/lazyverilog.toml", "[design]\n");
    tree.write("ip/fifo/rtl/fifo.sv");

    auto scan = scan_implicit_root(tree.root);

    CHECK(scan.files == std::vector<std::string>{tree.indexed("rtl/top.sv")});
}

TEST_CASE("implicit scan puts every header directory on the include path, sorted",
          "[implicit-project]") {
    ScanTree tree("incdir");
    tree.write("rtl/top.sv");
    tree.write("zeta/defs.svh");
    tree.write("include/pkg.vh");
    tree.write("include/more.svh");
    tree.write("alpha/iface.svi");

    auto scan = scan_implicit_root(tree.root);

    CHECK(scan.include_dirs ==
          std::vector<std::string>{normalize_filesystem_path(tree.root / "alpha").string(),
                                   normalize_filesystem_path(tree.root / "include").string(),
                                   normalize_filesystem_path(tree.root / "zeta").string()});
    // Headers are include-path facts, not sources of their own.
    CHECK(scan.files == std::vector<std::string>{tree.indexed("rtl/top.sv")});
}

TEST_CASE("implicit scan gives up rather than truncating past a limit", "[implicit-project]") {
    ScanTree tree("limit");
    for (int i = 0; i < 5; ++i)
        tree.write("rtl/m" + std::to_string(i) + ".sv");

    ImplicitScanLimits limits;
    limits.max_source_files = 4;
    auto over = scan_implicit_root(tree.root, {}, limits);
    CHECK(over.over_limit);
    CHECK(over.files.empty());
    CHECK(over.include_dirs.empty());
    CHECK(over.over_limit_reason.find("4 source files") != std::string::npos);

    limits.max_source_files = 5;
    auto fits = scan_implicit_root(tree.root, {}, limits);
    CHECK_FALSE(fits.over_limit);
    CHECK(fits.files.size() == 5);

    ImplicitScanLimits entries;
    entries.max_dir_entries = 3;
    CHECK(scan_implicit_root(tree.root, {}, entries).over_limit);
}

TEST_CASE("implicit scan order does not depend on directory listing order",
          "[implicit-project]") {
    ScanTree tree("order");
    tree.write("b/z.sv");
    tree.write("a/y.sv");
    tree.write("c/x.sv");
    tree.write("a/b/w.sv");

    auto first = scan_implicit_root(tree.root);
    auto sorted = first.files;
    std::sort(sorted.begin(), sorted.end());
    CHECK(first.files == sorted);
    CHECK(scan_implicit_root(tree.root).files == first.files);
}

TEST_CASE("implicit scan honours .gitignore directory names and extensions",
          "[implicit-project]") {
    ScanTree tree("gitignore");
    tree.write(".gitignore", "# generated\ngen/\n/scratch\n*.bak.sv\n!keep/\nsrc/deep/path\n");
    tree.write("rtl/top.sv");
    tree.write("rtl/gen/auto.sv");
    tree.write("scratch/try.sv");
    tree.write("rtl/scratch/kept.sv");
    tree.write("rtl/old.bak.sv");
    tree.write("rtl/sub/.gitignore", "local/\n");
    tree.write("rtl/sub/local/x.sv");
    tree.write("local/y.sv");

    auto scan = scan_implicit_root(tree.root);

    // `.bak.sv` has extension `.sv`: only a single-extension pattern is honoured.
    CHECK(scan.files == std::vector<std::string>{tree.indexed("local/y.sv"),
                                                 tree.indexed("rtl/old.bak.sv"),
                                                 tree.indexed("rtl/scratch/kept.sv"),
                                                 tree.indexed("rtl/top.sv")});
}

TEST_CASE("implicit scan applies exclude globs relative to the root", "[implicit-project]") {
    ScanTree tree("exclude");
    tree.write("rtl/top.sv");
    tree.write("rtl/tb/tb_top.sv");
    tree.write("vendor/ip/core.sv");
    tree.write("rtl/top_old.sv");

    auto scan = scan_implicit_root(tree.root, {"vendor/**", "**/tb", "**/*_old.sv"});

    CHECK(scan.files == std::vector<std::string>{tree.indexed("rtl/top.sv")});
}

TEST_CASE("implicit glob matching", "[implicit-project]") {
    CHECK(implicit_glob_match("*.sv", "a.sv"));
    CHECK_FALSE(implicit_glob_match("*.sv", "rtl/a.sv"));
    CHECK(implicit_glob_match("**/*.sv", "a.sv"));
    CHECK(implicit_glob_match("**/*.sv", "rtl/deep/a.sv"));
    CHECK(implicit_glob_match("gen/**", "gen/"));
    CHECK(implicit_glob_match("gen/**", "gen/x/y.sv"));
    CHECK_FALSE(implicit_glob_match("gen/**", "rtl/gen/x.sv"));
    CHECK(implicit_glob_match("rtl/?.sv", "rtl/a.sv"));
    CHECK_FALSE(implicit_glob_match("rtl/?.sv", "rtl/ab.sv"));
}

TEST_CASE("implicit project mode names round-trip", "[implicit-project]") {
    for (auto mode : {ImplicitProjectMode::Off, ImplicitProjectMode::Workspace,
                      ImplicitProjectMode::Auto})
        CHECK(parse_implicit_project_mode(implicit_project_mode_name(mode)) == mode);
    CHECK_FALSE(parse_implicit_project_mode("on").has_value());
}

#ifndef _WIN32
TEST_CASE("implicit scan does not follow a symlinked directory", "[implicit-project]") {
    ScanTree tree("symlink");
    ScanTree outside("symlink-outside");
    tree.write("rtl/top.sv");
    outside.write("elsewhere.sv");
    fs::create_directory_symlink(outside.root, tree.root / "linked");
    fs::create_directory_symlink(tree.root, tree.root / "rtl" / "loop");

    auto scan = scan_implicit_root(tree.root);

    CHECK(scan.files == std::vector<std::string>{tree.indexed("rtl/top.sv")});
}
#endif

// Scan cost has to stay linear in the tree: a structurally identical tree
// twice the size takes about twice as long, never quadratically longer.
// A ratio against the same machine, never an absolute time (CLAUDE.md).
TEST_CASE("implicit scan time scales linearly with the tree", "[implicit-project][scaling]") {
    const auto build = [](ScanTree& tree, int dirs) {
        for (int d = 0; d < dirs; ++d) {
            const auto dir = "d" + std::to_string(d) + "/";
            for (int f = 0; f < 10; ++f)
                tree.write(dir + "m" + std::to_string(f) + ".sv");
            tree.write(dir + "notes.txt");
            tree.write(dir + "defs.svh");
        }
    };
    constexpr int kFactor = 4;
    ScanTree small("scaling-n");
    ScanTree large("scaling-4n");
    build(small, 60);
    build(large, 60 * kFactor);

    // One sample of the small tree is kFactor scans of it, so both samples
    // take about as long.  Compared to a single small scan, a longer sample is
    // likelier to catch a preemption on a shared runner, and min-of-N then
    // keeps the clean small scans and only noisy large ones: 2N against N once
    // measured 2.76 on macOS x64 against a 2.6 limit, with nothing quadratic.
    const auto time_ms = [](const fs::path& root, int scans) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < scans; ++i)
            REQUIRE_FALSE(scan_implicit_root(root).over_limit);
        const std::chrono::duration<double, std::milli> took =
            std::chrono::steady_clock::now() - start;
        return took.count();
    };
    (void)time_ms(small.root, 1); // warm the path memo for both trees
    (void)time_ms(large.root, 1);
    // Interleaved, so a slow stretch on a shared runner hits both trees
    // instead of only the one being timed then.
    double n = 1e9;
    double large_n = 1e9;
    for (int i = 0; i < 15; ++i) {
        n = std::min(n, time_ms(small.root, kFactor));
        large_n = std::min(large_n, time_ms(large.root, 1));
    }
    const double ratio = large_n / n;
    std::cout << "\n[implicit scan scaling] " << kFactor << " x N: " << n << " ms, " << kFactor
              << "N: " << large_n << " ms, ratio=" << ratio << "\n";
    // Linear is ~1.  A step quadratic in the tree makes the 4N scan ~4x its
    // share of four N scans; 2 is halfway in log terms.
    CHECK(ratio < 2.0);
}
