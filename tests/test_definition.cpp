#include "analyzer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string_view>

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

static std::pair<int, int> position_of(std::string_view text, std::string_view needle) {
    const auto offset = text.find(needle);
    REQUIRE(offset != std::string_view::npos);
    int line = 0;
    int col = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}

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

TEST_CASE("definition: current-file lookup accepts Windows-style file URI", "[definition][path][uri]") {
    Analyzer analyzer;
    const std::string uri = "file:///C:/lazyverilog/definition_windows_uri.sv";
    analyzer.open(uri,
                  "module top;\n"
                  "    logic ready;\n"
                  "    assign ready = 1'b1;\n"
                  "endmodule\n");

    auto loc = analyzer.definition_of(uri, 2, 11);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 1);
    CHECK(loc->col == 10);
    CHECK(loc->end_col == 15);
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

TEST_CASE("definition: package parameter lookup resolves qualified and imported names",
          "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_definition_package_param.sv";
    const std::string text = R"SV(package cpu_pkg;
    parameter int WIDTH = 8;
    localparam int DEPTH = WIDTH * 2;
endpackage

module top;
    import cpu_pkg::DEPTH;
    logic [cpu_pkg::WIDTH-1:0] data;
    logic [DEPTH-1:0] addr;
endmodule
)SV";
    analyzer.open(uri, text);

    auto [qualified_line, qualified_col] = position_of(text, "WIDTH-1");
    auto qualified = analyzer.definition_of(uri, qualified_line, qualified_col);
    REQUIRE(qualified.has_value());
    auto [width_line, width_col] = position_of(text, "WIDTH =");
    CHECK(qualified->uri == uri);
    CHECK(qualified->line == width_line);
    CHECK(qualified->col == width_col);

    auto [imported_line, imported_col] = position_of(text, "DEPTH-1");
    auto imported = analyzer.definition_of(uri, imported_line, imported_col);
    REQUIRE(imported.has_value());
    auto [depth_line, depth_col] = position_of(text, "DEPTH =");
    CHECK(imported->uri == uri);
    CHECK(imported->line == depth_line);
    CHECK(imported->col == depth_col);
}

TEST_CASE("definition: package parameter lookup uses closed project index", "[definition]") {
    Analyzer analyzer;
    const std::string package_text = R"SV(package bus_pkg;
    parameter int BUS_WIDTH = 64;
    localparam int BUS_DEPTH = 128;
endpackage
)SV";
    const auto package_path =
        write_temp_sv("lazyverilog_definition_package_param_pkg.sv", package_text);
    const std::string package_uri = "file://" + package_path.string();
    analyzer.set_extra_files({package_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string top_uri = "file:///tmp/lazyverilog_definition_package_param_top.sv";
    const std::string top_text = R"SV(module top;
    import bus_pkg::*;
    logic [bus_pkg::BUS_WIDTH-1:0] data;
    logic [BUS_DEPTH-1:0] addr;
endmodule
)SV";
    analyzer.open(top_uri, top_text);

    auto [qualified_line, qualified_col] = position_of(top_text, "BUS_WIDTH-1");
    auto qualified = analyzer.definition_of(top_uri, qualified_line, qualified_col);
    REQUIRE(qualified.has_value());
    auto [width_line, width_col] = position_of(package_text, "BUS_WIDTH =");
    CHECK(qualified->uri == package_uri);
    CHECK(qualified->line == width_line);
    CHECK(qualified->col == width_col);

    auto [imported_line, imported_col] = position_of(top_text, "BUS_DEPTH-1");
    auto imported = analyzer.definition_of(top_uri, imported_line, imported_col);
    REQUIRE(imported.has_value());
    auto [depth_line, depth_col] = position_of(package_text, "BUS_DEPTH =");
    CHECK(imported->uri == package_uri);
    CHECK(imported->line == depth_line);
    CHECK(imported->col == depth_col);

    std::filesystem::remove(package_path);
}

