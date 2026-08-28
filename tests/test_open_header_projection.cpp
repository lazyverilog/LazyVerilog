// An open buffer that `include`s a large header used to re-parse the whole
// header on every keystroke: a 214-line module including a 14k-line header
// measured 38.7 ms per keystroke on one core, against 0.4 ms for the same module
// without the include, and SyntaxTree::fromText() was 29 ms of a 29 ms parse.
//
// The background indexer already solved this for project files: once a header
// parses on its own, its shard is where its declarations live, and the rest of
// the burst is served the header's preprocessor directives alone.  The edit path
// now does the same for headers past kDirectivesOnlySeedBytes.
//
// These tests pin the deal from both ends.  The keystroke stops carrying the
// header's declarations into the buffer's own tree, features still answer for
// those declarations out of the header's shard, and a header small enough for
// its bulk not to matter is left exactly as it was.
#include "analyzer.hpp"
#include "dynamic_file_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

/// A project whose header is `include`d by an open buffer and by one closed
/// file.  The closed file matters: nothing indexes a header an open buffer alone
/// pulls in, so without it the header never earns the shard that makes serving
/// its directives alone safe.
class ProjectedHeaderProject {
  public:
    /// A header that parses on its own, versus one that is a textual fragment —
    /// here a module opened in the header and closed by whoever includes it.
    /// Only the first kind can be served as directives alone.
    enum class Shape { StandsAlone, Fragment };

    /// @p filler_lines sets the header's size, which is the other thing that
    /// decides whether the edit path projects it.
    ProjectedHeaderProject(const std::string& tag, int filler_lines,
                           Shape shape = Shape::StandsAlone)
        : shape_(shape) {
        dir_ = std::filesystem::temp_directory_path() / ("lazyverilog-header-projection-" + tag);
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "inc");
        std::filesystem::create_directories(dir_ / "rtl");

        std::string header = shape_ == Shape::StandsAlone
                                 ? "package hdr_pkg;\n    localparam int BIG_MARK = 7;\n"
                                 : "module opened (input logic clk_i);\n"
                                   "    localparam int BIG_MARK = 7;\n";
        for (int i = 0; i < filler_lines; ++i)
            header += "    localparam int FILLER_" + std::to_string(i) + " = " +
                      std::to_string(i) + ";\n";
        header += shape_ == Shape::StandsAlone ? "endpackage\ntypedef logic [7:0] byte_t;\n" : "";
        header += "`define HEADER_MACRO(x) ((x) + 1)\n";
        write(header_path(), header);

        write(opened_path(), opened_text());
        write(dir_ / "rtl" / "sibling.sv", shape_ == Shape::StandsAlone
                                               ? "`include \"marker.svh\"\n"
                                                 "module sibling;\n"
                                                 "    localparam int USES = hdr_pkg::BIG_MARK;\n"
                                                 "endmodule\n"
                                               : "`include \"marker.svh\"\n"
                                                 "endmodule\n");
    }

    ~ProjectedHeaderProject() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    ProjectedHeaderProject(const ProjectedHeaderProject&) = delete;
    ProjectedHeaderProject& operator=(const ProjectedHeaderProject&) = delete;

    /// An analyzer that has finished indexing this project, so the header's own
    /// shard exists and the buffer is open.
    ///
    /// @p lone_includer drops the sibling from the filelist and opens the buffer
    /// *before* indexing, which is the shape where nothing but an open buffer
    /// ever mentions the header.
    void start(Analyzer& analyzer, bool lone_includer = false) const {
        analyzer.set_project_index_publish_debounce_ms(0);
        analyzer.set_include_dirs({(dir_ / "inc").string()});
        if (lone_includer) {
            analyzer.open(opened_uri(), opened_text());
            analyzer.set_extra_files({opened_path().string()});
            analyzer.wait_for_background_index_idle();
            return;
        }
        analyzer.set_extra_files({opened_path().string(), (dir_ / "rtl" / "sibling.sv").string()});
        analyzer.wait_for_background_index_idle();
        analyzer.open(opened_uri(), opened_text());
    }

    std::filesystem::path header_path() const { return dir_ / "inc" / "marker.svh"; }
    std::filesystem::path opened_path() const { return dir_ / "rtl" / "opened.sv"; }
    std::string opened_uri() const { return "file://" + opened_path().string(); }
    std::string header_uri() const { return "file://" + header_path().string(); }

    /// Two spellings, so a change() is a real edit rather than a no-op.  The
    /// reference to BIG_MARK is on line 2 of the stand-alone shape.
    ///
    /// The fragment shape has no module header of its own: the header opens the
    /// module, so the buffer only closes it.
    std::string opened_text(const std::string& port = "clk_i") const {
        if (shape_ == Shape::Fragment) {
            return "`include \"marker.svh\"\n"
                   "    logic " + port + "_seen;\n"
                   "    localparam int MACRO_USES = `HEADER_MACRO(1);\n"
                   "endmodule\n";
        }
        return "`include \"marker.svh\"\n"
               "module opened (input logic " + port + ");\n"
               "    localparam int USES = hdr_pkg::BIG_MARK;\n"
               "    localparam int MACRO_USES = `HEADER_MACRO(1);\n"
               "    byte_t sig;\n"
               "endmodule\n";
    }

  private:
    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    Shape shape_;
    std::filesystem::path dir_;
};

