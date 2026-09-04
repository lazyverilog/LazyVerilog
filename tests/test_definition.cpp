#include "analyzer.hpp"
#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

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
    CHECK(loc->uri == uri_from_path(extra_path));
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
    CHECK(loc->uri == uri_from_path(extra_path));
    CHECK(loc->line == 6);
    CHECK(loc->col == 25);

    std::filesystem::remove(extra_path);
}

TEST_CASE("definition: macro lookup resolves local define", "[definition]") {
    Analyzer analyzer;
    const auto top_path = find_repo_file("tests/definition_memory_top.sv");
    const std::string top_uri = uri_from_path(top_path);
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
    const std::string top_uri = uri_from_path(top_path);
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
    const std::string top_uri = uri_from_path(top_path);
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

TEST_CASE("definition: included aggregate field does not shadow module signal", "[definition]") {
    Analyzer analyzer;
    const auto header_path = write_temp_sv("lazyverilog_definition_valid_params.svh",
                                           "typedef struct packed {\n"
                                           "    logic valid;\n"
                                           "    logic [31:0] data;\n"
                                           "} fifo_entry_t;\n");
    const auto top_path =
        write_temp_sv("lazyverilog_definition_valid_top.sv",
                      "module memory_top;\n"
                      "`include \"lazyverilog_definition_valid_params.svh\"\n"
                      "fifo_entry_t fifo_entry;\n"
                      "logic valid;\n"
                      "always_comb begin\n"
                      "    valid = fifo_entry.valid;\n"
                      "end\n"
                      "endmodule\n");
    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(top_uri, read_text(top_path.string()));

    // The included header defines fifo_entry_t.valid before the module signal
    // declaration.  The unqualified LHS must still resolve to the module-level
    // `logic valid`, not the aggregate field.
    auto lhs = analyzer.definition_of(top_uri, 5, 4);
    REQUIRE(lhs.has_value());
    CHECK(lhs->uri == top_uri);
    CHECK(lhs->line == 3);
    CHECK(lhs->col == 6);
    CHECK(lhs->end_col == 11);

    // Keep the member-access behavior intact for the RHS.
    auto rhs = analyzer.definition_of(top_uri, 5, 23);
    REQUIRE(rhs.has_value());
    CHECK(rhs->uri == uri_from_path(header_path));
    CHECK(rhs->line == 1);
    CHECK(rhs->col == 10);
    CHECK(rhs->end_col == 15);

    std::filesystem::remove(header_path);
    std::filesystem::remove(top_path);
}

TEST_CASE("definition: included function formal does not shadow module port", "[definition]") {
    Analyzer analyzer;
    const auto header_path = write_temp_sv("lazyverilog_definition_func_arg.svh",
                                           "function automatic logic [7:0] foo(\n"
                                           "    input logic [7:0] i_data\n"
                                           ");\n"
                                           "    foo = i_data;\n"
                                           "endfunction\n");
    const auto top_path =
        write_temp_sv("lazyverilog_definition_func_arg_top.sv",
                      "module test (\n"
                      "    i_data\n"
                      ");\n"
                      "`include \"lazyverilog_definition_func_arg.svh\"\n"
                      "input logic [7:0] i_data;\n"
                      "always_comb begin\n"
                      "    foo(.i_data(i_data));\n"
                      "end\n"
                      "endmodule\n");
    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(top_uri, read_text(top_path.string()));

    // The included function declares a formal named `i_data` before the module
    // port declaration.  The connected expression must still resolve to the
    // module port, not the function formal.
    auto expr = analyzer.definition_of(top_uri, 6, 17);
    REQUIRE(expr.has_value());
    CHECK(expr->uri == top_uri);
    CHECK(expr->line == 4);
    CHECK(expr->col == 18);
    CHECK(expr->end_col == 24);

    // The named-argument label keeps resolving to the formal.
    auto label = analyzer.definition_of(top_uri, 6, 10);
    REQUIRE(label.has_value());
    CHECK(label->uri == uri_from_path(header_path));
    CHECK(label->line == 1);
    CHECK(label->col == 22);
    CHECK(label->end_col == 28);

    std::filesystem::remove(header_path);
    std::filesystem::remove(top_path);
}

TEST_CASE("definition: function formal visible only inside its body", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_definition_func_scope.sv";
    analyzer.open(uri,
                  "module m;\n"
                  "    function automatic logic [7:0] foo(input logic [7:0] arg_data);\n"
                  "        foo = arg_data;\n"
                  "    endfunction\n"
                  "    logic [7:0] arg_data;\n"
                  "    assign arg_data = foo(8'd1);\n"
                  "endmodule\n");

    // Inside the function body the formal wins.
    auto inside = analyzer.definition_of(uri, 2, 15);
    REQUIRE(inside.has_value());
    CHECK(inside->line == 1);
    CHECK(inside->col == 57);
    CHECK(inside->end_col == 65);

    // Outside the function the module-level declaration wins.
    auto outside = analyzer.definition_of(uri, 5, 12);
    REQUIRE(outside.has_value());
    CHECK(outside->line == 4);
    CHECK(outside->col == 16);
    CHECK(outside->end_col == 24);
}

TEST_CASE("definition: named subroutine argument resolves to formal argument", "[definition]") {
    Analyzer analyzer;
    const auto top_path = find_repo_file("tests/definition_memory_top.sv");
    const std::string top_uri = uri_from_path(top_path);
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
    const std::string extra_uri = uri_from_path(extra_path);
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top_macro.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(extra_uri, kExtraDefinitionFixture);
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 9, 13);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(extra_path));
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
    const std::string extra_uri = uri_from_path(extra_path);
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top_arg.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(extra_uri, kExtraDefinitionFixture);
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 7, 15);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(extra_path));
    CHECK(loc->line == 2);
    CHECK(loc->col == 28);

    std::filesystem::remove(extra_path);
}