TEST_CASE("definition: package name lookup uses closed project index", "[definition]") {
    Analyzer analyzer;
    const std::string package_text = R"SV(package name_pkg;
    parameter int WIDTH = 32;
endpackage
)SV";
    const auto package_path =
        write_temp_sv("lazyverilog_definition_package_name_pkg.sv", package_text);
    const std::string package_uri = "file://" + package_path.string();
    analyzer.set_extra_files({package_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string top_uri = "file:///tmp/lazyverilog_definition_package_name_top.sv";
    const std::string top_text = R"SV(module top;
    import name_pkg::*;
    logic [name_pkg::WIDTH-1:0] data;
endmodule
)SV";
    analyzer.open(top_uri, top_text);

    auto [import_line, import_col] = position_of(top_text, "name_pkg::*");
    auto import_def = analyzer.definition_of(top_uri, import_line, import_col);
    REQUIRE(import_def.has_value());
    auto [pkg_line, pkg_col] = position_of(package_text, "name_pkg;");
    CHECK(import_def->uri == package_uri);
    CHECK(import_def->line == pkg_line);
    CHECK(import_def->col == pkg_col);

    auto [scoped_line, scoped_col] = position_of(top_text, "name_pkg::WIDTH");
    auto scoped_def = analyzer.definition_of(top_uri, scoped_line, scoped_col);
    REQUIRE(scoped_def.has_value());
    CHECK(scoped_def->uri == package_uri);
    CHECK(scoped_def->line == pkg_line);
    CHECK(scoped_def->col == pkg_col);

    std::filesystem::remove(package_path);
}

TEST_CASE("definition: package name lookup uses current AST", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_definition_package_name_current.sv";
    const std::string text = R"SV(package current_pkg;
    parameter int WIDTH = 32;
endpackage

module top;
    import current_pkg::*;
    logic [current_pkg::WIDTH-1:0] data;
endmodule
)SV";
    analyzer.open(uri, text);

    auto [import_line, import_col] = position_of(text, "current_pkg::*");
    auto import_def = analyzer.definition_of(uri, import_line, import_col);
    REQUIRE(import_def.has_value());
    auto [pkg_line, pkg_col] = position_of(text, "current_pkg;");
    CHECK(import_def->uri == uri);
    CHECK(import_def->line == pkg_line);
    CHECK(import_def->col == pkg_col);

    auto [scoped_line, scoped_col] = position_of(text, "current_pkg::WIDTH");
    auto scoped_def = analyzer.definition_of(uri, scoped_line, scoped_col);
    REQUIRE(scoped_def.has_value());
    CHECK(scoped_def->uri == uri);
    CHECK(scoped_def->line == pkg_line);
    CHECK(scoped_def->col == pkg_col);
}

TEST_CASE("definition: package type lookup uses closed project index", "[definition]") {
    Analyzer analyzer;
    const std::string package_text = R"SV(package type_pkg;
    typedef logic [7:0] byte_t;
    class packet_cfg;
    endclass
endpackage
)SV";
    const auto package_path =
        write_temp_sv("lazyverilog_definition_package_type_pkg.sv", package_text);
    const std::string package_uri = "file://" + package_path.string();
    analyzer.set_extra_files({package_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string top_uri = "file:///tmp/lazyverilog_definition_package_type_top.sv";
    const std::string top_text = R"SV(module top;
    type_pkg::byte_t data;
    type_pkg::packet_cfg cfg;
endmodule
)SV";
    analyzer.open(top_uri, top_text);

    auto [byte_use_line, byte_use_col] = position_of(top_text, "byte_t data");
    auto byte_def = analyzer.definition_of(top_uri, byte_use_line, byte_use_col);
    REQUIRE(byte_def.has_value());
    auto [byte_line, byte_col] = position_of(package_text, "byte_t;");
    CHECK(byte_def->uri == package_uri);
    CHECK(byte_def->line == byte_line);
    CHECK(byte_def->col == byte_col);

    auto [class_use_line, class_use_col] = position_of(top_text, "packet_cfg cfg");
    auto class_def = analyzer.definition_of(top_uri, class_use_line, class_use_col);
    REQUIRE(class_def.has_value());
    auto [class_line, class_col] = position_of(package_text, "packet_cfg;");
    CHECK(class_def->uri == package_uri);
    CHECK(class_def->line == class_line);
    CHECK(class_def->col == class_col);

    std::filesystem::remove(package_path);
}