/// Whether the buffer's own tree still carries the header's package.
bool buffer_declares_header_package(const Analyzer& analyzer, const std::string& uri) {
    auto state = analyzer.get_state(uri);
    if (!state)
        return false;
    return get_structural_index(*state).package_names.contains("hdr_pkg");
}

/// Whether the buffer's own tree still carries a value the header declares.
bool buffer_declares_value(const Analyzer& analyzer, const std::string& uri,
                           const std::string& name) {
    auto state = analyzer.get_state(uri);
    if (!state)
        return false;
    const auto& index = get_structural_index(*state);
    return std::any_of(index.values.begin(), index.values.end(),
                       [&](const ValueEntry& value) { return value.name == name; });
}

std::vector<ParseDiagInfo> errors_in(const Analyzer& analyzer, const std::string& uri) {
    std::vector<ParseDiagInfo> errors;
    auto state = analyzer.get_state(uri);
    if (!state)
        return errors;
    std::copy_if(state->parse_diagnostics.begin(), state->parse_diagnostics.end(),
                 std::back_inserter(errors),
                 [](const ParseDiagInfo& diag) { return diag.severity == 1; });
    return errors;
}

} // namespace

TEST_CASE("header projection: a keystroke stops re-parsing a large header's declarations",
          "[sync][index]") {
    // 4000 filler localparams is comfortably past kDirectivesOnlySeedBytes.
    ProjectedHeaderProject project("large", 4000);
    Analyzer analyzer;
    project.start(analyzer);
    // The first parse of the buffer reads the header whole: there is no previous
    // dependency list to seed from, which is also what fills the cache.
    REQUIRE(buffer_declares_header_package(analyzer, project.opened_uri()));

    analyzer.change(project.opened_uri(), project.opened_text("clk"));
    CHECK_FALSE(buffer_declares_header_package(analyzer, project.opened_uri()));
}

TEST_CASE("header projection: a projected header's macros still expand", "[sync][index]") {
    ProjectedHeaderProject project("macros", 4000);
    Analyzer analyzer;
    project.start(analyzer);

    analyzer.change(project.opened_uri(), project.opened_text("clk"));
    auto state = analyzer.get_state(project.opened_uri());
    REQUIRE(state);
    // A directive is what the projection keeps, so an undefined-macro error here
    // would mean the projection dropped the half an includer genuinely needs.
    CHECK(std::none_of(state->parse_diagnostics.begin(), state->parse_diagnostics.end(),
                       [](const ParseDiagInfo& diag) { return diag.severity == 1; }));
}

TEST_CASE("header projection: a header symbol still resolves from the buffer", "[definition][index]") {
    ProjectedHeaderProject project("definition", 4000);
    Analyzer analyzer;
    project.start(analyzer);
    analyzer.change(project.opened_uri(), project.opened_text("clk"));

    // Line 2, on BIG_MARK's occurrence in `localparam int USES = BIG_MARK;`.
    // The buffer's own tree no longer holds that declaration, so this is the
    // header shard answering.
    REQUIRE_FALSE(buffer_declares_header_package(analyzer, project.opened_uri()));

    auto loc = analyzer.definition_of(project.opened_uri(), 2, 36);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == project.header_uri());
    CHECK(loc->line == 1); // 0-based: BIG_MARK's line inside the header
}

