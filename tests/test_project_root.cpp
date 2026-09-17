#include "analyzer.hpp"
#include "config.hpp"
#include "features/autoinst.hpp"
#include "index_cache.hpp"
#include "project_root.hpp"
#include "string_utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
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

TEST_CASE("a project that turns the cache off gets no shard directory",
          "[project-root][storage]") {
    // `[index].cache` is the project's answer, not the session's.  Gating the
    // whole storage on the server's own config made the setting depend on which
    // directory the server was launched from: with no rootUri -- what the
    // Neovim plugin sends -- that config is whatever sits above the working
    // directory, so a project that says `cache = false` because nothing may be
    // written into it got a `.cache/` regardless.
    //
    // Both halves are asserted: one project turning it off must not turn the
    // other's off with it, or the check below would pass against a storage that
    // simply never caches.
    TempTree tree("storage-cache-off");
    tree.write("chip_a/lazyverilog.toml", "[index]\ncache = true\n");
    tree.write("chip_b/lazyverilog.toml", "[index]\ncache = false\n");
    auto a = tree.write("chip_a/rtl/a.sv", "module a; endmodule\n");
    auto b = tree.write("chip_b/rtl/b.sv", "module b; endmodule\n");

    auto resolver = std::make_shared<ProjectRootResolver>();
    // The policy the server installs: that project's own config.
    IndexCacheStorage storage(resolver, [](const fs::path& source_root) {
        return source_root.empty() ? true : load_config(source_root).index.cache;
    });

    CHECK(storage.for_uri(uri_from_path(a)) != nullptr);
    CHECK(storage.for_uri(uri_from_path(b)) == nullptr);
    CHECK(fs::exists(tree.root / "chip_a" / ".cache"));
    CHECK_FALSE(fs::exists(tree.root / "chip_b" / ".cache"));
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

    const auto* from_a = snapshot->find_module("fifo", (tree.root / "chip_a/rtl/top.sv").string());
    const auto* from_b = snapshot->find_module("fifo", (tree.root / "chip_b/rtl/top.sv").string());
    REQUIRE(from_a);
    REQUIRE(from_b);
    CHECK(snapshot->module_path(*from_a) == a.string());
    CHECK(snapshot->module_path(*from_b) == b.string());
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

    const std::string asking = (tree.root / "chip_a/rtl/top.sv").string();
    const auto* crossed = snapshot->find_module("only_in_b", asking);
    REQUIRE(crossed);
    CHECK(snapshot->module_path(*crossed) == only_b.string());

    const auto* ip = snapshot->find_module("sync_2ff", asking);
    REQUIRE(ip);
    CHECK(snapshot->module_path(*ip) == shared.string());

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