TEST_CASE("definition: package type lookup uses current AST", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_definition_package_type_current.sv";
    const std::string text = R"SV(package current_type_pkg;
    typedef logic [7:0] byte_t;
endpackage

module top;
    current_type_pkg::byte_t data;
endmodule
)SV";
    analyzer.open(uri, text);

    auto [use_line, use_col] = position_of(text, "byte_t data");
    auto def = analyzer.definition_of(uri, use_line, use_col);
    REQUIRE(def.has_value());
    auto [type_line, type_col] = position_of(text, "byte_t;");
    CHECK(def->uri == uri);
    CHECK(def->line == type_line);
    CHECK(def->col == type_col);
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

TEST_CASE("definition: unqualified name ignores aggregate fields", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_definition_aggregate_field_scope.sv";
    analyzer.open(uri,
                  "module top;\n"
                  "    typedef struct packed {\n"
                  "        logic valid;\n"
                  "    } fifo_entry_t;\n"
                  "    fifo_entry_t fifo_entry;\n"
                  "    logic valid;\n"
                  "    always_comb begin\n"
                  "        valid = fifo_entry.valid;\n"
                  "    end\n"
                  "endmodule\n");

    // The left-hand `valid` is an ordinary unqualified module variable use.
    // A packed-struct field happens to have the same spelling and appears
    // earlier in the same module text, but it is only visible through member
    // access (`fifo_entry.valid`) and must not shadow the module signal.
    auto lhs = analyzer.definition_of(uri, 7, 8);
    REQUIRE(lhs.has_value());
    CHECK(lhs->uri == uri);
    CHECK(lhs->line == 5);
    CHECK(lhs->col == 10);
    CHECK(lhs->end_col == 15);

    // The right-hand member access should still resolve to the aggregate field.
    auto rhs = analyzer.definition_of(uri, 7, 27);
    REQUIRE(rhs.has_value());
    CHECK(rhs->uri == uri);
    CHECK(rhs->line == 2);
    CHECK(rhs->col == 14);
    CHECK(rhs->end_col == 19);
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

TEST_CASE("definition: package members included as text require import", "[definition]") {
    Analyzer analyzer;
    const auto header_path =
        write_temp_sv("lazyverilog_definition_package_member.svh",
                      "package cpu_pkg;\n"
                      "task add_number(input int a, input int b, output int result);\n"
                      "    result = a + b;\n"
                      "endtask\n"
                      "endpackage\n");
    const auto top_path =
        std::filesystem::temp_directory_path() / "lazyverilog_definition_package_member_top.sv";
    const std::string top_uri = "file://" + top_path.string();

    // Including a header that contains a package declaration makes the package
    // syntax visible to slang's parsed tree, but it does not import the package
    // members into the following module scope.  Go-to-definition / references
    // must therefore not treat cpu_pkg::add_number as an unqualified
    // add_number declaration in top_no_import.
    analyzer.open(top_uri, "`include \"lazyverilog_definition_package_member.svh\"\n"
                           "module top_no_import;\n"
                           "    initial begin\n"
                           "        add_number();\n"
                           "    end\n"
                           "endmodule\n");
    CHECK_FALSE(analyzer.definition_of(top_uri, 3, 11).has_value());

    // A wildcard import in the module makes the package task visible at the
    // call site, so the same unqualified name should now resolve to the header
    // declaration.
    analyzer.open(top_uri, "`include \"lazyverilog_definition_package_member.svh\"\n"
                           "module top_with_import;\n"
                           "    import cpu_pkg::*;\n"
                           "    initial begin\n"
                           "        add_number();\n"
                           "    end\n"
                           "endmodule\n");
    auto imported = analyzer.definition_of(top_uri, 4, 11);
    REQUIRE(imported.has_value());
    CHECK(imported->uri == "file://" + header_path.string());
    CHECK(imported->line == 1);
    CHECK(imported->col == 5);

    std::filesystem::remove(header_path);
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
    const auto top_path = std::filesystem::temp_directory_path() / "top_nested_collision.sv";
    const std::string top_uri = "file://" + top_path.string();
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
