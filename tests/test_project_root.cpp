#include "analyzer.hpp"
#include "background_compiler.hpp"
#include "features/autoinst.hpp"
#include "features/connect.hpp"
#include "features/rename.hpp"
#include "index_cache.hpp"
#include "project_root.hpp"
#include "string_utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

// ── IndexCacheStorage ────────────────────────────────────────────────────────
//
// The point of the storage manager is that ONE index serves files from several
// projects, each writing shards beside its own lazyverilog.toml.  These tests
// assert where a shard actually lands, because that is the whole user-visible
// behaviour: "a .cache directory appeared somewhere I did not expect" is how
// the old editor-decided root was noticed in the first place.

TEST_CASE("a shard lands beside its own project's config", "[project-root][storage]") {
    TempTree tree("storage-basic");
    tree.write("lazyverilog.toml", "[design]\n");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    IndexCacheStorage storage(resolver);

    const IndexCache* cache = storage.for_uri(uri_from_path(source));
    REQUIRE(cache != nullptr);
    CHECK(cache->directory() == tree.root / ".cache" / "lazyverilog" / "index");
}

TEST_CASE("files from two projects get two cache directories",
          "[project-root][storage]") {
    TempTree tree("storage-multi");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module a; endmodule\n");
    auto b = tree.write("chip_b/rtl/b.sv", "module b; endmodule\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    IndexCacheStorage storage(resolver);

    const IndexCache* cache_a = storage.for_uri(uri_from_path(a));
    const IndexCache* cache_b = storage.for_uri(uri_from_path(b));

    REQUIRE(cache_a != nullptr);
    REQUIRE(cache_b != nullptr);
    CHECK(cache_a->directory() == tree.root / "chip_a" / ".cache" / "lazyverilog" / "index");
    CHECK(cache_b->directory() == tree.root / "chip_b" / ".cache" / "lazyverilog" / "index");
    CHECK(storage.opened().size() == 2);
}

TEST_CASE("two files in one project share one cache object",
          "[project-root][storage]") {
    // Not just the same directory -- the same object.  A cache opened per file
    // would create the directory and stat the .gitignore once per indexed file,
    // which on a shared filesystem is the cost this manager exists to avoid.
    TempTree tree("storage-share");
    tree.write("lazyverilog.toml", "[design]\n");
    auto a = tree.write("rtl/a.sv", "module a; endmodule\n");
    auto b = tree.write("rtl/sub/b.sv", "module b; endmodule\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    IndexCacheStorage storage(resolver);

    CHECK(storage.for_uri(uri_from_path(a)) == storage.for_uri(uri_from_path(b)));
    CHECK(storage.opened().size() == 1);
}

TEST_CASE("a file in no project falls back to the user cache directory",
          "[project-root][storage]") {
    TempTree tree("storage-fallback");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    IndexCacheStorage storage(resolver);

    const IndexCache* cache = storage.for_uri(uri_from_path(source));

    auto base = user_cache_directory();
    if (!base) {
        // No HOME (or LOCALAPPDATA): running uncached is the documented answer.
        CHECK(cache == nullptr);
        return;
    }

    REQUIRE(cache != nullptr);
    CHECK(cache->directory() == *base / "lazyverilog" / "index");
    // The thing this replaces: nothing was written next to the opened file.
    CHECK_FALSE(std::filesystem::exists(tree.root / ".cache"));
    CHECK_FALSE(std::filesystem::exists(tree.root / "rtl" / ".cache"));
}

TEST_CASE("the project cache is gitignored and the fallback is not",
          "[project-root][storage]") {
    // Inside a repo the directory would otherwise show up in git status for
    // everyone who runs the server; the user's own cache directory is in no
    // repository and a .gitignore there is litter.
    TempTree tree("storage-gitignore");
    tree.write("lazyverilog.toml", "[design]\n");
    auto source = tree.write("rtl/alu.sv", "module alu; endmodule\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    IndexCacheStorage storage(resolver);

    const IndexCache* cache = storage.for_uri(uri_from_path(source));
    REQUIRE(cache != nullptr);
    CHECK(std::filesystem::is_regular_file(cache->directory() / ".gitignore"));

    if (auto base = user_cache_directory()) {
        auto fallback = *base / "lazyverilog" / "index";
        std::error_code ec;
        const bool existed_before = std::filesystem::exists(fallback, ec);
        if (!existed_before) {
            TempTree orphan_tree("storage-gitignore-orphan");
            auto orphan = orphan_tree.write("a.sv", "module a; endmodule\n");
            IndexCacheStorage fallback_storage(std::make_shared<ProjectRootResolver>());
            if (fallback_storage.for_uri(uri_from_path(orphan)) != nullptr)
                CHECK_FALSE(std::filesystem::exists(fallback / ".gitignore"));
        }
    }
}

// ── Per-file parse inputs ────────────────────────────────────────────────────
//
// The point of ProjectParseInputs: one Analyzer, but each file preprocessed
// with its own project's defines and include directories -- clangd's
// per-file compile command, not a session-wide set.
//
// Every test here is built so that a session-wide set CANNOT pass it: each
// project's source only produces a module when *its* define is set, so a single
// merged define list would leave one of the two missing.

namespace {

/// The spelling the index stores for @p path.
///
/// A shard's path is normalized -- weakly_canonical, native separators -- and a
/// path built by appending "chip_a/rtl/fifo.sv" is not: on Windows those
/// forward slashes survive into the spelling, while the analyzer's copy has
/// backslashes.  Comparing the raw spelling passed on POSIX, where the two
/// happen to agree, and failed on Windows for a reason that has nothing to do
/// with what is being tested.
std::string indexed_path(const fs::path& path) {
    return normalize_filesystem_path(path).string();
}

/// Module names the project index holds, across every shard.
std::set<std::string> indexed_module_names(const Analyzer& analyzer) {
    std::set<std::string> names;
    auto snapshot = analyzer.project_index_snapshot();
    if (!snapshot)
        return names;
    for (const auto& [name, ref] : snapshot->module_by_name)
        names.insert(name);
    return names;
}

} // namespace

TEST_CASE("two projects parse under their own defines", "[project-root][parse-inputs]") {
    TempTree tree("inputs-defines");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv",
                        "`ifdef CHIP_A\nmodule m_only_in_a; endmodule\n`endif\n");
    auto b = tree.write("chip_b/rtl/b.sv",
                        "`ifdef CHIP_B\nmodule m_only_in_b; endmodule\n`endif\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(resolver);
    analyzer.set_parse_inputs_for_root(tree.root / "chip_a", {"CHIP_A"}, {});
    analyzer.set_parse_inputs_for_root(tree.root / "chip_b", {"CHIP_B"}, {});
    // No defaults: whatever these files parse with has to have come from their
    // own project's entry.
    analyzer.set_project_config({}, {}, {a.string(), b.string()});
    analyzer.wait_for_background_index_idle();

    const auto names = indexed_module_names(analyzer);
    CHECK(names.contains("m_only_in_a"));
    CHECK(names.contains("m_only_in_b"));
}

TEST_CASE("a define from one project does not leak into another",
          "[project-root][parse-inputs]") {
    // The other half: CHIP_A must NOT be set while parsing chip_b's file.  A
    // merged list would define both and index the guarded module anyway.
    TempTree tree("inputs-isolation");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module m_plain_a; endmodule\n");
    // Guarded by the *other* project's define, and by its own.  Serving chip_b
    // the wrong entry shows up as m_leaked appearing and m_plain_b vanishing;
    // merging every project's defines shows up as m_leaked appearing alongside
    // it.  Neither can be mistaken for a pass.
    auto b = tree.write("chip_b/rtl/b.sv",
                        "`ifdef CHIP_A\nmodule m_leaked; endmodule\n`endif\n"
                        "`ifdef CHIP_B\nmodule m_plain_b; endmodule\n`endif\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(resolver);
    analyzer.set_parse_inputs_for_root(tree.root / "chip_a", {"CHIP_A"}, {});
    analyzer.set_parse_inputs_for_root(tree.root / "chip_b", {"CHIP_B"}, {});
    analyzer.set_project_config({}, {}, {a.string(), b.string()});
    analyzer.wait_for_background_index_idle();

    const auto names = indexed_module_names(analyzer);
    CHECK(names.contains("m_plain_a"));
    CHECK(names.contains("m_plain_b"));
    CHECK_FALSE(names.contains("m_leaked"));
}

TEST_CASE("each project searches its own include directories",
          "[project-root][parse-inputs]") {
    // Both files include "shared.svh" by the same spelling, resolving to
    // different headers through different +incdir+ entries.  This is also what
    // the preload's include-resolution memo has to be keyed on: keyed by
    // spelling alone, whichever project resolved first would answer for both.
    TempTree tree("inputs-incdir");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    tree.write("chip_a/inc/shared.svh", "module m_header_a; endmodule\n");
    tree.write("chip_b/inc/shared.svh", "module m_header_b; endmodule\n");
    auto a = tree.write("chip_a/rtl/a.sv", "`include \"shared.svh\"\n");
    auto b = tree.write("chip_b/rtl/b.sv", "`include \"shared.svh\"\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(resolver);
    analyzer.set_parse_inputs_for_root(tree.root / "chip_a", {},
                                       {(tree.root / "chip_a" / "inc").string()});
    analyzer.set_parse_inputs_for_root(tree.root / "chip_b", {},
                                       {(tree.root / "chip_b" / "inc").string()});
    analyzer.set_project_config({}, {}, {a.string(), b.string()});
    analyzer.wait_for_background_index_idle();

    const auto names = indexed_module_names(analyzer);
    CHECK(names.contains("m_header_a"));
    CHECK(names.contains("m_header_b"));
}

TEST_CASE("a file in no registered project uses the defaults",
          "[project-root][parse-inputs]") {
    // clangd's fallback command.  Answering "no inputs" for an unconfigured
    // file would make it fail to preprocess rather than merely parse without a
    // project's defines.
    TempTree tree("inputs-fallback");
    auto loose = tree.write("loose/rtl/x.sv",
                            "`ifdef FROM_DEFAULTS\nmodule m_fallback; endmodule\n`endif\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(resolver);
    analyzer.set_project_config({"FROM_DEFAULTS"}, {}, {loose.string()});
    analyzer.wait_for_background_index_idle();

    CHECK(indexed_module_names(analyzer).contains("m_fallback"));
}

// ── Duplicate module names across projects ───────────────────────────────────
//
// SystemVerilog has no namespaces.  Two projects open in one editor session
// routinely both declare `fifo`, and the index is deliberately a union across
// them -- splitting it is what would make the buffer you were just reading
// disappear when you open a second project.  So the disambiguation happens at
// the lookup, and it ranks rather than filters: see
// ProjectIndexSnapshot::find_module().
//
// Every test below is built so the pre-existing "first declaration wins"
// behaviour CANNOT pass it: whichever of the two files won that race, one of
// the two directions asserted here would get the other project's answer.

TEST_CASE("path proximity counts whole components", "[module-proximity]") {
    // A sibling checkout is the case this exists to tell apart, and it is
    // exactly the case a raw string prefix gets wrong.
    // "w" alone: chipA_old is a different component, not a longer chipA.
    CHECK(shared_path_prefix_components("/w/chipA/rtl/top.sv", "/w/chipA_old/rtl/fifo.sv") == 1);
    // "w", "chipA", "rtl"; the file names differ.
    CHECK(shared_path_prefix_components("/w/chipA/rtl/top.sv", "/w/chipA/rtl/fifo.sv") == 3);
    CHECK(shared_path_prefix_components("/w/chipA/rtl/top.sv", "/w/chipB/rtl/fifo.sv") == 1);
    // A sibling checkout scores no better than an unrelated sibling project,
    // which is the point: neither is the project that asked.
    CHECK(shared_path_prefix_components("/w/chipA/rtl/top.sv", "/w/chipA_old/rtl/fifo.sv") ==
          shared_path_prefix_components("/w/chipA/rtl/top.sv", "/w/chipB/rtl/fifo.sv"));
    // Deeper inside the same project still beats a sibling project.
    CHECK(shared_path_prefix_components("/w/chipA/rtl/top.sv", "/w/chipA/verif/fifo.sv") >
          shared_path_prefix_components("/w/chipA/rtl/top.sv", "/w/chipB/rtl/fifo.sv"));
    // Either separator, so a Windows path and a POSIX one score alike.
    CHECK(shared_path_prefix_components("C:\\w\\chipA\\top.sv", "C:/w/chipA/fifo.sv") == 3);
    CHECK(shared_path_prefix_components("/w/a.sv", "/x/b.sv") == 0);
    CHECK(shared_path_prefix_components("", "/w/a.sv") == 0);
}

TEST_CASE("a name two projects declare resolves toward the asking file",
          "[project-root][module-proximity]") {
    TempTree tree("dup-modules");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/fifo.sv", "module fifo; endmodule\n");
    auto b = tree.write("chip_b/rtl/fifo.sv", "module fifo; endmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_extra_files({a.string(), b.string()});
    analyzer.wait_for_background_index_idle();

    auto snapshot = analyzer.project_index_snapshot();
    REQUIRE(snapshot);

    // Normalized, because that is what a real caller passes: every feature
    // hands find_module() the querying document's normalized_path.
    const auto* from_a = snapshot->find_module("fifo", indexed_path(tree.root / "chip_a/rtl/top.sv"));
    const auto* from_b = snapshot->find_module("fifo", indexed_path(tree.root / "chip_b/rtl/top.sv"));
    REQUIRE(from_a);
    REQUIRE(from_b);
    CHECK(snapshot->module_path(*from_a) == indexed_path(a));
    CHECK(snapshot->module_path(*from_b) == indexed_path(b));
}

TEST_CASE("a module only the other project declares is still found",
          "[project-root][module-proximity]") {
    // The tie-break ranks, it does not filter.  A name with one declaration
    // anywhere resolves from anywhere -- which is what a filter would break,
    // and what shared IP outside either project root depends on.
    TempTree tree("dup-crossproject");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/fifo.sv", "module fifo; endmodule\n");
    auto b = tree.write("chip_b/rtl/fifo.sv", "module fifo; endmodule\n");
    auto only_b = tree.write("chip_b/rtl/only_b.sv", "module only_in_b; endmodule\n");
    // No lazyverilog.toml above it: shared IP in no project at all.
    auto shared = tree.write("common_ip/sync.sv", "module sync_2ff; endmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_extra_files({a.string(), b.string(), only_b.string(), shared.string()});
    analyzer.wait_for_background_index_idle();

    auto snapshot = analyzer.project_index_snapshot();
    REQUIRE(snapshot);

    const std::string asking = indexed_path(tree.root / "chip_a/rtl/top.sv");
    const auto* crossed = snapshot->find_module("only_in_b", asking);
    REQUIRE(crossed);
    CHECK(snapshot->module_path(*crossed) == indexed_path(only_b));

    const auto* ip = snapshot->find_module("sync_2ff", asking);
    REQUIRE(ip);
    CHECK(snapshot->module_path(*ip) == indexed_path(shared));

    CHECK(snapshot->find_module("no_such_module", asking) == nullptr);
}

TEST_CASE("a caller with no path in hand still gets an answer",
          "[project-root][module-proximity]") {
    TempTree tree("dup-nopath");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/fifo.sv", "module fifo; endmodule\n");
    auto b = tree.write("chip_b/rtl/fifo.sv", "module fifo; endmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_extra_files({a.string(), b.string()});
    analyzer.wait_for_background_index_idle();

    auto snapshot = analyzer.project_index_snapshot();
    REQUIRE(snapshot);

    // Opting out takes the first-indexed declaration rather than nothing, and
    // gives the same one every time it is asked.
    const auto* first = snapshot->find_module("fifo", {});
    REQUIRE(first);
    CHECK(snapshot->module_path(*snapshot->find_module("fifo", {})) ==
          snapshot->module_path(*first));
}

TEST_CASE("autoinst instantiates its own project's module",
          "[project-root][module-proximity]") {
    // End to end through a feature: the two `fifo`s differ in their ports, so
    // the ports that come back name which project answered.
    TempTree tree("dup-autoinst");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/fifo.sv",
                        "module fifo(input logic i_a_clk, output logic o_a_full); endmodule\n");
    auto b = tree.write("chip_b/rtl/fifo.sv",
                        "module fifo(input logic i_b_clk, output logic o_b_full); endmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_extra_files({a.string(), b.string()});
    analyzer.wait_for_background_index_idle();

    const std::string top = "module top;\n    fifo u_fifo ();\nendmodule\n";
    const auto ports_seen_from = [&](const fs::path& top_path) {
        const std::string uri = uri_from_path(top_path);
        analyzer.open(uri, top);
        auto state = analyzer.get_state(uri);
        REQUIRE(state);
        auto result = autoinst_impl(*state, 1, 9, nullptr,
                                    analyzer.project_index_snapshot().get());
        REQUIRE(result.has_value());
        return std::set<std::string>(result->port_names.begin(), result->port_names.end());
    };

    const auto from_b = ports_seen_from(tree.root / "chip_b/rtl/top.sv");
    CHECK(from_b.contains("i_b_clk"));
    CHECK_FALSE(from_b.contains("i_a_clk"));

    const auto from_a = ports_seen_from(tree.root / "chip_a/rtl/top.sv");
    CHECK(from_a.contains("i_a_clk"));
    CHECK_FALSE(from_a.contains("i_b_clk"));
}

// ── The same rule, through the features that consume it ──────────────────────
//
// Ranking lived in ProjectIndexSnapshot::find_module(), and the guard above
// exercised it through AutoInst.  Go-to-definition, hover, references and
// Connect never called it: they scan the project's shards by name and take the
// first that answers, over a container sorted by path, so the alphabetically
// first project won every tie regardless of which file was asking.  AutoInst
// was already correct, so testing AutoInst could not see it.
//
// Each case below asserts *both* directions.  A first-match implementation
// answers both with the same project, so whichever of the two files it happens
// to pick, one direction fails.
//
// find_references() takes the rule from the other end.  Its SymbolID really is
// `module::<name>` with no project in it -- the shard indexer is syntactic and
// cannot know which file declares the `fifo` a use site means -- so ranking
// cannot help the occurrence search that follows.  What decides there is the
// *file* an occurrence was written in: an occurrence in a project that is
// neither the declaration's nor the asking file's cannot mean this declaration.
// Both escapes are load-bearing, and both are asserted below: a file under no
// project is never rejected, and the asking file's own project is always
// admitted.

namespace {

/// Two projects that both declare `fifo`, differing in ports and doc comment so
/// the answer names which project produced it.
struct TwoProjectTree {
    TempTree tree;
    fs::path a;
    fs::path b;

    explicit TwoProjectTree(const std::string& name) : tree(name) {
        tree.write("chip_a/lazyverilog.toml", "[design]\n");
        tree.write("chip_b/lazyverilog.toml", "[design]\n");
        a = tree.write("chip_a/rtl/fifo.sv",
                       "module fifo (\n"
                       "    input logic i_clk,\n"
                       "    output logic o_a_full\n"
                       ");\n"
                       "    leaf_a u_leaf ();\n"
                       "endmodule\n"
                       "module leaf_a;\n"
                       "endmodule\n");
        // One line lower, a wider clock, its own port name and its own leaf:
        // four independent ways for an answer to say which project produced it.
        b = tree.write("chip_b/rtl/fifo.sv",
                       "\n"
                       "module fifo (\n"
                       "    input logic [1:0] i_clk,\n"
                       "    output logic o_b_full\n"
                       ");\n"
                       "    leaf_b u_leaf ();\n"
                       "endmodule\n"
                       "module leaf_b;\n"
                       "endmodule\n");
    }

    void index(Analyzer& analyzer) const {
        analyzer.set_project_index_publish_debounce_ms(0);
        // The server hands the analyzer one resolver for every per-file
        // question (server.cpp).  Without it a file has no project here, and
        // the rule that a file under no project is never rejected would make
        // the reference cases below pass for the wrong reason.
        analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
        analyzer.set_extra_files({a.string(), b.string()});
        analyzer.wait_for_background_index_idle();
    }

    /// A `top` in the named project instantiating `fifo`, opened and ready.
    std::string open_top(Analyzer& analyzer, const std::string& project) const {
        const std::string uri = uri_from_path(tree.root / (project + "/rtl/top.sv"));
        analyzer.open(uri, "module top;\n"
                           "    fifo u_fifo (\n"
                           "        .i_clk(1'b0)\n"
                           "    );\n"
                           "endmodule\n");
        return uri;
    }
};

/// Run one background semantic compilation and flatten what it reported.
///
/// Every diagnostic as "<uri>: <message>", which is what these cases actually
/// assert on: which project's module was elaborated, and under whose defines.
std::string semantic_messages(Analyzer& analyzer) {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<BackgroundCompileResult> result;
    BackgroundCompiler compiler([&] { return analyzer.compilation_snapshot(); },
                                [&](BackgroundCompileResult compiled) {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    result = std::move(compiled);
                                    cv.notify_all();
                                });

    BackgroundCompilerConfig config;
    config.enabled = true;
    config.thread_count = 1;
    config.debounce_ms = 0;
    compiler.configure(config);
    compiler.schedule();
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(60), [&] { return result.has_value(); }))
            return "<background compilation did not finish>";
    }
    compiler.stop();

    std::string messages;
    for (const auto& [uri, diags] : result->diagnostics_by_uri) {
        for (const auto& diag : diags)
            messages += uri + ": " + diag.message + "\n";
    }
    return messages;
}

} // namespace

TEST_CASE("go to definition lands in the asking file's project",
          "[project-root][module-proximity]") {
    // The reported symptom: open project A, open project B in a split, go to
    // definition on an instance in B -- and land in A.
    TwoProjectTree fixture("dup-definition");
    Analyzer analyzer;
    fixture.index(analyzer);

    const auto module_declaration_from = [&](const std::string& project) {
        const std::string uri = fixture.open_top(analyzer, project);
        auto loc = analyzer.definition_of(uri, 1, 6); // cursor on `fifo`
        REQUIRE(loc.has_value());
        return loc->uri;
    };

    CHECK(module_declaration_from("chip_b") == uri_from_path(fixture.b));
    CHECK(module_declaration_from("chip_a") == uri_from_path(fixture.a));
}

TEST_CASE("a named port connection resolves in the asking project's module",
          "[project-root][module-proximity]") {
    // `.i_clk(...)` names a port both `fifo`s declare, so the port name cannot
    // say which module answered -- only the file it was found in can.
    TwoProjectTree fixture("dup-named-port");
    Analyzer analyzer;
    fixture.index(analyzer);

    const auto port_declaration_from = [&](const std::string& project) {
        const std::string uri = fixture.open_top(analyzer, project);
        auto loc = analyzer.definition_of(uri, 2, 10); // cursor on `.i_clk`
        REQUIRE(loc.has_value());
        return loc->uri;
    };

    CHECK(port_declaration_from("chip_b") == uri_from_path(fixture.b));
    CHECK(port_declaration_from("chip_a") == uri_from_path(fixture.a));
}

TEST_CASE("hover describes the asking file's module", "[project-root][module-proximity]") {
    // Hover shares definition_of_state() with go-to-definition, so it had the
    // same defect; the doc comments differ so the rendered hover names which
    // project answered.
    TwoProjectTree fixture("dup-hover");
    Analyzer analyzer;
    fixture.index(analyzer);

    const auto hover_from = [&](const std::string& project, int line, int col) {
        const std::string uri = fixture.open_top(analyzer, project);
        auto info = analyzer.symbol_at(uri, line, col);
        REQUIRE(info.has_value());
        return *info;
    };

    // `module fifo` sits one line lower in chip_b, so the declaration position
    // hover reports names the file it resolved through.  Hover on a module
    // renders a bare kind, which is the same string for both.
    CHECK(hover_from("chip_b", 1, 6).line == 1);
    CHECK(hover_from("chip_a", 1, 6).line == 0);

    // And on a port both modules declare, where the rendered type differs.
    CHECK(hover_from("chip_b", 2, 10).detail == "logic [1:0]");
    CHECK(hover_from("chip_a", 2, 10).detail == "logic");
}

TEST_CASE("connect resolves the asking project's module", "[project-root][module-proximity]") {
    // Connect builds its own view of the design in collect_files(), which had
    // neither the ranking nor a stable order -- open buffers arrived in
    // unordered_map bucket order.  The two `fifo`s differ in one port, so the
    // ports Connect reports name which project it resolved into.
    TwoProjectTree fixture("dup-connect");
    Analyzer analyzer;
    fixture.index(analyzer);

    // Expanding `top.u_fifo` resolves the instance to a module and reports what
    // that module instantiates.  Each project's fifo holds its own leaf, so the
    // child that comes back names which fifo Connect resolved into.  A listing
    // of every module in the design could not tell them apart -- both are in
    // the union index, and both are meant to be.
    const auto leaf_under_fifo_from = [&](const std::string& project) {
        const std::string uri = fixture.open_top(analyzer, project);
        return connect_hierarchy_children_json(analyzer, uri, "top.u_fifo");
    };

    const auto from_b = leaf_under_fifo_from("chip_b");
    CHECK(from_b.find("leaf_b") != std::string::npos);
    CHECK(from_b.find("leaf_a") == std::string::npos);

    const auto from_a = leaf_under_fifo_from("chip_a");
    CHECK(from_a.find("leaf_a") != std::string::npos);
    CHECK(from_a.find("leaf_b") == std::string::npos);
}

TEST_CASE("nearest-first ordering sees past the shared prefix", "[module-proximity]") {
    // find_module() answers from the prebuilt by-name table and by_path_proximity()
    // orders a candidate sequence, and CLAUDE.md is explicit that they must stay
    // on one scoring function or the features disagree about which project a
    // name belongs to.  So the term added to one has to reach the other: a
    // sibling declaration beats one three directories below the same prefix,
    // whichever entry point is asked.
    TempTree tree("proximity-same-prefix");
    tree.write("chip/lazyverilog.toml", "[design]\n");
    auto near = tree.write("chip/rtl/fifo.sv", "module fifo;\nendmodule\n");
    auto deep = tree.write("chip/rtl/sub/legacy/fifo.sv", "module fifo;\nendmodule\n");
    auto top = tree.write("chip/rtl/top.sv", "module top;\n  fifo u_fifo ();\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_project_config({}, {}, {near.string(), deep.string(), top.string()});
    analyzer.wait_for_background_index_idle();

    const auto snapshot = analyzer.project_index_snapshot();
    REQUIRE(snapshot != nullptr);
    const auto* ref = snapshot->find_module("fifo", normalize_filesystem_path(top).string());
    REQUIRE(ref != nullptr);
    CHECK(snapshot->module_path(*ref) == normalize_filesystem_path(near).string());

    // And from inside the deep directory the deep one wins, so this is ranking
    // and not a fixed preference for shallow paths.
    const auto* from_deep =
        snapshot->find_module("fifo", normalize_filesystem_path(deep).string());
    REQUIRE(from_deep != nullptr);
    CHECK(snapshot->module_path(*from_deep) == normalize_filesystem_path(deep).string());
}

TEST_CASE("nearest-first ordering ranks without filtering", "[module-proximity]") {
    // The primitive every one of the above now shares.  Three properties the
    // scans depend on: nothing is dropped, ties keep the caller's order, and a
    // single project -- the overwhelmingly common case -- is returned untouched.
    struct Candidate {
        std::string path;
    };
    const std::vector<Candidate> files{
        {"/w/chip_a/rtl/fifo.sv"},
        {"/w/chip_b/rtl/fifo.sv"},
        {"/w/common_ip/sync.sv"},
        {"/w/chip_b/verif/fifo.sv"},
    };

    const auto paths = [](const std::vector<const Candidate*>& ranked) {
        std::vector<std::string> out;
        for (const auto* c : ranked)
            out.push_back(c->path);
        return out;
    };

    const auto from_b =
        paths(by_path_proximity(std::span<const Candidate>(files), "/w/chip_b/rtl/top.sv"));
    CHECK(from_b.front() == "/w/chip_b/rtl/fifo.sv");
    // Ranks, does not filter: shared IP in no project is still reachable.
    CHECK(from_b.size() == files.size());
    CHECK(std::find(from_b.begin(), from_b.end(), "/w/common_ip/sync.sv") != from_b.end());
    // The whole order, which is clangd's directory-tree edit distance for these
    // four: 0 up/down to chip_b/rtl, 2 to chip_b/verif, 3 to common_ip, 4 to
    // chip_a/rtl.
    CHECK(from_b == std::vector<std::string>{"/w/chip_b/rtl/fifo.sv",
                                             "/w/chip_b/verif/fifo.sv",
                                             "/w/common_ip/sync.sv",
                                             "/w/chip_a/rtl/fifo.sv"});

    // No path in hand, and one project: both return the caller's order as-is.
    CHECK(paths(by_path_proximity(std::span<const Candidate>(files), "")) ==
          std::vector<std::string>{"/w/chip_a/rtl/fifo.sv", "/w/chip_b/rtl/fifo.sv",
                                   "/w/common_ip/sync.sv", "/w/chip_b/verif/fifo.sv"});

    // What the shared prefix alone cannot see: both of these share exactly
    // `w/chip_b/rtl` with the asking file, so on that term they tie and the
    // winner was whichever the caller's path order happened to put first.  The
    // sibling is nearer -- one step down against three -- and saying so is the
    // "down" half of the edit distance.
    const std::vector<Candidate> same_prefix{{"/w/chip_b/rtl/sub/legacy/fifo.sv"},
                                             {"/w/chip_b/rtl/fifo.sv"}};
    CHECK(paths(by_path_proximity(std::span<const Candidate>(same_prefix),
                                  "/w/chip_b/rtl/top.sv"))
              .front() == "/w/chip_b/rtl/fifo.sv");

    const std::vector<Candidate> one_project{{"/w/chip_a/rtl/a.sv"}, {"/w/chip_a/rtl/b.sv"}};
    CHECK(paths(by_path_proximity(std::span<const Candidate>(one_project),
                                  "/w/chip_a/rtl/top.sv")) ==
          std::vector<std::string>{"/w/chip_a/rtl/a.sv", "/w/chip_a/rtl/b.sv"});

    // The in-place variant moves the same elements into the same order.
    std::vector<Candidate> owned = files;
    order_by_path_proximity(owned, "/w/chip_b/rtl/top.sv");
    std::vector<std::string> owned_paths;
    for (const auto& c : owned)
        owned_paths.push_back(c.path);
    CHECK(owned_paths == from_b);
}

TEST_CASE("project root: the File hint answers exactly as the stat would", "[project-root]") {
    // Deciding whether a path is a file or a directory costs an is_directory()
    // the per-directory cache cannot serve: that cache is keyed on directories,
    // and this question is about the path itself.  So it was one uncached stat
    // per lookup, and lookups run per request (config_for) and per parse
    // (ProjectParseInputs::for_path) -- measured on a warm launch of a 300-file
    // project as roughly three metadata calls per project file, 1814 against
    // 902 after.
    //
    // The hint is only sound while it answers identically for a real file,
    // which is what this pins, and while nothing passes File for a directory --
    // stated here by showing what that would do.
    TempTree tree("file-hint");
    tree.write(ProjectRootResolver::kMarker, "[design]\n");
    const auto file = tree.write("rtl/core/m.sv", "module m;\nendmodule\n");

    ProjectRootResolver resolver;

    const auto stated = resolver.project_info(file, ProjectRootResolver::PathKind::File);
    const auto probed = resolver.project_info(file, ProjectRootResolver::PathKind::Unknown);
    REQUIRE(stated.has_value());
    REQUIRE(probed.has_value());
    CHECK(stated->source_root == tree.root);
    CHECK(stated->source_root == probed->source_root);

    // A directory that *is* a project root, asked about without the stat: the
    // walk starts at its parent, so the marker inside it is not seen.  Callers
    // holding a path that may be either must leave the hint at Unknown.
    const auto nested = tree.root / "rtl";
    tree.write(std::string("rtl/") + ProjectRootResolver::kMarker, "[design]\n");
    resolver.invalidate();
    CHECK(resolver.project_info(nested, ProjectRootResolver::PathKind::Unknown)->source_root ==
          nested);
    CHECK(resolver.project_info(nested, ProjectRootResolver::PathKind::File)->source_root ==
          tree.root);
}

TEST_CASE("references stay inside the asking file's project",
          "[project-root][module-proximity]") {
    // Both projects declare `fifo` and both instantiate it.  The occurrence
    // search is keyed on `module::fifo`, which is one SymbolID for both, so
    // before the file an occurrence was written in was consulted this reported
    // all four -- including the *other* project's `module fifo` declaration.
    TwoProjectTree fixture("dup-references");
    Analyzer analyzer;
    fixture.index(analyzer);

    const auto reference_uris = [&](const std::string& project) {
        const std::string uri = fixture.open_top(analyzer, project);
        std::set<std::string> uris;
        for (const auto& ref : analyzer.find_references(uri, 1, 6, true))
            uris.insert(ref.uri);
        return uris;
    };

    const auto from_b = reference_uris("chip_b");
    CHECK(from_b.contains(uri_from_path(fixture.b)));
    CHECK_FALSE(from_b.contains(uri_from_path(fixture.a)));

    const auto from_a = reference_uris("chip_a");
    CHECK(from_a.contains(uri_from_path(fixture.a)));
    CHECK_FALSE(from_a.contains(uri_from_path(fixture.b)));
}

TEST_CASE("rename does not rewrite the other project's declaration",
          "[project-root][module-proximity]") {
    // The consequence of the case above, and the reason it is worth a guard of
    // its own: over-reporting a reference is noise, but renaming through it
    // edits a file in a project the user never opened the buffer for.
    TwoProjectTree fixture("dup-rename");
    Analyzer analyzer;
    fixture.index(analyzer);

    const std::string uri = fixture.open_top(analyzer, "chip_b");
    TextDocumentRename::Params params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(1, 6); // cursor on `fifo`
    params.newName = "fifo_v2";

    auto edit = provide_rename(analyzer, params);
    REQUIRE(edit.changes.has_value());
    CHECK(edit.changes->contains(uri_from_path(fixture.b)));
    CHECK_FALSE(edit.changes->contains(uri_from_path(fixture.a)));
}

TEST_CASE("references to shared IP reach every project that uses it",
          "[project-root][module-proximity]") {
    // The case a project *filter* would break, and the reason the rule rejects
    // only what it can prove.  One `fifo`, outside every root -- a `common_ip/`
    // with no lazyverilog.toml is routine in hardware -- used by two projects.
    // Renaming it from either side has to edit both, or the other project stops
    // compiling.
    TempTree tree("shared-ip-references");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto ip = tree.write("common_ip/fifo.sv", "module fifo (input logic i_clk);\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_extra_files({ip.string()});
    analyzer.wait_for_background_index_idle();

    const auto open_top = [&](const std::string& project) {
        const std::string uri = uri_from_path(tree.root / (project + "/rtl/top.sv"));
        analyzer.open(uri, "module top;\n    fifo u_fifo ();\nendmodule\n");
        return uri;
    };
    const std::string uri_b = open_top("chip_b");
    const std::string uri_a = open_top("chip_a");

    std::set<std::string> uris;
    for (const auto& ref : analyzer.find_references(uri_b, 1, 4, true))
        uris.insert(ref.uri);

    CHECK(uris.contains(uri_from_path(ip)));
    CHECK(uris.contains(uri_b));
    // The whole point: asked from chip_b, a use in chip_a is still a use.
    CHECK(uris.contains(uri_a));
}

TEST_CASE("references from a project that borrows another's module keep the asking file",
          "[project-root][module-proximity]") {
    // Declaration in chip_a, asked from chip_b: three projects' worth of
    // answers, none of them provably wrong.  Admitting the asking file's own
    // project is what stops this returning results that omit the very file the
    // cursor is in.
    TempTree tree("borrowed-module-references");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto owned = tree.write("chip_a/rtl/fifo.sv", "module fifo (input logic i_clk);\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_extra_files({owned.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri_b = uri_from_path(tree.root / "chip_b/rtl/top.sv");
    analyzer.open(uri_b, "module top;\n    fifo u_fifo ();\nendmodule\n");

    std::set<std::string> uris;
    for (const auto& ref : analyzer.find_references(uri_b, 1, 4, true))
        uris.insert(ref.uri);

    CHECK(uris.contains(uri_from_path(owned)));
    CHECK(uris.contains(uri_b));
}

TEST_CASE("semantic compilation keeps two projects' same-named modules apart",
          "[project-root][module-proximity]") {
    // Background compilation is one slang Compilation over every open project's
    // files -- it has a single preprocessor, so it cannot be split per project
    // -- and SystemVerilog's module namespace is flat.  Two projects that both
    // declare `fifo` were therefore a redefinition to slang, which said so and
    // kept one of them.
    //
    // The undeclared identifier is the other half of the guard, and the reason
    // every library here is marked default.  It is reported only from inside a
    // module that was really elaborated, and slang never auto-instantiates a
    // definition sitting in a library -- so assigning libraries the obvious way
    // silences the redefinition by elaborating nothing at all, and this notices.
    TempTree tree("dup-compilation");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto fifo_a = tree.write("chip_a/rtl/fifo.sv", "module fifo;\n"
                                                   "    logic [1:0] w;\n"
                                                   "    assign w = undeclared_in_a;\n"
                                                   "endmodule\n");
    auto fifo_b = tree.write("chip_b/rtl/fifo.sv", "module fifo;\n"
                                                   "    logic [1:0] w;\n"
                                                   "    assign w = undeclared_in_b;\n"
                                                   "endmodule\n");
    // Each project's own top, so `fifo` is instantiated rather than picked as a
    // top-level module itself.  Two same-named tops would collide as instance
    // names under $root, which is a different problem and not one libraries
    // answer.
    auto top_a = tree.write("chip_a/rtl/top_a.sv", "module top_a;\n    fifo u_fifo ();\nendmodule\n");
    auto top_b = tree.write("chip_b/rtl/top_b.sv", "module top_b;\n    fifo u_fifo ();\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_extra_files(
        {fifo_a.string(), fifo_b.string(), top_a.string(), top_b.string()});
    analyzer.wait_for_background_index_idle();

    const std::string all_messages = semantic_messages(analyzer);
    INFO(all_messages);

    // Neither `fifo` is a redefinition of the other -- they are in different
    // libraries now, which SystemVerilog allows.
    CHECK(all_messages.find("edefinition of 'fifo'") == std::string::npos);
    CHECK(all_messages.find("duplicate definition of 'fifo'") == std::string::npos);
    // And the design still elaborates, so the diagnostics the user actually
    // wants did not go with it.
    CHECK(all_messages.find("undeclared_in_a") != std::string::npos);
}

TEST_CASE("each project compiles under its own defines", "[project-root][module-proximity]") {
    // Semantic compilation used to be one slang Compilation over the union, and
    // a Compilation has one preprocessor -- so it could only ever be handed the
    // *merged* defines.  Each module here is visible only under its own
    // project's define, so a merged compilation either sees both (if the merge
    // is what it gets) or neither.  With no defaults registered at all, the only
    // way both diagnostics appear is if each project was compiled with its own.
    TempTree tree("per-project-compilation-defines");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "`ifdef CHIP_A\n"
                                           "module a_top;\n"
                                           "    logic [1:0] w;\n"
                                           "    assign w = undeclared_in_a;\n"
                                           "endmodule\n"
                                           "`endif\n");
    auto b = tree.write("chip_b/rtl/b.sv", "`ifdef CHIP_B\n"
                                           "module b_top;\n"
                                           "    logic [1:0] w;\n"
                                           "    assign w = undeclared_in_b;\n"
                                           "endmodule\n"
                                           "`endif\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_parse_inputs_for_root(tree.root / "chip_a", {"CHIP_A"}, {});
    analyzer.set_parse_inputs_for_root(tree.root / "chip_b", {"CHIP_B"}, {});
    // No defaults: a define that reaches either file has to have come from that
    // file's own project entry.
    analyzer.set_project_config({}, {}, {a.string(), b.string()});
    analyzer.set_project_compilation_inputs({
        {.root = tree.root / "chip_a", .files = {a.string()}, .background_compilation = true},
        {.root = tree.root / "chip_b", .files = {b.string()}, .background_compilation = true},
    });
    analyzer.wait_for_background_index_idle();

    const std::string all_messages = semantic_messages(analyzer);
    INFO(all_messages);
    CHECK(all_messages.find("undeclared_in_a") != std::string::npos);
    CHECK(all_messages.find("undeclared_in_b") != std::string::npos);
}

TEST_CASE("a project with compilation off stays quiet next to one with it on",
          "[project-root][module-proximity]") {
    // The direct analog of `[lint]`, which has been per file since config_for()
    // existed.  `[compilation]` was read from the session config, so one
    // project's switch decided for every project open beside it.
    TempTree tree("per-project-compilation-switch");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module a_top;\n"
                                           "    logic [1:0] w;\n"
                                           "    assign w = undeclared_in_a;\n"
                                           "endmodule\n");
    auto b = tree.write("chip_b/rtl/b.sv", "module b_top;\n"
                                           "    logic [1:0] w;\n"
                                           "    assign w = undeclared_in_b;\n"
                                           "endmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_project_config({}, {}, {a.string(), b.string()});
    analyzer.set_project_compilation_inputs({
        {.root = tree.root / "chip_a", .files = {a.string()}, .background_compilation = true},
        {.root = tree.root / "chip_b", .files = {b.string()}, .background_compilation = false},
    });
    analyzer.wait_for_background_index_idle();

    const std::string all_messages = semantic_messages(analyzer);
    INFO(all_messages);
    CHECK(all_messages.find("undeclared_in_a") != std::string::npos);
    CHECK(all_messages.find("undeclared_in_b") == std::string::npos);
}

TEST_CASE("a buffer under no project joins no compilation group",
          "[project-root][module-proximity]") {
    // Not an oversight -- it is what that file's config says.  `config_for()`
    // resolves a file with no `lazyverilog.toml` above it to `Config{}`, and
    // `[compilation].background_compilation` defaults to false, so
    // `publish_diagnostics()` never publishes semantic diagnostics for such a
    // buffer.  Compiling it anyway is work whose result is always discarded.
    //
    // Measured against the real server before this guard was written: with an
    // orphan group in place, `common_ip/shared.sv` was compiled and its
    // diagnostic never appeared in a single `publishDiagnostics` notification,
    // while the project beside it published its own.  A user who wants these
    // adds a `lazyverilog.toml` to the shared tree, which is the same opt-in
    // every other per-project setting takes.
    //
    // This asserts the grouping directly rather than through the compiler: a
    // compiler-level check passes whether or not the publish gate would ever
    // let the result out, which is exactly how the wasted work went unnoticed.
    TempTree tree("no-project-buffer-forms-no-group");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module a_top;\nendmodule\n");
    auto ip = tree.write("common_ip/shared.sv", "module shared_ip;\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_project_config({}, {}, {a.string()});
    analyzer.set_project_compilation_inputs({
        {.root = tree.root / "chip_a", .files = {a.string()}, .background_compilation = true},
    });
    analyzer.open(uri_from_path(ip), "module shared_ip;\nendmodule\n");
    analyzer.wait_for_background_index_idle();

    const auto snapshot = analyzer.compilation_snapshot();
    const auto ip_path = normalize_filesystem_path(ip).string();
    const auto a_path = normalize_filesystem_path(a).string();

    bool ip_grouped = false;
    bool a_grouped = false;
    for (const auto& group : snapshot.groups) {
        for (const auto index : group.files) {
            const auto& file = snapshot.files[index];
            if (file.path == ip_path)
                ip_grouped = true;
            if (file.path == a_path)
                a_grouped = true;
        }
    }
    // The project that asked for compilation still gets it...
    CHECK(a_grouped);
    // ...and the buffer whose config never asked does not.
    CHECK_FALSE(ip_grouped);
    // The snapshot still carries the buffer -- the index needs every open
    // buffer, and `files` is what exists rather than what is compiled.  Only
    // the grouping leaves it out.
    CHECK(std::any_of(snapshot.files.begin(), snapshot.files.end(),
                      [&](const CompilationSourceFile& f) { return f.path == ip_path; }));
}

TEST_CASE("an unlisted buffer respects its own project's compilation switch",
          "[project-root][module-proximity]") {
    // The other half of the rule above: a buffer *does* have a project, that
    // project's filelist just does not name it yet -- a file created a moment
    // ago, or opened before being added to the `.f`.  It joins its project's
    // group, so the project's switch is what decides, not the fact that the
    // filelist is out of date.
    TempTree tree("unlisted-buffer-respects-switch");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module a_top;\n"
                                           "    logic [1:0] w;\n"
                                           "    assign w = undeclared_in_a;\n"
                                           "endmodule\n");
    auto b = tree.write("chip_b/rtl/b.sv", "module b_top;\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_project_config({}, {}, {a.string()});
    analyzer.set_project_compilation_inputs({
        {.root = tree.root / "chip_a", .files = {a.string()}, .background_compilation = true},
        {.root = tree.root / "chip_b", .files = {}, .background_compilation = false},
    });
    analyzer.open(uri_from_path(b), "module b_top;\n"
                                    "    logic [1:0] w;\n"
                                    "    assign w = undeclared_in_b;\n"
                                    "endmodule\n");
    analyzer.wait_for_background_index_idle();

    const std::string all_messages = semantic_messages(analyzer);
    INFO(all_messages);
    CHECK(all_messages.find("undeclared_in_a") != std::string::npos);
    CHECK(all_messages.find("undeclared_in_b") == std::string::npos);
}

TEST_CASE("every project declining compilation leaves nothing to compile",
          "[project-root][module-proximity]") {
    // The fallback group and the per-project groups answer different questions,
    // and only one thing may pick between them: whether any project registered
    // what it compiles.  Choosing on "did the project loop produce a group"
    // instead conflated "nobody told us what to compile" with "everybody told us
    // not to" -- so a session whose projects all have `[compilation]` off fell
    // through to the merged fallback and compiled the union of their filelists
    // anyway, under one project's defines, with publish_diagnostics() then
    // discarding every result.
    //
    // Reachable from the session config alone: the background compiler runs
    // when *anybody* wants it, and `config_.compilation.background_compilation`
    // is enough to start it even when no project's own switch is on.
    //
    // The grouping is what is asserted, not the compiler's output.  A
    // compiler-level check passes whether or not the publish gate would ever let
    // the result out, which is how this went unnoticed in the first place.
    TempTree tree("all-projects-decline-compilation");
    tree.write("chip_a/lazyverilog.toml", "[design]\n");
    tree.write("chip_b/lazyverilog.toml", "[design]\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module a_top;\nendmodule\n");
    auto b = tree.write("chip_b/rtl/b.sv", "module b_top;\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_project_config({}, {}, {a.string(), b.string()});
    analyzer.set_project_compilation_inputs({
        {.root = tree.root / "chip_a", .files = {a.string()}, .background_compilation = false},
        {.root = tree.root / "chip_b", .files = {b.string()}, .background_compilation = false},
    });
    analyzer.wait_for_background_index_idle();

    const auto snapshot = analyzer.compilation_snapshot();
    CHECK(snapshot.groups.empty());
    // The files are still in the snapshot -- `files` is what the analyzer knows
    // about, and the index needs all of it.  Only the grouping is empty, which
    // is the difference between "compiled and thrown away" and "not compiled".
    CHECK(snapshot.files.size() >= 2);
}

TEST_CASE("a session with no registered project still compiles as one group",
          "[project-root][module-proximity]") {
    // The other side of the same branch, and the reason it cannot simply be
    // deleted: a CLI tool, a test, or a client that sent no rootUri registers no
    // project at all.  Everything the analyzer knows about becomes one group
    // against the merged defaults, exactly as it did before groups existed.
    TempTree tree("no-registered-project-one-group");
    auto a = tree.write("rtl/a.sv", "module a_top;\nendmodule\n");
    auto b = tree.write("rtl/b.sv", "module b_top;\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_root_resolver(std::make_shared<ProjectRootResolver>());
    analyzer.set_project_config({}, {}, {a.string(), b.string()});
    analyzer.wait_for_background_index_idle();

    const auto snapshot = analyzer.compilation_snapshot();
    REQUIRE(snapshot.groups.size() == 1);
    // An empty root is what marks the fallback, and it carries every file.
    CHECK(snapshot.groups.front().root.empty());
    CHECK(snapshot.groups.front().files.size() == snapshot.files.size());
    CHECK(snapshot.files.size() >= 2);
}