TEST_CASE("header projection: a bare header typedef still resolves from the buffer",
          "[definition][index]") {
    // The package-qualified case above goes through the package's symbol list.
    // A typedef written at the header's file scope is the other shape a header
    // symbol comes in, and nothing in the buffer's own tree names it any more.
    ProjectedHeaderProject project("typedef", 4000);
    Analyzer analyzer;
    project.start(analyzer);
    analyzer.change(project.opened_uri(), project.opened_text("clk"));

    REQUIRE_FALSE(buffer_declares_header_package(analyzer, project.opened_uri()));

    // Line 4, on byte_t in `    byte_t sig;`.
    auto loc = analyzer.definition_of(project.opened_uri(), 4, 5);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == project.header_uri());
    // 0-based: package line, BIG_MARK, 4000 fillers, endpackage, then the typedef.
    CHECK(loc->line == 4003);
}

TEST_CASE("header projection: an open buffer's own header earns a shard", "[sync][index]") {
    // Nothing but this buffer includes the header, and the buffer was already
    // open when indexing ran.  The background indexer skips an open buffer's own
    // shard because its text is unsaved -- but a header's shard is built from the
    // header, so the claim still has to happen or this header would never get
    // one, leaving both a hole in the project index and the buffer parsing the
    // whole header on every keystroke.
    ProjectedHeaderProject project("lone-includer", 4000);
    Analyzer analyzer;
    project.start(analyzer, /*lone_includer=*/true);

    analyzer.change(project.opened_uri(), project.opened_text("clk"));
    CHECK_FALSE(buffer_declares_header_package(analyzer, project.opened_uri()));

    analyzer.wait_for_background_index_idle();
    auto snapshot = analyzer.project_index_snapshot();
    REQUIRE(snapshot);
    CHECK(std::any_of(snapshot->shards.begin(), snapshot->shards.end(),
                      [&](const ProjectIndexSnapshot::Shard& shard) {
                          return shard.uri == project.header_uri();
                      }));

    auto loc = analyzer.definition_of(project.opened_uri(), 2, 36);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == project.header_uri());
}

TEST_CASE("header projection: an incomplete header is still spliced whole", "[sync][index]") {
    // A header that opens a module the includer closes cannot be parsed on its
    // own, so it never enters standalone_header_uris_ however large it is, and
    // the buffer keeps seeing all of it.  Without that the buffer would be left
    // holding a bare `endmodule`.
    ProjectedHeaderProject project("fragment", 4000, ProjectedHeaderProject::Shape::Fragment);
    Analyzer analyzer;
    project.start(analyzer);
    REQUIRE(buffer_declares_value(analyzer, project.opened_uri(), "BIG_MARK"));

    analyzer.change(project.opened_uri(), project.opened_text("clk"));
    CHECK(buffer_declares_value(analyzer, project.opened_uri(), "BIG_MARK"));
    CHECK(errors_in(analyzer, project.opened_uri()).empty());
}

TEST_CASE("header projection: the buffer's own syntax error is still reported", "[sync][lint]") {
    ProjectedHeaderProject project("syntax-error", 4000);
    Analyzer analyzer;
    project.start(analyzer);

    // Same buffer, with the semicolon dropped from line 3.  The projection blanks
    // header lines rather than removing them, so a diagnostic keeps the line it
    // is written on either way.
    analyzer.change(project.opened_uri(), "`include \"marker.svh\"\n"
                                          "module opened (input logic clk);\n"
                                          "    localparam int USES = hdr_pkg::BIG_MARK\n"
                                          "    localparam int AFTER = 1;\n"
                                          "endmodule\n");
    const auto errors = errors_in(analyzer, project.opened_uri());
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().line == 2); // 0-based: the localparam line
    CHECK(errors.front().uri == project.opened_uri()); // the buffer, not the header
}

TEST_CASE("header projection: a small header is still parsed in full", "[sync][index]") {
    // Below the size threshold the exact tree is worth more than the microseconds
    // the projection would save, so nothing changes for it.
    ProjectedHeaderProject project("small", 4);
    Analyzer analyzer;
    project.start(analyzer);
    REQUIRE(buffer_declares_header_package(analyzer, project.opened_uri()));

    analyzer.change(project.opened_uri(), project.opened_text("clk"));
    CHECK(buffer_declares_header_package(analyzer, project.opened_uri()));
}
