// didChange builds a fresh SourceManager per snapshot, so every keystroke used
// to re-open and re-read every header the buffer `include`s.  With one large
// shared header on a networked filesystem that read, not the parse, is what the
// user feels while typing.
//
// OpenParseHeaderCache holds that text across keystrokes and never revalidates
// it against the filesystem; the client's OS watcher reports changed files and
// the server drops those entries.  These tests pin both halves of the deal: the
// read is genuinely skipped while nothing reports a change, and a reported
// change is picked up.
#include "analyzer.hpp"
#include "dynamic_file_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

/// A temp directory holding one header and one module that includes it.
class HeaderProject {
  public:
    explicit HeaderProject(const std::string& tag) {
        dir_ = std::filesystem::temp_directory_path() / ("lazyverilog-open-header-" + tag);
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        write_header("CACHED_MARK");
        write(module_path(), module_text());
    }

    ~HeaderProject() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    HeaderProject(const HeaderProject&) = delete;
    HeaderProject& operator=(const HeaderProject&) = delete;

    /// Marker names must be the same length in every call, so that rewriting the
    /// header leaves its size unchanged and mtime is the only thing separating
    /// the two versions.
    void write_header(const std::string& marker) {
        write(header_path(), "localparam int " + marker + " = 1;\n");
    }

    std::filesystem::path header_path() const { return dir_ / "marker.svh"; }
    std::filesystem::path module_path() const { return dir_ / "blk.sv"; }
    std::string module_uri() const { return "file://" + module_path().string(); }
    std::string header_uri() const { return "file://" + header_path().string(); }

    /// Two spellings so a reparse is a real content change, not a no-op.
    static std::string module_text(const std::string& port = "clk_i") {
        return "module blk (input logic " + port + ");\n"
               "`include \"marker.svh\"\n"
               "endmodule\n";
    }

  private:
    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::filesystem::path dir_;
};

bool has_value_named(const Analyzer& analyzer, const std::string& uri, const std::string& name) {
    auto state = analyzer.get_state(uri);
    if (!state)
        return false;
    const auto& index = get_structural_index(*state);
    return std::any_of(index.values.begin(), index.values.end(),
                       [&](const ValueEntry& value) { return value.name == name; });
}

} // namespace

TEST_CASE("open header cache: a keystroke does not re-read an unchanged header", "[sync]") {
    HeaderProject project("reuse");
    Analyzer analyzer;
    analyzer.open(project.module_uri(), HeaderProject::module_text());
    REQUIRE(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));

    // Rewrite the header on disk and say nothing.  Size and mtime both move, so
    // this is exactly the edit a revalidating cache would notice; the point of
    // the change is that the keystroke path asks the filesystem nothing at all.
    project.write_header("FRESHX_MARK");
    std::filesystem::last_write_time(project.header_path(),
                                     std::filesystem::last_write_time(project.header_path()) +
                                         std::chrono::seconds(2));

    analyzer.change(project.module_uri(), HeaderProject::module_text("clk"));
    CHECK(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));
    CHECK_FALSE(has_value_named(analyzer, project.module_uri(), "FRESHX_MARK"));
}

TEST_CASE("open header cache: a watcher-reported header change is picked up", "[sync]") {
    HeaderProject project("refresh");
    Analyzer analyzer;
    analyzer.open(project.module_uri(), HeaderProject::module_text());
    REQUIRE(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));

    project.write_header("FRESHX_MARK");
    // What workspace/didChangeWatchedFiles delivers.  This, not a stat, is what
    // makes the next parse read the header again.
    analyzer.refresh_changed_extra_files({project.header_uri()});

    analyzer.change(project.module_uri(), HeaderProject::module_text("clk"));
    CHECK(has_value_named(analyzer, project.module_uri(), "FRESHX_MARK"));
    CHECK_FALSE(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));
}

TEST_CASE("open header cache: closing a header drops its pre-session text", "[sync]") {
    HeaderProject project("close");
    Analyzer analyzer;
    analyzer.open(project.module_uri(), HeaderProject::module_text());
    REQUIRE(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));

    // Edit the header through a buffer and close it.  An open buffer is excluded
    // from the cache, so nothing invalidated the disk text cached before the
    // buffer existed; close() has to drop it or the includer keeps seeing it.
    analyzer.open(project.header_uri(), "localparam int FRESHX_MARK = 1;\n");
    project.write_header("FRESHX_MARK");
    analyzer.close(project.header_uri());

    analyzer.change(project.module_uri(), HeaderProject::module_text("clk"));
    CHECK(has_value_named(analyzer, project.module_uri(), "FRESHX_MARK"));
    CHECK_FALSE(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));
}
