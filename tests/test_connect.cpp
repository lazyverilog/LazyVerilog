#include "analyzer.hpp"
#include "features/connect.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

TEST_CASE("connect: reports modules, ports, and hierarchical instances", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic [7:0] data);
endmodule
module consumer(input logic [7:0] data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto json = connect_info_json(analyzer, uri);
    CHECK(json.find("\"producer\"") != std::string::npos);
    CHECK(json.find("\"consumer\"") != std::string::npos);
    CHECK(json.find("top.u_prod") != std::string::npos);
    CHECK(json.find("top.u_cons") != std::string::npos);
}

TEST_CASE("connect: lazy info defers hierarchy expansion", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_lazy_hierarchy_fixture.sv";
    analyzer.open(uri, R"(
module leaf(output logic data);
endmodule
module mid;
    leaf u_leaf (.data());
endmodule
module top;
    mid u_mid ();
endmodule
)");

    const auto info = connect_info_json(analyzer, uri, true);
    CHECK(info.find("\"leaf\"") != std::string::npos);
    CHECK(info.find("\"roots\"") != std::string::npos);
    CHECK(info.find("top.u_mid.u_leaf") == std::string::npos);

    const auto top_children = connect_hierarchy_children_json(analyzer, uri, "top");
    CHECK(top_children.find("top.u_mid") != std::string::npos);
    CHECK(top_children.find("top.u_mid.u_leaf") == std::string::npos);

    const auto mid_children = connect_hierarchy_children_json(analyzer, uri, "top.u_mid");
    CHECK(mid_children.find("top.u_mid.u_leaf") != std::string::npos);
}

TEST_CASE("connect: preview and apply produce wiring edits", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_apply_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic [7:0] data);
endmodule
module consumer(input logic [7:0] data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto preview = connect_apply_preview_json(analyzer, uri, "top.u_prod", "data",
                                                    "top.u_cons", "data", "data_w");
    CHECK(preview.find("connect top.u_prod.data(data_w)") != std::string::npos);
    CHECK(preview.find("declare logic [7:0] data_w in top") != std::string::npos);

    const auto edit = connect_apply_edit_json(analyzer, uri, "top.u_prod", "data",
                                              "top.u_cons", "data", "data_w");
    CHECK(edit.find(".data(data_w)") != std::string::npos);
    CHECK(edit.find("logic [7:0] data_w;") != std::string::npos);
}

TEST_CASE("connect: wire declaration fallback is scoped to common ancestor module", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_wire_insert_scope.sv";
    analyzer.open(uri, R"(
module helper(input logic x);
endmodule
module producer(output logic data);
endmodule
module consumer(input logic data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri, "top.u_prod", "data",
                                              "top.u_cons", "data", "data_w");
    CHECK(edit.find("logic data_w;") != std::string::npos);
    CHECK(edit.find("\"start\":{\"line\":8,\"character\":0},\"end\":{\"line\":8,\"character\":0}},\"newText\":\"logic data_w;\\n\"") != std::string::npos);
}

TEST_CASE("connect: root wire is not inserted into ANSI port list", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_wire_insert_ansi_root.sv";
    analyzer.open(uri, R"(
module producer(output logic data);
endmodule
module consumer(input logic data);
endmodule
module top (
    input logic clk
);
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri, "top.u_prod", "data",
                                              "top.u_cons", "data", "data_w");
    CHECK(edit.find("logic data_w;") != std::string::npos);
    CHECK(edit.find("\"start\":{\"line\":8,\"character\":0},\"end\":{\"line\":8,\"character\":0}},\"newText\":\"logic data_w;\\n\"") != std::string::npos);
}

TEST_CASE("interface: returns shared signal rows and connect edits", "[connect][interface]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/interface_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic ready);
endmodule
module consumer(input logic ready);
endmodule
module top;
    logic ready_w;
    producer u_prod (.ready(ready_w));
    consumer u_cons (.ready(ready_w));
endmodule
)");

    const auto iface = interface_json(analyzer, uri, "u_prod", "u_cons");
    CHECK(iface.find("\"inst1_port\":\"ready\"") != std::string::npos);
    CHECK(iface.find("\"inst2_port\":\"ready\"") != std::string::npos);
    CHECK(iface.find("ready_w") != std::string::npos);

    const auto single = single_interface_json(analyzer, uri, "u_prod");
    CHECK(single.find("\"other_inst\":\"u_cons\"") != std::string::npos);

    const auto edit = interface_connect_edit_json(analyzer, uri, "u_prod", "u_cons",
                                                  "ready", "ready", "ready2_w", "logic");
    CHECK(edit.find(".ready(ready2_w)") != std::string::npos);
    CHECK(edit.find("logic ready2_w;") != std::string::npos);
}