TEST_CASE("definition: generic lookup uses open extra file AST", "[definition]") {
    Analyzer analyzer;
    const auto extra_path =
        write_temp_sv("lazyverilog_definition_extra_generic.sv", kExtraDefinitionFixture);
    const std::string extra_uri = uri_from_path(extra_path);
    const std::string top_uri = "file:///tmp/lazyverilog_definition_top_generic.sv";
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(extra_uri, kExtraDefinitionFixture);
    analyzer.open(top_uri, kTopUsingExtraFixture);

    auto loc = analyzer.definition_of(top_uri, 10, 6);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(extra_path));
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
    const std::string top_uri = uri_from_path(std::filesystem::temp_directory_path() /
                                              "lazyverilog_definition_package_member_top.sv");

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
    CHECK(imported->uri == uri_from_path(header_path));
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
    const std::string top_uri =
        uri_from_path(std::filesystem::temp_directory_path() / "top_nested_collision.sv");
    analyzer.open(top_uri, "`include \"A_nested_collision.svh\"\n"
                           "module top;\n"
                           "  localparam int X = A_PARAMETER;\n"
                           "endmodule\n");

    auto ident = analyzer.identifier_at(top_uri, 2, 22);
    REQUIRE(ident.has_value());
    CHECK(ident->name == "A_PARAMETER");

    auto loc = analyzer.definition_of(top_uri, 2, 22);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(a_path));
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

// ── Package-qualified resolution (D1, F1, F3) ────────────────────────────────

// Closed extra file: never opened, so every lookup goes through SyntaxIndex.
static const std::string kCpuPkg = R"(package cpu_pkg;
    parameter  int      WIDTH = 8;
    localparam int      DEPTH = WIDTH * 2;
    typedef logic [7:0] byte_t;
    class packet_cfg;
    endclass
endpackage
)";

static const std::string kTopUsingCpuPkg = R"(module top;
localparam DEPTH = 1;
import cpu_pkg::DEPTH;
logic [cpu_pkg::WIDTH-1:0] data;
logic [DEPTH-1:0] addr;
cpu_pkg::byte_t b;
cpu_pkg::packet_cfg cfg;
endmodule
)";

TEST_CASE("definition: qualified package parameter resolves to closed extra file",
          "[definition]") {
    auto pkg_path = write_temp_sv("lazyverilog_def_cpu_pkg_closed.sv", kCpuPkg);

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_def_top_closed.sv";
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, kTopUsingCpuPkg);

    // F1 -- cursor on WIDTH in `cpu_pkg::WIDTH`
    auto loc = analyzer.definition_of(top_uri, 3, 16);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(pkg_path));
    CHECK(loc->line == 1);

    // F3 -- cursor on byte_t in `cpu_pkg::byte_t`
    auto type_loc = analyzer.definition_of(top_uri, 5, 12);
    REQUIRE(type_loc.has_value());
    CHECK(type_loc->uri == uri_from_path(pkg_path));
    CHECK(type_loc->line == 3);

    // A qualified class must still resolve now that the generic bare-name
    // fallback no longer runs for qualified names.
    auto class_loc = analyzer.definition_of(top_uri, 6, 12);
    REQUIRE(class_loc.has_value());
    CHECK(class_loc->uri == uri_from_path(pkg_path));
    CHECK(class_loc->line == 4);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: qualified package member does not resolve to a local declaration",
          "[definition]") {
    auto pkg_path = write_temp_sv("lazyverilog_def_cpu_pkg_shadow.sv", kCpuPkg);

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_def_top_shadow.sv";
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, kTopUsingCpuPkg);

    // D1, qualified half: `import cpu_pkg::DEPTH;` names the package's DEPTH,
    // even though module top declares its own `localparam DEPTH = 1` first.
    auto qualified = analyzer.definition_of(top_uri, 2, 17);
    REQUIRE(qualified.has_value());
    CHECK(qualified->uri == uri_from_path(pkg_path));
    CHECK(qualified->line == 2);

    // D1, unqualified half: a bare DEPTH use still resolves to the local
    // declaration in the current file.
    auto unqualified = analyzer.definition_of(top_uri, 4, 7);
    REQUIRE(unqualified.has_value());
    CHECK(unqualified->uri == top_uri);
    CHECK(unqualified->line == 1);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: qualified package parameter resolves when package is an open buffer",
          "[definition]") {
    // "Open buffer" in this codebase means a project file that is *also* open in
    // the editor, i.e. ExtraFileInfo::state is non-null (analyzer.hpp:48-51).
    // That exercises the live-index branch rather than the closed-shard branch.
    auto pkg_path = write_temp_sv("lazyverilog_def_cpu_pkg_open.sv", kCpuPkg);
    const std::string pkg_uri = uri_from_path(pkg_path);

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_def_top_open.sv";
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(pkg_uri, kCpuPkg);
    analyzer.open(top_uri, kTopUsingCpuPkg);

    auto loc = analyzer.definition_of(top_uri, 3, 16);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == pkg_uri);
    CHECK(loc->line == 1);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: qualified package parameter resolves within a single file",
          "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_def_same_file_pkg.sv";
    analyzer.open(uri, kCpuPkg + "\n" + kTopUsingCpuPkg);

    // `cpu_pkg::WIDTH` on the `logic [cpu_pkg::WIDTH-1:0]` line.
    auto loc = analyzer.definition_of(uri, 11, 16);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 1);
}

