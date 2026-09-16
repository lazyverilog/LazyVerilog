#include "project_root.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

/// Unique directory per test, removed on scope exit.
struct TempTree {
    fs::path root;

    explicit TempTree(const std::string& name) {
        // Named, not randomized, matching the TempDir idiom in
        // tests/test_index_cache.cpp: remove_all() first is what makes a rerun
        // after a crashed run behave like a fresh one.
        root = fs::temp_directory_path() / ("lazyverilog-project-root-" + name);
        fs::remove_all(root);
        fs::create_directories(root);
        // The temp directory is a symlink on macOS (/var -> /private/var), and
        // the resolver normalizes what it is handed.  Comparing against an
        // unresolved path would fail there for a reason that has nothing to do
        // with the lookup.
        root = fs::canonical(root);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    fs::path dir(const std::string& relative) const {
        auto path = root / relative;
        fs::create_directories(path);
        return path;
    }

    fs::path write(const std::string& relative, std::string_view contents = "") const {
        auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << contents;
        return path;
    }
};

} // namespace

TEST_CASE("a file finds the lazyverilog.toml above it", "[project-root]") {
    TempTree tree("basic");
    tree.write("lazyverilog.toml", "[design]\n");
    auto source = tree.write("rtl/core/alu.sv", "module alu; endmodule\n");

    ProjectRootResolver resolver;
    auto info = resolver.project_info(source);

    REQUIRE(info.has_value());
    CHECK(info->source_root == tree.root);
}

TEST_CASE("the nearest lazyverilog.toml wins over one higher up", "[project-root]") {
    // The case Neovim's marker-order resolution gets wrong: a `.git` at the top
    // of a monorepo outranks a nearer config, and the sub-project is served the
    // wrong one -- or none.  Proximity is the whole point of walking up.
    TempTree tree("nearest");
    tree.write("lazyverilog.toml", "[design]\n");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    auto source = tree.write("chip_a/rtl/alu.sv", "module alu; endmodule\n");

    ProjectRootResolver resolver;
    auto info = resolver.project_info(source);

    REQUIRE(info.has_value());
    CHECK(info->source_root == tree.root / "chip_a");
}

TEST_CASE("two files in one session resolve to different roots", "[project-root]") {
    // The reason the resolver answers per file rather than once per session.
    TempTree tree("multi");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module a; endmodule\n");
    auto b = tree.write("chip_b/rtl/b.sv", "module b; endmodule\n");

    ProjectRootResolver resolver;

    CHECK(resolver.project_info(a)->source_root == tree.root / "chip_a");
    CHECK(resolver.project_info(b)->source_root == tree.root / "chip_b");
}

TEST_CASE("no config anywhere above is nullopt, not a guessed root", "[project-root]") {
    // Substituting the file's own directory here is what puts a .cache
    // directory next to whatever file the user happened to open.  The caller
    // needs to be able to tell "no project" from "project rooted here".
    TempTree tree("none");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    ProjectRootResolver resolver;

    CHECK_FALSE(resolver.project_info(source).has_value());
}

TEST_CASE("a directory resolves like a file in it", "[project-root]") {
    TempTree tree("dir");
    tree.write("lazyverilog.toml", "[design]\n");
    auto dir = tree.dir("rtl/core");

    ProjectRootResolver resolver;
    auto info = resolver.project_info(dir);

    REQUIRE(info.has_value());
    CHECK(info->source_root == tree.root);
}

TEST_CASE("a directory holding the marker is its own root", "[project-root]") {
    TempTree tree("self");
    tree.write("lazyverilog.toml", "[design]\n");

    ProjectRootResolver resolver;
    auto info = resolver.project_info(tree.root);

    REQUIRE(info.has_value());
    CHECK(info->source_root == tree.root);
}

TEST_CASE("a file that does not exist still resolves", "[project-root]") {
    // didOpen can arrive for a buffer that was never written, and an indexer
    // can be handed a path from a filelist that is stale.  Neither should be
    // answered "no project" when the config above it is plainly there.
    TempTree tree("missing");
    tree.write("lazyverilog.toml", "[design]\n");

    ProjectRootResolver resolver;
    auto info = resolver.project_info(tree.root / "rtl" / "never_written.sv");

    REQUIRE(info.has_value());
    CHECK(info->source_root == tree.root);
}

TEST_CASE("a directory named lazyverilog.toml is not a root", "[project-root]") {
    // is_regular_file, not exists: a directory with that name would otherwise
    // root the project somewhere load_config() can never read.
    TempTree tree("dirmarker");
    tree.dir("lazyverilog.toml");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    ProjectRootResolver resolver;

    CHECK_FALSE(resolver.project_info(source).has_value());
}

TEST_CASE("invalidate picks up a config created after a miss was cached",
          "[project-root]") {
    // The negative cache is what makes a deep miss affordable, so this is the
    // path that has to work when the client reports a new config: the answer
    // has to change without waiting out the freshness window.
    TempTree tree("invalidate");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    ProjectRootResolver resolver;
    REQUIRE_FALSE(resolver.project_info(source).has_value());

    tree.write("lazyverilog.toml", "[design]\n");
    CHECK_FALSE(resolver.project_info(source).has_value()); // still the cached miss

    resolver.invalidate();
    auto info = resolver.project_info(source);
    REQUIRE(info.has_value());
    CHECK(info->source_root == tree.root);
}

TEST_CASE("a forced root bypasses the walk", "[project-root]") {
    // clangd's --compile-commands-dir: the answer is that directory or nothing,
    // whatever the file's ancestors hold.
    TempTree tree("forced");
    tree.write("lazyverilog.toml", "[design]\n");
    tree.write("elsewhere/lazyverilog.toml", "[design]\n");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    ProjectRootResolver resolver;
    resolver.set_forced_root(tree.root / "elsewhere");

    auto info = resolver.project_info(source);
    REQUIRE(info.has_value());
    CHECK(info->source_root == tree.root / "elsewhere");
}

TEST_CASE("a forced root without a config resolves to nothing", "[project-root]") {
    TempTree tree("forced-empty");
    tree.write("lazyverilog.toml", "[design]\n");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    ProjectRootResolver resolver;
    resolver.set_forced_root(tree.dir("empty"));

    CHECK_FALSE(resolver.project_info(source).has_value());
}

TEST_CASE("an empty path resolves to nothing", "[project-root]") {
    ProjectRootResolver resolver;
    CHECK_FALSE(resolver.project_info({}).has_value());
}

TEST_CASE("the user cache directory follows the platform convention",
          "[project-root]") {
    auto dir = user_cache_directory();

#if defined(_WIN32)
    // LOCALAPPDATA is set for any interactive Windows session; CI runners have
    // it too.  Nothing to assert about its value beyond it being absolute.
    if (dir)
        CHECK(dir->is_absolute());
#elif defined(__APPLE__)
    if (std::getenv("HOME") != nullptr) {
        REQUIRE(dir.has_value());
        CHECK(dir->filename() == "Caches");
        CHECK(dir->parent_path().filename() == "Library");
    }
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME");
        xdg != nullptr && *xdg != '\0' && fs::path(xdg).is_absolute()) {
        REQUIRE(dir.has_value());
        CHECK(*dir == fs::path(xdg));
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        REQUIRE(dir.has_value());
        CHECK(*dir == fs::path(home) / ".cache");
    }
#endif
}