TEST_CASE("interface: declares complete net port types and preserves symbolic dimensions", "[connect][interface]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/interface_type_fixture.sv";
    analyzer.open(uri, R"(
`define BUS_W 6
typedef struct packed { logic valid; } payload_t;
module memory(input wire [5:0] i_data, output wire [5:0] o_data);
endmodule
module typed_source(output payload_t [`BUS_W-1:0] payload);
endmodule
module typed_sink(input payload_t [DEPTH-1:0] payload);
endmodule
module top;
    memory u_mem2 (.i_data(), .o_data());
    memory u_mem3 (.i_data(), .o_data());
    typed_source u_src (.payload());
    typed_sink u_dst (.payload());
endmodule
)");

    const auto iface = interface_json(analyzer, uri, "u_mem2", "u_mem3");
    CHECK(iface.find("\"type\":\"wire [5:0]\"") != std::string::npos);

    const auto net_edit = interface_connect_edit_json(analyzer, uri, "u_mem2", "u_mem3",
                                                      "o_data", "i_data", "data32", "wire [5:0]");
    CHECK(net_edit.find("logic [5:0] data32;") != std::string::npos);

    const auto reverse_order_edit = interface_connect_edit_json(analyzer, uri, "u_mem3", "u_mem2",
                                                               "i_data", "o_data", "data33", "wire [5:0]");
    CHECK(reverse_order_edit.find("logic [5:0] data33;") != std::string::npos);

    const auto typed = interface_json(analyzer, uri, "u_src", "u_dst");
    CHECK(typed.find("payload_t [`BUS_W-1:0]") != std::string::npos);
    CHECK(typed.find("payload_t [DEPTH-1:0]") != std::string::npos);

    const auto typed_edit = interface_connect_edit_json(analyzer, uri, "u_src", "u_dst",
                                                        "payload", "payload", "payload_w",
                                                        "payload_t [`BUS_W-1:0]");
    CHECK(typed_edit.find("payload_t [`BUS_W-1:0] payload_w;") != std::string::npos);
}

TEST_CASE("interface: falls back when UI supplies only packed dimensions", "[connect][interface]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/interface_dimension_only_fixture.sv";
    analyzer.open(uri, R"(
module producer(output wire [WIDTH-1:0] data);
endmodule
module consumer(input wire [WIDTH-1:0] data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto edit = interface_connect_edit_json(analyzer, uri, "u_prod", "u_cons",
                                                  "data", "data", "data_w", "[WIDTH-1:0]");
    CHECK(edit.find("logic [WIDTH-1:0] data_w;") != std::string::npos);
}

TEST_CASE("connect: AST declaration lookup handles multiline and dollar identifiers", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_ast_decl_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic [7:0] data);
endmodule
module consumer(input logic [7:0] data);
endmodule
module top;
    logic [7:0]
        data$w;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri, "top.u_prod", "data",
                                              "top.u_cons", "data", "data$w");
    CHECK(edit.find("logic [7:0] data$w;") == std::string::npos);
    CHECK(edit.find(".data(data$w)") != std::string::npos);
}

TEST_CASE("interface: AST signal type lookup handles multiline declarations", "[connect][interface]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/interface_ast_type_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic [7:0] data);
endmodule
module consumer(input logic [7:0] data);
endmodule
module top;
    logic [7:0]
        data_w;
    producer u_prod (.data(data_w));
    consumer u_cons (.data(data_w));
endmodule
)");

    const auto iface = interface_json(analyzer, uri, "u_prod", "u_cons");
    CHECK(iface.find("\"signal_type\":\"logic [7:0]\"") != std::string::npos);
}