TEST_CASE("definition: unqualified name does not leak into another module's parameters",
          "[definition]") {
    // D1's original repro: module top has no DEPTH and no import, so DEPTH is
    // unresolvable.  It must not silently resolve into an unrelated module's
    // header parameter in a closed project file.
    auto other_path = write_temp_sv("lazyverilog_def_other_module.sv",
                                    "module other_mod #(\n"
                                    "    parameter int DEPTH = 16\n"
                                    ")();\n"
                                    "endmodule\n");

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_def_leak_top.sv";
    analyzer.set_extra_files({other_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri,
                  "module top;\n"
                  "logic [DEPTH-1:0] addr;\n"
                  "endmodule\n");

    auto loc = analyzer.definition_of(top_uri, 1, 7);
    CHECK_FALSE(loc.has_value());

    std::filesystem::remove(other_path);
}

// ── Performance guard ────────────────────────────────────────────────────────

// Qualified-name resolution must stay keyed.  The obvious implementation --
// scanning every ValueEntry across the current index and every project shard --
// is invisible in functional tests and only shows up at project scale, so this
// guards the shape of the lookup rather than any single feature.
//
// The probe is an *unresolvable* identifier because that is the worst case: it
// exhausts every resolution path before returning nothing.
TEST_CASE("definition: unresolvable identifier stays fast on a large project",
          "[definition][performance]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_perf_pkgs";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    constexpr int kPackages = 300;
    constexpr int kParamsPerPackage = 40;

    std::vector<std::string> paths;
    paths.reserve(kPackages);
    for (int p = 0; p < kPackages; ++p) {
        std::string text = "package perf_pkg_" + std::to_string(p) + ";\n";
        for (int v = 0; v < kParamsPerPackage; ++v) {
            text += "    parameter int P_" + std::to_string(p) + "_" + std::to_string(v) + " = " +
                    std::to_string(v) + ";\n";
        }
        text += "endpackage\n";

        const auto path = dir / ("perf_pkg_" + std::to_string(p) + ".sv");
        std::ofstream out(path);
        REQUIRE(out.good());
        out << text;
        out.close();
        paths.push_back(path.string());
    }

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_perf_top.sv";
    analyzer.set_extra_files(paths);
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri,
                  "module perf_top;\n"
                  "logic [NOT_DECLARED_ANYWHERE-1:0] addr;\n"
                  "endmodule\n");

    // Warm up caches so the measurement reflects steady-state request cost.
    for (int i = 0; i < 20; ++i)
        (void)analyzer.definition_of(top_uri, 1, 7);

    constexpr int kIterations = 200;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto loc = analyzer.definition_of(top_uri, 1, 7);
        CHECK_FALSE(loc.has_value());
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double mean_us =
        std::chrono::duration<double, std::micro>(elapsed).count() / kIterations;

    INFO("mean go-to-definition on unresolvable identifier: " << mean_us << " us");
    // Deliberately loose.  A linear scan over 12,000 values measured ~172 us
    // against a ~62 us keyed baseline; this catches that regression without
    // being a flake generator on a loaded CI machine.  x86_64 macOS CI runs
    // under Rosetta translation on Apple Silicon runners and measured 124.7 us
    // in steady state, so the margin needs to clear that noise too.
    CHECK(mean_us < 160.0);

    std::filesystem::remove_all(dir);
}

