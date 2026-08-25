// didChange builds a fresh SourceManager per snapshot, so every keystroke used
// to re-open and re-read every header the buffer `include`s.  With one large
// shared header on a networked filesystem that read, not the parse, is what the
// user feels while typing.
//
// OpenParseHeaderCache holds that text across keystrokes and validates it by
// size and modification time.  These tests pin both halves of the deal: the read
// is genuinely skipped when the file has not moved, and a header edited on disk
// is still picked up.
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

    // Rewrite the header in place and put its modification time back.  Byte
    // count and mtime both match what was cached, so a server that reuses the
    // cached text cannot see this edit — which is precisely the read it skipped.
    const auto stamp = std::filesystem::last_write_time(project.header_path());
    project.write_header("FRESHX_MARK");
    std::filesystem::last_write_time(project.header_path(), stamp);

    analyzer.change(project.module_uri(), HeaderProject::module_text("clk"));
    CHECK(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));
    CHECK_FALSE(has_value_named(analyzer, project.module_uri(), "FRESHX_MARK"));
}

TEST_CASE("open header cache: a header edited on disk is still picked up", "[sync]") {
    HeaderProject project("refresh");
    Analyzer analyzer;
    analyzer.open(project.module_uri(), HeaderProject::module_text());
    REQUIRE(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));

    project.write_header("FRESHX_MARK");
    // Advance the stamp explicitly: a coarse filesystem clock can report the
    // same mtime for two writes in the same test, which would make this pass or
    // fail for reasons that have nothing to do with the cache.
    std::filesystem::last_write_time(project.header_path(),
                                     std::filesystem::last_write_time(project.header_path()) +
                                         std::chrono::seconds(2));

    analyzer.change(project.module_uri(), HeaderProject::module_text("clk"));
    CHECK(has_value_named(analyzer, project.module_uri(), "FRESHX_MARK"));
    CHECK_FALSE(has_value_named(analyzer, project.module_uri(), "CACHED_MARK"));
}
