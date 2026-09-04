#include "analyzer.hpp"
#include "features/rename.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("rename: prepares identifier range and edits all resolved references", "[rename]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_fixture.sv";
    analyzer.open(uri, R"(
module top;
    logic a;
    assign a = a;
endmodule
)");

    lsTextDocumentPositionParams prepare_params;
    prepare_params.textDocument.uri.raw_uri_ = uri;
    prepare_params.position = lsPosition(3, 11);
    auto prepared = prepare_rename(analyzer, prepare_params);
    REQUIRE(prepared.has_value());
    CHECK(prepared->placeholder == "a");
    CHECK(prepared->range.start.line == 3);
    CHECK(prepared->range.start.character == 11);

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(3, 11);
    rename_params.newName = "next_a";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 3);
    CHECK(edits[0].range.start.line == 2);
    CHECK(edits[0].newText == "next_a");
    CHECK(edits[1].range.start.line == 3);
    CHECK(edits[2].range.start.line == 3);
}

TEST_CASE("rename: port declaration renames body usages", "[rename]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_port_fixture.sv";
    analyzer.open(uri, R"(module foo(
    input i_clk
);

always_ff @(posedge i_clk) begin
end

endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(1, 10); // i_clk in the port list
    rename_params.newName = "i_clock";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 2);
    CHECK(edits[0].range.start.line == 1);
    CHECK(edits[0].range.start.character == 10);
    CHECK(edits[1].range.start.line == 4);
    CHECK(edits[1].range.start.character == 20);
    CHECK(edits[1].newText == "i_clock");
}

TEST_CASE("rename: port declaration renames instance connection expressions", "[rename]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_port_conn_fixture.sv";
    analyzer.open(uri, R"(module memory(
    input logic i_data
);
endmodule

module memory_top(
    input logic i_data
);

memory u_mem (
    .i_data (i_data)
);

endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(6, 16); // i_data in memory_top's port list
    rename_params.newName = "i_payload";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 2);
    CHECK(edits[0].range.start.line == 6);
    // The connected expression, not the `.i_data` port name, which belongs to
    // module memory and must keep its own identity.
    CHECK(edits[1].range.start.line == 10);
    CHECK(edits[1].range.start.character == 13);
}

TEST_CASE("rename: body usage renames port declaration and named connections", "[rename]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_port_from_body_fixture.sv";
    analyzer.open(uri, R"(module memory(
    input logic i_data
);
    logic local_copy;
    assign local_copy = i_data;
endmodule

module memory_top;
memory u_mem (
    .i_data (1'b0)
);
endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(4, 24); // i_data inside memory's body
    rename_params.newName = "i_payload";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 3);
    CHECK(edits[0].range.start.line == 1);  // declaration
    CHECK(edits[1].range.start.line == 4);  // body usage
    CHECK(edits[2].range.start.line == 9);  // .i_data named connection
}

// Renaming a module-level signal used to rewrite a same-named declaration inside
// a generate block — a different signal — and its uses, silently rewiring the
// block.
TEST_CASE("rename: a module-level signal does not rewrite a generate-block declaration",
          "[rename]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_generate_shadow.sv";
    analyzer.open(uri, R"(module top;
  logic [7:0] dout;
  assign dout = 8'h0;

  genvar i;
  generate
    for (i = 0; i < 2; i++) begin : g_lanes
      logic [7:0] dout;
      assign dout = 8'h1;
    end
  endgenerate
endmodule
)");

    TextDocumentRename::Params params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(1, 14); // module-level declaration
    params.newName = "dout_o";

    auto edit = provide_rename(analyzer, params);
    REQUIRE(edit.changes.has_value());
    const auto& edits = edit.changes->at(uri);
    CHECK(edits.size() == 2);
    for (const auto& e : edits)
        CHECK(e.range.start.line < 7);
}