TEST_CASE("definition: class member access walks the extends chain across files",
          "[definition]") {
    Analyzer analyzer;
    const auto base_path = write_temp_sv("lazyverilog_definition_pkt_base.sv",
                                         "class pkt_base;\n"
                                         "    int depth;\n"
                                         "\n"
                                         "    function void apply();\n"
                                         "    endfunction\n"
                                         "endclass\n");
    const std::string top_uri = "file:///tmp/lazyverilog_definition_pkt_child.sv";
    analyzer.set_extra_files({base_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, "class pkt_child extends pkt_base;\n"
                           "endclass\n"
                           "\n"
                           "module top;\n"
                           "    pkt_child child;\n"
                           "    initial begin\n"
                           "        child.depth = 1;\n"
                           "        child.apply();\n"
                           "    end\n"
                           "endmodule\n");

    auto field = analyzer.definition_of(top_uri, 6, 15);
    REQUIRE(field.has_value());
    CHECK(field->uri == uri_from_path(base_path));
    CHECK(field->line == 1);
    CHECK(field->col == 8);

    auto method = analyzer.definition_of(top_uri, 7, 15);
    REQUIRE(method.has_value());
    CHECK(method->uri == uri_from_path(base_path));
    CHECK(method->line == 3);

    std::filesystem::remove(base_path);
}

TEST_CASE("definition: unqualified inherited member inside a class body resolves to the base",
          "[definition]") {
    Analyzer analyzer;
    const auto base_path = write_temp_sv("lazyverilog_definition_inherited_base.sv",
                                         "class pkt_base;\n"
                                         "    int depth;\n"
                                         "endclass\n");
    const std::string child_uri = "file:///tmp/lazyverilog_definition_inherited_child.sv";
    analyzer.set_extra_files({base_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(child_uri, "class pkt_child extends pkt_base;\n"
                             "    function void bump();\n"
                             "        depth = depth + 1;\n"
                             "    endfunction\n"
                             "endclass\n");

    auto loc = analyzer.definition_of(child_uri, 2, 8);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(base_path));
    CHECK(loc->line == 1);
    CHECK(loc->col == 8);

    // A method local of the same name still shadows the inherited member.
    analyzer.open(child_uri, "class pkt_child extends pkt_base;\n"
                             "    function void bump();\n"
                             "        int depth;\n"
                             "        depth = 1;\n"
                             "    endfunction\n"
                             "endclass\n");
    auto shadowed = analyzer.definition_of(child_uri, 3, 8);
    REQUIRE(shadowed.has_value());
    CHECK(shadowed->uri == child_uri);
    CHECK(shadowed->line == 2);

    std::filesystem::remove(base_path);
}

TEST_CASE("definition: imported package subroutine resolves while the package file is open",
          "[definition]") {
    Analyzer analyzer;
    const auto pkg_path = write_temp_sv("lazyverilog_definition_open_pkg.sv",
                                        "package common_pkg;\n"
                                        "typedef int my_int_t;\n"
                                        "\n"
                                        "function void foo();\n"
                                        "endfunction\n"
                                        "endpackage\n");
    const std::string pkg_uri = uri_from_path(pkg_path);
    const std::string top_uri = "file:///tmp/lazyverilog_definition_open_pkg_top.sv";
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    // The package buffer being open routes the lookup through the dynamic
    // open-file shard instead of the disk-backed one; both must report the
    // declaration position.
    analyzer.open(pkg_uri, "package common_pkg;\n"
                           "typedef int my_int_t;\n"
                           "\n"
                           "function void foo();\n"
                           "endfunction\n"
                           "endpackage\n");
    analyzer.open(top_uri, "module top;\n"
                           "import common_pkg::*;\n"
                           "    initial begin\n"
                           "        foo();\n"
                           "    end\n"
                           "endmodule\n");

    auto loc = analyzer.definition_of(top_uri, 3, 9);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == pkg_uri);
    CHECK(loc->line == 3);
    CHECK(loc->col == 14);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("identifiers inside macro arguments resolve instead of the macro", "[definition][macro]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_macro_arg.sv";
    analyzer.open(uri, "`define INFO(ID, MSG) $display(ID, MSG)\n"
                       "class item_t;\n"
                       "    bit [7:0] addr;\n"
                       "endclass\n"
                       "class drv;\n"
                       "    task run();\n"
                       "        item_t item;\n"
                       "        `INFO(\"DRV\", $sformatf(\"a=%0h\", item.addr))\n"
                       "    endtask\n"
                       "endclass\n");

    // Macro arguments are text the user typed at the call site, so a name there
    // resolves like any other; only the rest of the invocation is the macro.
    const std::string call = "        `INFO(\"DRV\", $sformatf(\"a=%0h\", item.addr))";

    auto object = analyzer.definition_of(uri, 7, (int)call.find("item.addr"));
    REQUIRE(object.has_value());
    CHECK(object->uri == uri);
    CHECK(object->line == 6);
    CHECK(object->col == 15);

    auto member = analyzer.definition_of(uri, 7, (int)call.find("item.addr") + 5);
    REQUIRE(member.has_value());
    CHECK(member->uri == uri);
    CHECK(member->line == 2);
    CHECK(member->col == 14);

    // The macro name itself still goes to the `define.
    auto macro = analyzer.definition_of(uri, 7, (int)call.find("`INFO") + 1);
    REQUIRE(macro.has_value());
    CHECK(macro->line == 0);
    CHECK(macro->col == 8);
}

TEST_CASE("definition: a macro argument expanded into several contexts resolves to its "
          "own declaration",
          "[definition][macro]") {
    // `uvm_field_object(cfg, ...) expands ARG into many positions, including as
    // the suffix of a member access (`uvm_copy_object(ARG, local_rhs__.ARG,...)).
    // Every one of those expanded tokens maps back to the same original range,
    // so all of them "contain" the cursor and the resolver must pick the one
    // used in its own right rather than whichever it happens to reach first.
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_macro_arg_multi.sv";
    analyzer.open(uri, "`define COPY_FIELD(ARG) \\\n"
                       "    function void do_copy(peer_t rhs); \\\n"
                       "        ARG = rhs.ARG; \\\n"
                       "    endfunction\n"
                       "class peer_t;\n"
                       "    int unrelated_field;\n"
                       "endclass\n"
                       "class holder;\n"
                       "    int cfg;\n"
                       "    `COPY_FIELD(cfg)\n"
                       "endclass\n");

    const std::string call = "    `COPY_FIELD(cfg)";
    auto arg = analyzer.definition_of(uri, 9, (int)call.find("(cfg") + 1);
    REQUIRE(arg.has_value());
    CHECK(arg->uri == uri);
    CHECK(arg->line == 8); // holder's own `int cfg;`, not peer_t's
}

// ── Member access whose receiver type lives in another file ──────────────────

static const std::string kPackageClassDefinitionFixture = R"(package tb_pkg;
    class phase_obj;
        extern virtual function void raise_objection();
    endclass

    class driver_base;
        phase_obj port_handle;
    endclass
endpackage
)";

TEST_CASE("definition: method called through a package-class argument resolves",
          "[definition][class]") {
    Analyzer analyzer;
    const auto pkg_path = write_temp_sv("lazyverilog_definition_pkg_class_arg.sv",
                                        kPackageClassDefinitionFixture);
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_definition_pkg_arg.sv";
    analyzer.open(uri, "import tb_pkg::*;\n"
                       "class my_drv;\n"
                       "    task run(phase_obj arg_phase);\n"
                       "        arg_phase.raise_objection();\n"
                       "    endtask\n"
                       "endclass\n");

    // Line 3, on `raise_objection`.
    auto loc = analyzer.definition_of(uri, 3, 20);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(pkg_path));
    CHECK(loc->line == 2);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: method called through an inherited handle resolves",
          "[definition][class]") {
    Analyzer analyzer;
    const auto pkg_path = write_temp_sv("lazyverilog_definition_pkg_class_field.sv",
                                        kPackageClassDefinitionFixture);
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_definition_pkg_field.sv";
    // `port_handle` is declared by driver_base in the package file, so both the
    // receiver's type and the method live outside the open buffer.
    analyzer.open(uri, "import tb_pkg::*;\n"
                       "class my_drv extends driver_base;\n"
                       "    task run();\n"
                       "        port_handle.raise_objection();\n"
                       "    endtask\n"
                       "endclass\n");

    auto loc = analyzer.definition_of(uri, 3, 22);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(pkg_path));
    CHECK(loc->line == 2);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: package parameter resolves through a wildcard import",
          "[definition][package]") {
    Analyzer analyzer;
    // Shaped like UVM's `uvm_object_globals.svh`: a typedef'd parameter type,
    // a parameter defined in terms of another parameter, and an enum next to
    // them, all inside a package body pulled in through an include.
    const auto inc_path = write_temp_sv("lazyverilog_definition_pkg_param_inc.svh",
                                        "typedef bit [7:0] flag_t;\n"
                                        "parameter int P_INT = 3;\n"
                                        "parameter flag_t P_TYPED = 8'h1;\n"
                                        "parameter flag_t P_DERIVED = P_TYPED;\n"
                                        "typedef enum int { MODE_ONE = 1 } mode_e;\n");
    const auto pkg_path = write_temp_sv("lazyverilog_definition_pkg_param.sv",
                                        "package flag_pkg;\n"
                                        "`include \"lazyverilog_definition_pkg_param_inc.svh\"\n"
                                        "endpackage\n");
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_definition_pkg_param_use.sv";
    analyzer.open(uri, "import flag_pkg::*;\n"
                       "module top;\n"
                       "    int a = P_INT;\n"
                       "    int b = P_TYPED;\n"
                       "    int c = P_DERIVED;\n"
                       "    int d = MODE_ONE;\n"
                       "endmodule\n");

    auto param_loc = analyzer.definition_of(uri, 2, 13);
    REQUIRE(param_loc.has_value());
    CHECK(param_loc->uri == uri_from_path(inc_path));
    CHECK(param_loc->line == 1);

    // The parameter's type and its initializer must not change the answer, and
    // the enum member next to them keeps working.
    CHECK(analyzer.definition_of(uri, 3, 13).has_value());
    CHECK(analyzer.definition_of(uri, 4, 13).has_value());
    CHECK(analyzer.definition_of(uri, 5, 13).has_value());

    std::filesystem::remove(pkg_path);
    std::filesystem::remove(inc_path);
}