TEST_CASE("connect: cross-hierarchy route uses chosen boundary ports", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_hierarchy_route_fixture.sv";
    analyzer.open(uri, R"(
module A(output logic [3:0] o_a);
endmodule
module B(input logic [3:0] i_b);
endmodule
module A2;
    A u_a (
        .o_a()
    );
endmodule
module B2;
    B u_b (
        .i_b()
    );
endmodule
module top;
    A2 u_a2 (
    );
    B2 u_b2 (
    );
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri,
                                              "top.u_a2.u_a", "o_a",
                                              "top.u_b2.u_b", "i_b",
                                              "a_to_b_w",
                                              {"o_a2"}, {"i_b2"});
    CHECK(edit.find(".o_a(o_a2)") != std::string::npos);
    CHECK(edit.find(".i_b(i_b2)") != std::string::npos);
    CHECK(edit.find("output logic [3:0] o_a2") != std::string::npos);
    CHECK(edit.find("input logic [3:0] i_b2") != std::string::npos);
    CHECK(edit.find("logic [3:0] a_to_b_w;") != std::string::npos);
    CHECK(edit.find(".o_a2(a_to_b_w)") != std::string::npos);
    CHECK(edit.find(".i_b2(a_to_b_w)") != std::string::npos);
}

TEST_CASE("connect: same child module under two parents routes through existing boundary ports", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_same_child_existing_ports.sv";
    analyzer.open(uri, R"(
module inv(input logic i, output logic o);
endmodule
module memory(input logic [3:0] i_data, output logic [3:0] o_data);
    inv u_inv (
        .i(old_i),
        .o(old_o)
    );
endmodule
module memory_top;
    memory u_mem2 (
        .i_data(),
        .o_data()
    );
    memory u_mem3 (
        .i_data(),
        .o_data()
    );
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri,
                                              "memory_top.u_mem3.u_inv", "o",
                                              "memory_top.u_mem2.u_inv", "i",
                                              "data_w",
                                              {"o_data"}, {"i_data"});
    CHECK(edit.find(".o(o_data)") != std::string::npos);
    CHECK(edit.find(".i(i_data)") != std::string::npos);
    CHECK(edit.find(".o_data(data_w)") != std::string::npos);
    CHECK(edit.find(".i_data(data_w)") != std::string::npos);
    CHECK(edit.find("logic [3:0] data_w;") != std::string::npos);
}


TEST_CASE("connect: edits closed filelist child module text", "[connect]") {
    const std::string inv_path =
        (std::filesystem::temp_directory_path() / "lv_connect_closed_inv.sv").string();
    const std::string mem_path =
        (std::filesystem::temp_directory_path() / "lv_connect_closed_memory.sv").string();
    {
        std::ofstream inv(inv_path);
        inv << R"(module inv(input logic i, output logic o);
endmodule
)";
    }
    {
        std::ofstream mem(mem_path);
        mem << R"(module memory(input logic [3:0] i_data, output logic [3:0] o_data);
    inv u_inv (
        .i(old_i),
        .o(old_o)
    );
endmodule
)";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({inv_path, mem_path});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///tmp/lv_connect_closed_top.sv";
    analyzer.open(uri, R"(module memory_top;
    memory u_mem2 (
        .i_data(),
        .o_data()
    );
    memory u_mem3 (
        .i_data(),
        .o_data()
    );
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri,
                                              "memory_top.u_mem3.u_inv", "o",
                                              "memory_top.u_mem2.u_inv", "i",
                                              "data_w",
                                              {"o_data"}, {"i_data"});
    CHECK(edit.find("lv_connect_closed_memory.sv") != std::string::npos);
    CHECK(edit.find(".o(o_data)") != std::string::npos);
    CHECK(edit.find(".i(i_data)") != std::string::npos);
    CHECK(edit.find(".o_data(data_w)") != std::string::npos);
    CHECK(edit.find(".i_data(data_w)") != std::string::npos);
    CHECK(edit.find("logic [3:0] data_w;") != std::string::npos);
}

TEST_CASE("connect: empty instance receives merged missing port edit", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_empty_instance_merge.sv";
    analyzer.open(uri, R"(
module inv(input logic i, output logic o);
endmodule
module memory(input logic [3:0] i_data, output logic [3:0] o_data);
    inv u_inv ();
endmodule
module memory_top;
    memory u_mem2 (
        .i_data(),
        .o_data()
    );
    memory u_mem3 (
        .i_data(),
        .o_data()
    );
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri,
                                              "memory_top.u_mem3.u_inv", "o",
                                              "memory_top.u_mem2.u_inv", "i",
                                              "data_w",
                                              {"o_data"}, {"i_data"});
    CHECK(edit.find("inv u_inv (\\n        .o(o_data),\\n        .i(i_data)\\n    );") != std::string::npos);
    CHECK(edit.find(".o(o_data));") == std::string::npos);
    CHECK(edit.find(".i(i_data));") == std::string::npos);
}

