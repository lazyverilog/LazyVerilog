#include "analyzer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

static const std::string kDefinitionFixture = R"(
module child (
    input logic clk,
    output logic done
);
endmodule

module top;
    child u_child (
        .clk(clk),
        .done(done)
    );
endmodule
)";

TEST_CASE("definition: instance resolves to module declaration", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_fixture.sv";
    analyzer.open(uri, kDefinitionFixture);

    auto loc = analyzer.definition_of(uri, 8, 11);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 1);
    CHECK(loc->col == 7);
    CHECK(loc->end_col == 12);
}

TEST_CASE("definition: named port resolves to port declaration", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_fixture.sv";
    analyzer.open(uri, kDefinitionFixture);

    auto loc = analyzer.definition_of(uri, 9, 10);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 2);
    CHECK(loc->col == 16);
    CHECK(loc->end_col == 19);
}

static std::string read_text(const std::string& path) {
    std::ifstream input(path);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static std::filesystem::path find_repo_file(const std::filesystem::path& relative) {
    auto dir = std::filesystem::current_path();
    while (true) {
        auto candidate = dir / relative;
        if (std::filesystem::exists(candidate))
            return candidate;
        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return relative;
}

static std::filesystem::path write_temp_sv(const std::string& name, const std::string& text) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    REQUIRE(out.good());
    out << text;
    out.close();
    return path;
}

static const std::string kExtraDefinitionFixture = R"(`define EXTRA_WIDTH 16
typedef logic [3:0] extra_t;
function int calc(input int lhs, input int rhs);
    return lhs + rhs;
endfunction

module child(input logic clk, output logic done);
endmodule
)";

static const std::string kTopUsingExtraFixture = R"(module top;
    logic clk, done;
    child u_child (
        .clk(clk),
        .done(done)
    );
    initial begin
        calc(.lhs(1), .rhs(2));
    end
    logic [`EXTRA_WIDTH-1:0] data;
    extra_t value;
endmodule
)";

TEST_CASE("definition: module lookup uses vcode extra files", "[definition]") {
    Analyzer analyzer;
    const auto extra_path =
        write_temp_sv("lazyverilog_definition_extra.sv", kExtraDefinitionFixture);
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 2, 11);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + extra_path.string());
    CHECK(loc->line == 6);
    CHECK(loc->col == 7);

    std::filesystem::remove(extra_path);
}

TEST_CASE("definition: named port lookup uses vcode extra files", "[definition]") {
    Analyzer analyzer;
    const auto extra_path =
        write_temp_sv("lazyverilog_definition_extra_port.sv", kExtraDefinitionFixture);
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top_port.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 3, 10);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + extra_path.string());
    CHECK(loc->line == 6);
    CHECK(loc->col == 25);

    std::filesystem::remove(extra_path);
}

TEST_CASE("definition: macro lookup resolves local define", "[definition]") {
    Analyzer analyzer;
    const auto top_path = find_repo_file("tests/definition_memory_top.sv");
    const std::string top_uri = "file://" + top_path.string();
    analyzer.open(top_uri, read_text(top_path.string()));

    auto loc = analyzer.definition_of(top_uri, 52, 24);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 1);
    CHECK(loc->col == 8);
    CHECK(loc->end_col == 13);
}

TEST_CASE("definition: typedef lookup resolves named type", "[definition]") {
    Analyzer analyzer;
    const auto top_path = find_repo_file("tests/definition_memory_top.sv");
    const std::string top_uri = "file://" + top_path.string();
    analyzer.open(top_uri, read_text(top_path.string()));

    auto loc = analyzer.definition_of(top_uri, 20, 12);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 18);
    CHECK(loc->col == 2);
    CHECK(loc->end_col == 10);
}

TEST_CASE("definition: class type lookup resolves class declaration", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_definition_class_type.sv";
    analyzer.open(uri,
                  "class packet_cfg;\n"
                  "endclass\n"
                  "\n"
                  "module top;\n"
                  "    packet_cfg cfg;\n"
                  "endmodule\n");

    auto loc = analyzer.definition_of(uri, 4, 8);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 0);
    CHECK(loc->col == 6);
    CHECK(loc->end_col == 16);
}

TEST_CASE("definition: variable lookup prefers same module scope", "[definition]") {
    Analyzer analyzer;
    const auto top_path = find_repo_file("tests/definition_memory_top.sv");
    const std::string top_uri = "file://" + top_path.string();
    analyzer.open(top_uri, read_text(top_path.string()));

    auto loc = analyzer.definition_of(top_uri, 84, 11);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 54);
    CHECK(loc->col == 40);
}

TEST_CASE("definition: named subroutine argument resolves to formal argument", "[definition]") {
    Analyzer analyzer;
    const auto top_path = find_repo_file("tests/definition_memory_top.sv");
    const std::string top_uri = "file://" + top_path.string();
    analyzer.open(top_uri, read_text(top_path.string()));

    auto loc = analyzer.definition_of(top_uri, 182, 17);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 24);
    CHECK(loc->col == 27);
    CHECK(loc->end_col == 28);
}

TEST_CASE("definition: macro lookup uses open extra file AST", "[definition]") {
    Analyzer analyzer;
    const auto extra_path =
        write_temp_sv("lazyverilog_definition_extra_macro.sv", kExtraDefinitionFixture);
    const std::string extra_uri = "file://" + extra_path.string();
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top_macro.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(extra_uri, kExtraDefinitionFixture);
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 9, 13);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + extra_path.string());
    CHECK(loc->line == 0);
    CHECK(loc->col == 8);
    CHECK(loc->end_col == 19);

    std::filesystem::remove(extra_path);
}

TEST_CASE("definition: slang built-in macro has no user-facing definition", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_builtin_macro_definition.sv";
    analyzer.open(uri,
                  "module top;\n"
                  "    int value = `SV_COV_ERROR;\n"
                  "endmodule\n");

    CHECK_FALSE(analyzer.definition_of(uri, 1, 18).has_value());
}