TEST_CASE("definition: macro-generated typedef resolves to its declaration",
          "[definition][macro]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_definition_macro_typedef.sv";
    analyzer.open(uri, "`define UTILS(T) typedef registry #(T) type_id;\n"
                       "class registry #(type T = int);\n"
                       "endclass\n"
                       "class my_item;\n"
                       "    `UTILS(my_item)\n"
                       "endclass\n"
                       "module top;\n"
                       "    initial my_item::type_id::create();\n"
                       "endmodule\n");

    auto loc = analyzer.definition_of(uri, 7, 22);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    // `type_id` here is a literal in the macro *body*, not a substituted
    // argument -- must not be confused with the isMacroArgLoc-only case.
    CHECK(loc->line == 0);
    CHECK(loc->col == 39);
}

TEST_CASE("definition: class-scoped typedef resolves from a closed package file",
          "[definition]") {
    const auto pkg_path = write_temp_sv("lazyverilog_definition_closed_type_id.sv",
                                        "package type_pkg;\n"
                                        "    class registry #(type T = int);\n"
                                        "        static function T create();\n"
                                        "        endfunction\n"
                                        "    endclass\n"
                                        "    class my_item;\n"
                                        "        typedef registry #(my_item) type_id;\n"
                                        "    endclass\n"
                                        "endpackage\n");

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    // The package file stays closed, so the class body is reachable only
    // through its SyntaxIndex shard.
    const std::string uri = "file:///tmp/lazyverilog_definition_closed_type_id_use.sv";
    analyzer.open(uri, "import type_pkg::*;\n"
                       "module top;\n"
                       "    initial my_item::type_id::create();\n"
                       "endmodule\n");

    auto loc = analyzer.definition_of(uri, 2, 22);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(pkg_path));
    CHECK(loc->line == 6);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: static method called directly on a parameterized closed-file "
          "class resolves without a type_id hop",
          "[definition]") {
    // `ClassName #(params)::method()` parses its qualifier as ClassNameSyntax,
    // not the plain IdentifierNameSyntax a `pkg::name`/`my_item::type_id`
    // qualifier parses as.  The scoped-name handler only recognized the latter,
    // so a parameterized qualifier fell through to an unscoped bare-name
    // lookup that had no way to find a member scoped to a different class --
    // e.g. `uvm_config_db #(int)::get(...)`.
    const auto pkg_path = write_temp_sv("lazyverilog_definition_param_class_static.sv",
                                        "package registry_pkg;\n"
                                        "    class registry #(type T = int);\n"
                                        "        static function T get();\n"
                                        "        endfunction\n"
                                        "    endclass\n"
                                        "endpackage\n");

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_definition_param_class_static_use.sv";
    analyzer.open(uri, "import registry_pkg::*;\n"
                       "module top;\n"
                       "    initial registry #(int)::get();\n"
                       "endmodule\n");

    auto loc = analyzer.definition_of(uri, 2, 30);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(pkg_path));
    CHECK(loc->line == 2);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: field on a receiver with a parameterized declared type",
          "[definition]") {
    // `Container #(byte)` used to resolve the receiver's class name to `byte`
    // (the trailing parameter) instead of `Container`, since the type-name
    // heuristic grabbed the last identifier-like run in the type text.
    const auto pkg_path = write_temp_sv("lazyverilog_definition_param_field.sv",
                                        "package pkg;\n"
                                        "    class Container #(type REQ = int, RSP = REQ);\n"
                                        "        int seq_item_export;\n"
                                        "    endclass\n"
                                        "endpackage\n");

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_definition_param_field_use.sv";
    analyzer.open(uri, "import pkg::*;\n"
                       "module top;\n"
                       "    Container #(byte) sequencer;\n"
                       "    initial sequencer.seq_item_export = 1;\n"
                       "endmodule\n");

    auto loc = analyzer.definition_of(uri, 3, 24);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(pkg_path));
    CHECK(loc->line == 2);

    std::filesystem::remove(pkg_path);
}

