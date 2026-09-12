#include "analyzer.hpp"
#include "features/rename.hpp"
#include <catch2/catch_test_macros.hpp>
#include <set>

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

// `MK_REG(status)` declares `status_reg` by token pasting.  The declaration's
// occurrence was anchored at the macro *argument* (`status`, 6 characters) but
// carried the length of the expanded name (10), so the edit ran past the end of
// the line and swallowed `);`.  No edit may rewrite a span whose current text is
// not the identifier being renamed; when one would, the rename is refused
// instead of corrupting the file.
TEST_CASE("rename: refuses to rewrite a macro-pasted declaration", "[rename]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_macro_paste.sv";
    const std::string text =
        "`define MK_REG(nm) logic [31:0] nm``_reg\n"
        "module top;\n"
        "  `MK_REG(status);\n"
        "  initial status_reg = 32'h1;\n"
        "endmodule\n";
    analyzer.open(uri, text);

    TextDocumentRename::Params params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(3, 10); // on `status_reg`
    params.newName = "stat2_reg";

    auto edit = provide_rename(analyzer, params);

    // Either no edits at all, or only edits whose current text really is the old
    // name.  What must never happen is an edit whose range covers other source.
    if (edit.changes.has_value()) {
        for (const auto& [edit_uri, edits] : *edit.changes) {
            CHECK(edit_uri == uri);
            for (const auto& e : edits) {
                REQUIRE(e.range.start.line == e.range.end.line);
                std::string line;
                {
                    size_t pos = 0;
                    for (int i = 0; i < e.range.start.line; ++i)
                        pos = text.find('\n', pos) + 1;
                    const size_t end = text.find('\n', pos);
                    line = text.substr(pos, end - pos);
                }
                REQUIRE(e.range.end.character <= (int)line.size());
                CHECK(line.substr(e.range.start.character,
                                  e.range.end.character - e.range.start.character) ==
                      "status_reg");
            }
        }
    }
}

// Renaming a module-level signal used to rewrite a same-named declaration inside
// a generate block — a different signal — and its uses, silently rewiring the
// block.

TEST_CASE("rename: genvar rewrites the loop header and body uses", "[rename][genvar]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_genvar.sv";
    analyzer.open(uri, R"(
module genvar_rename #(
    parameter int N = 4
) (
    input  logic [N-1:0] a,
    output logic [N-1:0] y
);
    genvar gi;
    generate
        for (gi = 0; gi < N; gi++) begin : g_lane
            assign y[gi] = a[gi];
        end
    endgenerate
endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(7, 11); // genvar gi;
    rename_params.newName = "lane_idx";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);

    // Declaration + three loop-header uses + two body uses.
    CHECK(edits.size() == 6);
    for (const auto& e : edits)
        CHECK(e.newText == "lane_idx");
}

TEST_CASE("rename: a depth-2 member chain is rewritten too", "[rename][nested-member]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_nested_member.sv";
    analyzer.open(uri, R"(
module nest_rename;
    typedef struct packed { logic [3:0] f; } in_t;
    typedef struct packed { in_t a; }        out_t;

    out_t o;
    in_t  i;
    logic [3:0] r1, r2;

    assign r1 = i.f;
    assign r2 = o.a.f;
endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(2, 40); // the `f` field declaration
    rename_params.newName = "f_r";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);

    // Declaration, `i.f`, and `o.a.f`.  Missing the last one leaves the design
    // referring to a field that no longer exists.
    REQUIRE(edits.size() == 3);
    std::set<std::pair<int, int>> touched;
    for (const auto& e : edits) {
        CHECK(e.newText == "f_r");
        touched.insert({e.range.start.line, e.range.start.character});
    }
    CHECK(touched.count({2, 40}) == 1);
    CHECK(touched.count({9, 18}) == 1);
    CHECK(touched.count({10, 20}) == 1);
}

// `.p,` is shorthand for `.p(p)` -- a port name and a same-spelled signal
// reference in one token.  A rename touches one of those two meanings, so it
// has to expand the shorthand: after the rename the two halves no longer spell
// alike.  Leaving the token alone silently rebinds the connection.
TEST_CASE("rename: an implicit port connection expands when the port is renamed",
          "[rename][implicit-port]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_implicit_port_fixture.sv";
    analyzer.open(uri, R"(module leaf(
    input logic clk_i
);
endmodule

module top;
    logic clk_i;
    leaf u_leaf (
        .clk_i
    );
endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(1, 16); // clk_i in leaf's port list
    rename_params.newName = "zzz_clk";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 2);
    CHECK(edits[0].range.start.line == 1);
    CHECK(edits[0].newText == "zzz_clk");
    // The port half is what changed, so the net keeps its own name.
    CHECK(edits[1].range.start.line == 8);
    CHECK(edits[1].newText == "zzz_clk(clk_i)");
}

TEST_CASE("rename: an implicit port connection expands when the net is renamed",
          "[rename][implicit-port]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_implicit_net_fixture.sv";
    analyzer.open(uri, R"(module leaf(
    input logic clk_i
);
endmodule

module top;
    logic clk_i;
    leaf u_leaf (
        .clk_i
    );
endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(6, 10); // top's own clk_i declaration
    rename_params.newName = "sys_clk";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 2);
    CHECK(edits[0].range.start.line == 6);
    CHECK(edits[0].newText == "sys_clk");
    // The net half is what changed; leaf's port keeps its name.
    CHECK(edits[1].range.start.line == 8);
    CHECK(edits[1].newText == "clk_i(sys_clk)");
}

TEST_CASE("rename: an explicit port connection is still renamed in place",
          "[rename][implicit-port]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_explicit_port_fixture.sv";
    analyzer.open(uri, R"(module leaf(
    input logic clk_i
);
endmodule

module top;
    logic other_clk;
    leaf u_leaf (
        .clk_i (other_clk)
    );
endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(1, 16); // clk_i in leaf's port list
    rename_params.newName = "zzz_clk";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 2);
    // `.p(expr)` already spells both halves, so it is a plain replacement.
    CHECK(edits[1].range.start.line == 8);
    CHECK(edits[1].newText == "zzz_clk");
}

TEST_CASE("rename: a wildcard port connection is left alone", "[rename][implicit-port]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_wildcard_port_fixture.sv";
    analyzer.open(uri, R"(module leaf(
    input logic clk_i
);
endmodule

module top;
    logic clk_i;
    leaf u_leaf (
        .*
    );
endmodule
)");

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(1, 16);
    rename_params.newName = "zzz_clk";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    const auto& edits = edit.changes->at(uri);
    // Only the declaration; `.*` is not a NamedPortConnectionSyntax and must
    // never be rewritten.
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].range.start.line == 1);
}