TEST_CASE("definition: named subroutine argument lookup uses open extra file AST", "[definition]") {
    Analyzer analyzer;
    const auto extra_path =
        write_temp_sv("lazyverilog_definition_extra_arg.sv", kExtraDefinitionFixture);
    const std::string extra_uri = "file://" + extra_path.string();
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top_arg.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(extra_uri, kExtraDefinitionFixture);
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 7, 15);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + extra_path.string());
    CHECK(loc->line == 2);
    CHECK(loc->col == 28);

    std::filesystem::remove(extra_path);
}

TEST_CASE("definition: generic lookup uses open extra file AST", "[definition]") {
    Analyzer analyzer;
    const auto extra_path =
        write_temp_sv("lazyverilog_definition_extra_generic.sv", kExtraDefinitionFixture);
    const std::string extra_uri = "file://" + extra_path.string();
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top_generic.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(extra_uri, kExtraDefinitionFixture);
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 10, 6);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + extra_path.string());
    CHECK(loc->line == 1);
    CHECK(loc->col == 20);

    std::filesystem::remove(extra_path);
}

TEST_CASE("definition: nested include cursor matching is file-aware", "[definition]") {
    Analyzer analyzer;
    const auto b_path = write_temp_sv("B_nested_collision.svh",
                                      "// B line 0\n"
                                      "// B line 1\n"
                                      "parameter int B_PARAMETER = 2;\n");
    const auto a_path = write_temp_sv("A_nested_collision.svh",
                                      "`include \"B_nested_collision.svh\"\n"
                                      "parameter int A_PARAMETER = 1;\n");
    const std::string top_uri = "file:///tmp/top_nested_collision.sv";
    analyzer.open(top_uri, "`include \"A_nested_collision.svh\"\n"
                           "module top;\n"
                           "  localparam int X = A_PARAMETER;\n"
                           "endmodule\n");

    auto ident = analyzer.identifier_at(top_uri, 2, 22);
    REQUIRE(ident.has_value());
    CHECK(ident->name == "A_PARAMETER");

    auto loc = analyzer.definition_of(top_uri, 2, 22);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + a_path.string());
    CHECK(loc->line == 1);
    CHECK(loc->col == 14);

    std::filesystem::remove(a_path);
    std::filesystem::remove(b_path);
}

TEST_CASE("extra file cache refreshes on explicit filelist reset and drops removed files",
          "[definition]") {
    Analyzer analyzer;
    const auto extra_path = write_temp_sv("lazyverilog_definition_cache.sv",
                                          "module child(input logic clk, output logic done);\n"
                                          "endmodule\n");
    const std::string top_uri = "file:///tmp/lazyverilog_definition_cache_top.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    auto snapshots = analyzer.extra_file_snapshot_ptr();
    REQUIRE(snapshots != nullptr);
    REQUIRE(snapshots->size() == 1);
    CHECK((*snapshots)[0].state == nullptr);
    analyzer.open(top_uri, "module top;\n"
                           "    logic clk, done;\n"
                           "    child u_child(.clk(clk), .done(done));\n"
                           "endmodule\n");

    auto original = analyzer.definition_of(top_uri, 2, 31);
    REQUIRE(original.has_value());
    CHECK(original->line == 0);

    {
        std::ofstream out(extra_path);
        out << "module child(input logic clk, output logic ack);\nendmodule\n";
    }
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    CHECK_FALSE(analyzer.definition_of(top_uri, 2, 31).has_value());

    std::filesystem::remove(extra_path);
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    snapshots = analyzer.extra_file_snapshot_ptr();
    REQUIRE(snapshots != nullptr);
    CHECK(snapshots->empty());
}