TEST_CASE("definition: class method declared entirely inside a macro body resolves "
          "from a closed package file",
          "[definition][macro]") {
    // Mirrors UVM's `UVM_SEQ_ITEM_PULL_IMP, which expands to a whole
    // `task get_next_item(...); ... endtask` member -- the method name is a
    // macro-body literal, not a substituted argument.  Closed-file indexing
    // used to compute the entry's location from the raw (unresolved)
    // macro-expansion buffer location, which has no real line numbers, so the
    // entry landed at line 0 and find_class_member_definition's `line <= 0`
    // guard silently dropped it.
    const std::string macro_line =
        "    `define GEN_ITEM_METHOD(IMP) task get_next_item(); IMP.get_next_item(); "
        "endtask\n";
    const auto pkg_path = write_temp_sv("lazyverilog_definition_macro_method.sv",
                                        "package pkg;\n" + macro_line +
                                        "    class base;\n"
                                        "        `GEN_ITEM_METHOD(imp)\n"
                                        "    endclass\n"
                                        "endpackage\n");

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string use_line = "    initial b.get_next_item();\n";
    const std::string uri = "file:///tmp/lazyverilog_definition_macro_method_use.sv";
    analyzer.open(uri, "import pkg::*;\n"
                       "module top;\n"
                       "    base b;\n" +
                           use_line + "endmodule\n");

    auto loc = analyzer.definition_of(uri, 3, (int)use_line.find("get_next_item"));
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(pkg_path));
    CHECK(loc->line == 1);
    CHECK(loc->col == (int)macro_line.find("get_next_item"));

    std::filesystem::remove(pkg_path);
}

// `T::type_id::create(...)` is the most-typed line in a UVM testbench, and the
// class it is written on always has a `create` of its own -- `uvm_object::create`,
// generated by the same factory macro that declares `type_id`.  The scoped-name
// handler only understood a single `::` hop, so the nested qualifier was dropped
// and `create` resolved against the enclosing class instead of the registry the
// alias names.
static const std::string kScopedTypedefSource = R"(
class registry_base;
    static function registry_base create(string name, int parent);
        return null;
    endfunction
endclass

class thing;
    typedef registry_base type_id;

    static function thing create(string name = "");
        return null;
    endfunction
endclass

module m;
    initial begin
        thing::type_id::create("x", 0);
    end
endmodule
)";

// Line/column of the `create` in `thing::type_id::create`.
static std::pair<int, int> scoped_typedef_call_position() {
    const auto offset = kScopedTypedefSource.find("thing::type_id::create");
    REQUIRE(offset != std::string::npos);
    int line = 0;
    int col = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (kScopedTypedefSource[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col + (int)std::string("thing::type_id::").size()};
}

TEST_CASE("definition: T::type_id::create resolves through the alias, not the caller's class",
          "[definition][scoped]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_scoped_typedef.sv";
    analyzer.open(uri, kScopedTypedefSource);

    const auto [line, col] = scoped_typedef_call_position();
    auto loc = analyzer.definition_of(uri, line, col);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 2); // registry_base::create, not thing::create
}

TEST_CASE("hover: T::type_id::create describes the alias target's method",
          "[definition][scoped]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_scoped_typedef_hover.sv";
    analyzer.open(uri, kScopedTypedefSource);

    const auto [line, col] = scoped_typedef_call_position();
    auto info = analyzer.symbol_at(uri, line, col);
    REQUIRE(info.has_value());
    CHECK(info->name == "create");
    // `registry_base create(string name, int parent)`, not `thing create(string name = "")`.
    CHECK(info->doc.find("function registry_base create") != std::string::npos);
    CHECK(info->doc.find("int parent") != std::string::npos);
}