TEST_CASE("connect: typed missing leaf ports are added before instance wiring", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_missing_leaf_ports.sv";
    analyzer.open(uri, R"(
module inv(input logic i);
endmodule
module memory(input logic [3:0] i_data, output logic [3:0] o_data);
    inv u_inv ();
endmodule
module memory_top;
    memory u_mem2 (
        .i_data(),
        .o_data()
    );
    memory u_mem3 (
        .i_data(),
        .o_data()
    );
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri,
                                              "memory_top.u_mem3.u_inv", "new_o",
                                              "memory_top.u_mem2.u_inv", "new_i",
                                              "data_w",
                                              {"o_data"}, {"i_data"});
    CHECK(edit.find("output logic [3:0] new_o") != std::string::npos);
    CHECK(edit.find("input logic [3:0] new_i") != std::string::npos);
    CHECK(edit.find(".new_o(o_data)") != std::string::npos);
    CHECK(edit.find(".new_i(i_data)") != std::string::npos);
    CHECK(edit.find("logic [3:0] data_w;") != std::string::npos);
}

TEST_CASE("connect: missing intermediate port follows non-ANSI module style", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_non_ansi_boundary.sv";
    analyzer.open(uri, R"(
module leaf(input logic i, output logic o);
endmodule
module mid(a);
    output logic a;
    leaf u_leaf (
        .i(),
        .o()
    );
endmodule
module top;
    mid u_src(.a());
    mid u_dst(.a());
endmodule
)");

    const auto edit = connect_apply_edit_json(analyzer, uri,
                                              "top.u_src.u_leaf", "o",
                                              "top.u_dst.u_leaf", "i",
                                              "w",
                                              {"new_out"}, {"new_in"});
    CHECK(edit.find("new_out") != std::string::npos);
    CHECK(edit.find("output logic new_out;") != std::string::npos);
    CHECK(edit.find("input logic new_in;") != std::string::npos);
    CHECK(edit.find("output logic new_out") != std::string::npos);
    CHECK(edit.find(".new_out(w)") != std::string::npos);
    CHECK(edit.find(".new_in(w)") != std::string::npos);
}

TEST_CASE("connect: header edits survive a non-ASCII comment in the port list",
          "[connect]") {
    // Every column Connect reads out of the index -- ModuleEntry::col,
    // header_semi_col, port_list_close_col -- is an LSP column, and LSP counts
    // UTF-16 code units rather than bytes.  Slicing the module header with them
    // as if they were byte offsets agrees only while the header stays ASCII;
    // one multi-byte character inside the port list is enough to cut in the
    // wrong place and write the mangled header back.
    //
    // The comment sits on the header line itself, ahead of the `)` and the `;`
    // whose columns get converted -- that is what makes the two units disagree
    // for this edit.  Its bytes are written as escapes so this file stays ASCII
    // for compilers that do not default to UTF-8 source encoding.
    const std::string wide = "\xed\x81\xb4\xeb\x9f\xad"; // two 3-byte characters
    const std::string source = "\n"
                               "module leaf(input logic i, output logic o);\n"
                               "endmodule\n"
                               "module mid (input logic clk, /* " + wide +
                               " */ output logic done);\n"
                               "    leaf u_leaf (\n"
                               "        .i(),\n"
                               "        .o()\n"
                               "    );\n"
                               "endmodule\n"
                               "module top;\n"
                               "    mid u_src (.clk(), .done());\n"
                               "    mid u_dst (.clk(), .done());\n"
                               "endmodule\n";

    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_non_ascii_header.sv";
    analyzer.open(uri, source);

    // Routing through boundary ports that do not exist yet forces `mid`'s whole
    // header to be regenerated, which is the slice that used to go wrong.
    const auto edit = connect_apply_edit_json(analyzer, uri,
                                              "top.u_src.u_leaf", "o",
                                              "top.u_dst.u_leaf", "i",
                                              "w",
                                              {"new_out"}, {"new_in"});

    // The comment is carried through the regenerated header intact: not cut
    // short, not duplicated, and still attached to the port it documents.
    CHECK(edit.find("new_out") != std::string::npos);
    CHECK(edit.find("new_in") != std::string::npos);
    // The original port list survives verbatim, comment bytes included.
    CHECK(edit.find("input logic clk, /* " + wide + " */ output logic done") !=
          std::string::npos);
    // Exactly one copy of the comment: a short slice used to leave the tail
    // behind in the untouched text as well as in the replacement.
    const auto first = edit.find(wide);
    REQUIRE(first != std::string::npos);
    CHECK(edit.find(wide, first + wide.size()) == std::string::npos);
}

