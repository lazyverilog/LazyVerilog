// A filelist file gets a compact SyntaxIndex shard built by the background
// indexer, scoped to its own path so a header shared by N files is not copied
// into N shards.  Opening that file in the editor replaces the shard with one
// built from the live buffer, and that replacement has to be the same shape —
// otherwise a header shared by 60 modules costs 60 copies of itself as soon as
// the modules are opened, rebuilt on every edit.
//
// These tests pin the three properties that makes it: the live shard agrees
// with the disk shard, a header declaration the file actually uses survives,
// and one it never names does not.
#include "analyzer.hpp"
#include "string_utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

constexpr int kUnusedHeaderParams = 64;

/// A one-module project whose module body comes from an `include`d header —
/// the shape an HPC RTL tree with a central parameter header has.
class SharedHeaderProject {
  public:
    SharedHeaderProject() {
        dir_ = std::filesystem::temp_directory_path() / "lazyverilog-open-shard-scope";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        // Resolve once, up front: the analyzer identifies a shard by its
        // normalized (symlink-resolved) URI, so a temp directory that is
        // itself reached through a symlink (macOS's /tmp -> /private/tmp) or a
        // short name (Windows) would give module_uri() a spelling the
        // background-compiled shard never uses, and shard_value_names()'s
        // plain string match would never see it.
        dir_ = std::filesystem::canonical(dir_);

        std::string header = "localparam int SHARED_USED = 1;\n"
                             "`define DECLARE_TAG logic tag_q\n";
        for (int i = 0; i < kUnusedHeaderParams; ++i)
            header += "localparam int SHARED_UNUSED_" + std::to_string(i) + " = 0;\n";
        write(dir_ / "defs.svh", header);

        write(module_path(), "module blk (input logic clk_i);\n"
                             "`include \"defs.svh\"\n"
                             "`DECLARE_TAG;\n"
                             "localparam int OWN_PARAM = SHARED_USED;\n"
                             "endmodule\n");
    }

    ~SharedHeaderProject() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    SharedHeaderProject(const SharedHeaderProject&) = delete;
    SharedHeaderProject& operator=(const SharedHeaderProject&) = delete;

    std::filesystem::path module_path() const { return dir_ / "blk.sv"; }
    // Must match the production file:// spelling exactly: on Windows a plain
    // "file://" + path.string() keeps backslashes and skips the extra slash a
    // drive letter needs, so it would never equal the closed-file shard's
    // uri_from_path()-derived key and shard_value_names() would look up
    // nothing at all.
    std::string module_uri() const { return uri_from_path(module_path()); }
    std::string module_text() const {
        std::ifstream in(module_path(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::filesystem::path& dir() const { return dir_; }

  private:
    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::filesystem::path dir_;
};

/// Value names the project index holds for @p uri's own shard.
std::set<std::string> shard_value_names(const Analyzer& analyzer, const std::string& uri) {
    std::set<std::string> names;
    auto snapshot = analyzer.extra_index_snapshot_ptr();
    for (const auto& entry : *snapshot) {
        if (entry.uri != uri)
            continue;
        for (const auto& value : entry.index_ref().values)
            names.insert(value.name);
    }
    return names;
}

} // namespace

TEST_CASE("open shard: an open file's shard matches its closed-file shard", "[index][scaling]") {
    SharedHeaderProject project;
    Analyzer analyzer;
    analyzer.set_project_config({}, {project.dir().string()}, {project.module_path().string()});
    analyzer.wait_for_background_index_idle();

    const auto closed = shard_value_names(analyzer, project.module_uri());
    REQUIRE_FALSE(closed.empty());

    analyzer.open(project.module_uri(), project.module_text());
    const auto opened = shard_value_names(analyzer, project.module_uri());

    CHECK(opened == closed);
}

TEST_CASE("open shard: header declarations the file uses survive scoping", "[index][scaling]") {
    SharedHeaderProject project;
    Analyzer analyzer;
    analyzer.set_project_config({}, {project.dir().string()}, {project.module_path().string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(project.module_uri(), project.module_text());

    const auto names = shard_value_names(analyzer, project.module_uri());

    // Declared by the file itself.
    CHECK(names.contains("OWN_PARAM"));
    // Declared by the header, but this file names it, so it must still resolve.
    CHECK(names.contains("SHARED_USED"));
    // Declared through a macro the header defines.  The declaration belongs to
    // the header, so only the mentions scan reaching into macro bodies keeps it
    // — this is the case that would silently break AutoWire on a codebase whose
    // signals come from header macros.
    CHECK(names.contains("tag_q"));
}

TEST_CASE("open shard: header declarations the file never names are dropped", "[index][scaling]") {
    SharedHeaderProject project;
    Analyzer analyzer;
    analyzer.set_project_config({}, {project.dir().string()}, {project.module_path().string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(project.module_uri(), project.module_text());

    const auto names = shard_value_names(analyzer, project.module_uri());
    const auto unused = std::count_if(names.begin(), names.end(), [](const std::string& name) {
        return name.starts_with("SHARED_UNUSED_");
    });

    // These are the header's bulk: nothing in this file can ever look them up,
    // and the header's own shard already records them for whoever can.
    CHECK(unused == 0);
}