TEST_CASE("definition: loop variable resolves to its own for-loop declaration",
          "[definition][scope]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_for_loop_scope.sv";
    analyzer.open(uri,
                  "module m;\n"
                  "always_comb begin\n"
                  "    for (int i = 0; i < 10; i++) begin\n"
                  "        o_data[i] = 1;\n"
                  "    end\n"
                  "\n"
                  "    for (int i = 0; i < 10; i++) begin\n"
                  "        address[i] = 1;\n"
                  "    end\n"
                  "end\n"
                  "endmodule\n");

    auto first = analyzer.definition_of(uri, 3, 15);
    REQUIRE(first.has_value());
    CHECK(first->line == 2);
    CHECK(first->col == 13);

    auto second = analyzer.definition_of(uri, 7, 16);
    REQUIRE(second.has_value());
    CHECK(second->line == 6);
    CHECK(second->col == 13);
}

TEST_CASE("definition: block-local declaration does not shadow a later sibling block",
          "[definition][scope]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_block_scope.sv";
    analyzer.open(uri,
                  "module m;\n"
                  "always_comb begin\n"
                  "    begin\n"
                  "        int tmp;\n"
                  "        tmp = 1;\n"
                  "    end\n"
                  "    begin\n"
                  "        int tmp;\n"
                  "        tmp = 2;\n"
                  "    end\n"
                  "end\n"
                  "endmodule\n");

    auto loc = analyzer.definition_of(uri, 8, 8);
    REQUIRE(loc.has_value());
    CHECK(loc->line == 7);
}


TEST_CASE("definition: `include path resolves to the included file", "[definition][include]") {
    Analyzer analyzer;
    const auto inc_path = write_temp_sv("lazyverilog_definition_include_target.svh",
                                        "parameter int INCLUDED_PARAMETER = 7;\n");
    const std::string top_uri = uri_from_path(std::filesystem::temp_directory_path() /
                                              "lazyverilog_definition_include_top.sv");
    analyzer.open(top_uri, "`include \"lazyverilog_definition_include_target.svh\"\n"
                           "module top;\n"
                           "  localparam int X = INCLUDED_PARAMETER;\n"
                           "endmodule\n");

    // Cursor inside the quoted path.  The path is a string literal, so this
    // resolves through the preprocessor's include relation rather than through
    // an identifier lookup.
    auto loc = analyzer.definition_of(top_uri, 0, 14);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(inc_path));
    CHECK(loc->line == 0);
    CHECK(loc->col == 0);

    std::filesystem::remove(inc_path);
}

TEST_CASE("definition: `include of a missing file stays unresolved", "[definition][include]") {
    Analyzer analyzer;
    const std::string top_uri = uri_from_path(std::filesystem::temp_directory_path() /
                                              "lazyverilog_definition_include_missing.sv");
    analyzer.open(top_uri, "`include \"lazyverilog_definition_no_such_header.svh\"\n"
                           "module top;\n"
                           "endmodule\n");

    CHECK_FALSE(analyzer.definition_of(top_uri, 0, 14).has_value());
}

TEST_CASE("definition: `include resolution reports the directly included file",
          "[definition][include]") {
    Analyzer analyzer;
    const auto inner_path = write_temp_sv("lazyverilog_definition_include_inner.svh",
                                          "parameter int INNER_PARAMETER = 2;\n");
    const auto outer_path =
        write_temp_sv("lazyverilog_definition_include_outer.svh",
                      "`include \"lazyverilog_definition_include_inner.svh\"\n"
                      "parameter int OUTER_PARAMETER = 1;\n");
    const std::string top_uri = uri_from_path(std::filesystem::temp_directory_path() /
                                              "lazyverilog_definition_include_nested_top.sv");
    analyzer.open(top_uri, "`include \"lazyverilog_definition_include_outer.svh\"\n"
                           "module top;\n"
                           "  localparam int X = INNER_PARAMETER;\n"
                           "endmodule\n");

    // The nested header is in the same SourceManager, but its directive lives in
    // the outer header -- not on this line -- so it must not win here.
    auto loc = analyzer.definition_of(top_uri, 0, 14);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri_from_path(outer_path));

    std::filesystem::remove(outer_path);
    std::filesystem::remove(inner_path);
}

TEST_CASE("definition: a line without an `include is unaffected", "[definition][include]") {
    Analyzer analyzer;
    const auto inc_path = write_temp_sv("lazyverilog_definition_include_unaffected.svh",
                                        "parameter int INCLUDED_PARAMETER = 7;\n");
    const std::string top_uri = uri_from_path(std::filesystem::temp_directory_path() /
                                              "lazyverilog_definition_include_unaffected_top.sv");
    analyzer.open(top_uri, "`include \"lazyverilog_definition_include_unaffected.svh\"\n"
                           "module top;\n"
                           "  localparam int X = 1;\n"
                           "  logic unresolved_name;\n"
                           "endmodule\n");

    // An unresolvable position on an ordinary line must not fall back to the
    // file's include target.
    auto loc = analyzer.definition_of(top_uri, 2, 19);
    CHECK((!loc.has_value() || loc->uri != uri_from_path(inc_path)));

    std::filesystem::remove(inc_path);
}