// ── Declaration-walk cost model ──────────────────────────────────────────────
//
// Preparing a connect edit derives the edited module's declarations from the
// live AST.  That walk visits each declaration once, which is fine; what must
// not happen is per-declaration work proportional to the *file*, because the
// two grow together and the product is quadratic.
//
// Two ways in used to do exactly that: each declaration copied the whole source
// buffer into a std::string to find its line and column (twice, once per end of
// the range), and the line itself was found by counting newlines from byte zero.
// On a 111 KB module with 4000 declarations one preview took 862 ms.
//
// Guarded as a ratio between two structurally identical modules at different
// sizes, never an absolute budget: the quadratic term shows up as a ratio far
// above the size ratio, and that survives a shared CI runner where a millisecond
// threshold would not.
namespace {

std::string filler_module_source(int declarations) {
    std::string text = "\nmodule leaf(input logic i, output logic o);\nendmodule\n"
                       "module src_m(output logic y);\nendmodule\n"
                       "module top;\n";
    for (int i = 0; i < declarations; ++i)
        text += "    logic [31:0] filler_" + std::to_string(i) + ";\n";
    text += "    leaf u_leaf (.i(), .o());\n"
            "    src_m u_src (.y());\n"
            "endmodule\n";
    return text;
}

/// Fastest wall time of one connect preview over a module with @p declarations
/// declarations.  The minimum, not the median, for the reason the shared-header
/// guard takes it: the cost being measured is extra work, which raises the
/// floor, and everything a loaded runner adds only ever makes a sample slower.
double fastest_preview_ms(int declarations, int runs) {
    Analyzer analyzer;
    const std::string uri =
        "file:///tmp/connect_scaling_" + std::to_string(declarations) + ".sv";
    analyzer.open(uri, filler_module_source(declarations));

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto preview = connect_apply_preview_json(
            analyzer, uri, "top.u_src", "y", "top.u_leaf", "i",
            "w_" + std::to_string(i));
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        REQUIRE(preview.find("\"error\"") == std::string::npos);
        samples.push_back(elapsed);
    }
    std::sort(samples.begin(), samples.end());
    return samples.front();
}

} // namespace

TEST_CASE("connect: preview cost is linear in declaration count", "[connect][scaling]") {
    constexpr int kSmall = 250;
    constexpr int kLarge = 1000; // 4x the declarations, and ~4x the file
    constexpr int kRuns = 7;

    (void)fastest_preview_ms(kSmall, 2); // warm the allocator

    const double small_ms = fastest_preview_ms(kSmall, kRuns);
    const double large_ms = fastest_preview_ms(kLarge, kRuns);
    const double ratio = large_ms / small_ms;

    std::cout << "\n[connect scaling] decls " << kSmall << " -> " << kLarge << ": " << small_ms
              << " ms -> " << large_ms << " ms  ratio=" << ratio << "\n";

    // Linear in the 4x input is ~4.  The quadratic form this guards against was
    // ~16.  8 leaves room for a loaded runner without letting the product term
    // back in.
    CHECK(ratio < 8.0);
}

TEST_CASE("connect: a hierarchy path with stray dots resolves like its canonical spelling",
          "[connect]") {
    // resolve_instance_route() splits on '.' and drops empty segments, so these
    // spellings all name the same instance.  build_connect() used to keep the
    // caller's text and look the hierarchy map up with it, which missed, threw
    // std::out_of_range, and left the command answered with a bare null -- no
    // edits and no error to show the user.
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_stray_dots.sv";
    analyzer.open(uri, R"(
module producer(output logic [7:0] data);
endmodule
module consumer(input logic [7:0] data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto canonical = connect_apply_edit_json(analyzer, uri, "top.u_prod", "data",
                                                   "top.u_cons", "data", "data_w");
    REQUIRE(canonical.find(".data(data_w)") != std::string::npos);

    for (const auto* spelling : {"top..u_prod", "top.u_prod."}) {
        const auto edit = connect_apply_edit_json(analyzer, uri, spelling, "data",
                                                  "top.u_cons", "data", "data_w");
        INFO("source path spelled " << spelling);
        CHECK(edit == canonical);
    }

    // A path that names nothing still reports why, rather than throwing.
    const auto missing = connect_apply_edit_json(analyzer, uri, "top.u_nope", "data",
                                                 "top.u_cons", "data", "data_w");
    CHECK(missing.find("not found") != std::string::npos);
}