TEST_CASE("hover: `include path reports the included file", "[definition][include]") {
    Analyzer analyzer;
    const auto inc_path = write_temp_sv("lazyverilog_hover_include_target.svh",
                                        "parameter int INCLUDED_PARAMETER = 7;\n");
    const std::string top_uri = uri_from_path(std::filesystem::temp_directory_path() /
                                              "lazyverilog_hover_include_top.sv");
    analyzer.open(top_uri, "`include \"lazyverilog_hover_include_target.svh\"\n"
                           "module top;\n"
                           "endmodule\n");

    auto info = analyzer.symbol_at(top_uri, 0, 14);
    REQUIRE(info.has_value());
    CHECK(info->kind == "include");
    CHECK(info->name == "lazyverilog_hover_include_target.svh");

    std::filesystem::remove(inc_path);
}

// The open-buffer index skipped `extern` method prototypes that the closed-file
// shard builder already indexed, so a class written in the extern style lost every
// method the moment its file was opened: `obj.bump()` resolved while the file
// was closed and stopped resolving once it was open.
TEST_CASE("definition: a call to an extern-declared method in an open buffer", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_extern_method.sv";
    analyzer.open(uri, R"(package p;
  class base_c;
    extern function void bump(int amount);
  endclass
  function void base_c::bump(int amount);
  endfunction
endpackage

module top;
  initial begin
    p::base_c obj = new();
    obj.bump(3);
  end
endmodule
)");

    auto loc = analyzer.definition_of(uri, 11, 9);
    REQUIRE(loc.has_value());
    CHECK(loc->line == 2); // the extern prototype
}

// A declaration inside a generate block is a different signal from the
// module-level one that shares its name.  The index recorded generate-block
// declarations nowhere, so every use inside the block resolved to the outer
// declaration.
TEST_CASE("definition: a generate-block declaration shadows the module-level one",
          "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_generate_shadow.sv";
    const std::string text = R"(module top;
  logic [7:0] dout;

  genvar i;
  generate
    for (i = 0; i < 2; i++) begin : g_lanes
      logic [7:0] dout;
      assign dout = 8'h1;
    end
  endgenerate
endmodule
)";
    analyzer.open(uri, text);

    // `dout` on the assign inside the generate block (line 7, 0-based).
    auto inner = analyzer.definition_of(uri, 7, 13);
    REQUIRE(inner.has_value());
    CHECK(inner->line == 6);

    // The module-level declaration still answers for itself.
    auto outer = analyzer.definition_of(uri, 1, 14);
    REQUIRE(outer.has_value());
    CHECK(outer->line == 1);
}

// Navigating a testbench by hierarchy (`tb.u_dut.sig`) resolved nothing: the
// index knows instances and each module's signals, but nothing walked the path.

// Navigating a testbench by hierarchy (`tb.u_dut.sig`) resolved nothing: the
// index knows instances and each module's signals, but nothing walked the path.
TEST_CASE("definition: hierarchical reference resolves through instance paths",
          "[definition]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog-definition-hier";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const auto sub = dir / "hier_sub.sv";
    const auto dut = dir / "hier_dut.sv";
    {
        std::ofstream out(sub);
        out << "module hier_sub (input logic clk);\n"
               "  logic [7:0] stage;\n"
               "endmodule\n";
    }
    {
        std::ofstream out(dut);
        out << "module hier_dut (input logic clk);\n"
               "  logic [7:0] din;\n"
               "  genvar i;\n"
               "  generate\n"
               "    for (i = 0; i < 2; i++) begin : g_lanes\n"
               "      hier_sub u_sub (.clk(clk));\n"
               "    end\n"
               "  endgenerate\n"
               "endmodule\n";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({sub.string(), dut.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = uri_from_path(dir / "hier_tb.sv");
    const std::string text =
        "module hier_tb;\n"
        "  logic clk;\n"
        "  hier_dut u_dut (.clk(clk));\n"
        "  initial begin\n"
        "    force hier_tb.u_dut.din = 8'h5a;\n"
        "    $display(\"%0h\", hier_tb.u_dut.g_lanes[0].u_sub.stage);\n"
        "    $display(\"%0h\", u_dut.nosuch_signal);\n"
        "  end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    SECTION("plain instance path") {
        const auto col = (int)text.find("u_dut.din") + 6;
        const auto line_start = text.rfind('\n', text.find("u_dut.din")) + 1;
        auto loc = analyzer.definition_of(uri, 4, (int)(text.find("u_dut.din") + 6 - line_start));
        (void)col;
        REQUIRE(loc.has_value());
        CHECK(loc->uri == uri_from_path(dut));
        CHECK(loc->line == 1);
    }

    SECTION("through a named generate block") {
        const auto needle = text.find("u_sub.stage");
        const auto line_start = text.rfind('\n', needle) + 1;
        auto loc = analyzer.definition_of(uri, 5, (int)(needle + 6 - line_start));
        REQUIRE(loc.has_value());
        CHECK(loc->uri == uri_from_path(sub));
        CHECK(loc->line == 1);
    }

    SECTION("unresolvable path answers nothing") {
        const auto needle = text.find("nosuch_signal");
        const auto line_start = text.rfind('\n', needle) + 1;
        auto loc = analyzer.definition_of(uri, 6, (int)(needle - line_start));
        CHECK_FALSE(loc.has_value());
    }

    std::filesystem::remove_all(dir);
}

// The open-buffer index skipped `extern` method prototypes that the closed-file
// shard builder already indexed, so a class written in the extern style lost every
// method the moment its file was opened: `obj.bump()` resolved while the file
// was closed and stopped resolving once it was open.
