// Regressions found by stress-formatting macro-heavy, UVM, Verilog-95,
// OpenTitan and CIRCT-style RTL.  Every case asserts the exact output and
// that formatting it again changes nothing; cases that once produced code
// slang rejects also parse the output.
#include "features/formatter.hpp"
#include "config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/syntax/SyntaxTree.h>
#include <algorithm>
#include <regex>
#include <string>

namespace {

std::string format_stable(const std::string& input, const FormatOptions& opts = FormatOptions{}) {
    const std::string once = format_source(input, opts);
    const std::string twice = format_source(once, opts);
    INFO("first pass:\n" << once);
    CHECK(twice == once);
    return once;
}

bool parses_cleanly(const std::string& text) {
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    for (const auto& diag : tree->diagnostics()) {
        if (diag.isError())
            return false;
    }
    return true;
}

FormatOptions indent4() {
    FormatOptions opts;
    opts.indent_size = 4;
    return opts;
}

} // namespace

TEST_CASE("formatter regression: wildcard digits stay inside their literal", "[formatter][regression]") {
    const std::string input = "module t(input logic [3:0] d, input logic c, output logic [3:0] y);\n"
                              "always_comb begin\n"
                              "casez (d)\n"
                              "4'b1???: y = 4'b1?0?;\n"
                              "4'b01?? : y = c ? a : b;\n"
                              "4'bz?x?: y = 8'h?F;\n"
                              "default: y = (d ==? 4'b1??0) ? 'b? : '0;\n"
                              "endcase\n"
                              "end\n"
                              "endmodule\n";
    const std::string out = format_stable(input, indent4());
    INFO("formatted:\n" << out);
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(out));
    CHECK(out.find("4'b1???: y = 4'b1?0?;") != std::string::npos);
    CHECK(out.find("4'b01??: y = c ? a : b;") != std::string::npos);
    CHECK(out.find("4'bz?x?: y = 8'h?F;") != std::string::npos);
    CHECK(out.find("(d ==? 4'b1??0) ? 'b? : '0;") != std::string::npos);
}

TEST_CASE("formatter regression: a conditional's colon after a number keeps its space", "[formatter][regression]") {
    const std::string input = "module t;\n"
                              "always_comb begin\n"
                              "case (s)\n"
                              "2'd0: y = c ? 4'd1 : 4'd2;\n"
                              "1 : y = c ? 1.5 : 2;\n"
                              "endcase\n"
                              "end\n"
                              "assign z = c ? 8'hff : 0;\n"
                              "endmodule\n";
    const std::string out = format_stable(input, indent4());
    CHECK(out == "module t;\n"
                 "    always_comb begin\n"
                 "        case (s)\n"
                 "            2'd0: y = c ? 4'd1 : 4'd2;\n"
                 "            1: y = c ? 1.5 : 2;\n"
                 "        endcase\n"
                 "    end\n"
                 "    assign z = c ? 8'hff : 0;\n"
                 "endmodule\n");
}

TEST_CASE("formatter regression: code after a multi-line block comment gets no phantom blank line", "[formatter][regression]") {
    const std::string input = "module t;\n"
                              "wire w = a; // trailing\n"
                              "/* block\n"
                              "   comment */ reg k;\n"
                              "/* another\n"
                              "   one */\n"
                              "reg j;\n"
                              "endmodule\n";
    const std::string out = format_stable(input, indent4());
    CHECK(out == "module t;\n"
                 "    wire w = a; // trailing\n"
                 // The comment's second line keeps its place under the
                 // first, which moved four columns (R-8).
                 "    /* block\n"
                 "       comment */\n"
                 "    reg k;\n"
                 "    /* another\n"
                 "       one */\n"
                 "    reg j;\n"
                 "endmodule\n");
}

TEST_CASE("formatter regression: semicolonless macro items each start a line", "[formatter][regression]") {
    // OpenTitan style, no [format.macros] entries.  These used to render as
    // one line ending in `always_comb begin`.
    const std::string input = "module m;\n"
                              "logic q;\n"
                              "`PRIM_FLOP_A(d, q, 0) `ASSERT_INIT(P_A, N > 0)\n"
                              "`ASSERT(C_A, q |=> d)\n"
                              "prim_x u_x (.a(q));\n"
                              "`ASSERT_KNOWN(K_A, q) logic z;\n"
                              "always_comb begin\n"
                              "z = 0;\n"
                              "end\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "module m;\n"
                                  "  logic q;\n"
                                  "  `PRIM_FLOP_A(d, q, 0)\n"
                                  "  `ASSERT_INIT(P_A, N > 0)\n"
                                  "  `ASSERT(C_A, q |=> d)\n"
                                  "  prim_x u_x(\n"
                                  "    .a(q)\n"
                                  "  );\n"
                                  "  `ASSERT_KNOWN(K_A, q)\n"
                                  "  logic z;\n"
                                  "  always_comb begin\n"
                                  "    z = 0;\n"
                                  "  end\n"
                                  "endmodule\n");
}

TEST_CASE("formatter regression: a semicolonless macro statement ends its statement", "[formatter][regression]") {
    // The `else`, the statement after the `foreach` body and the case labels
    // after a macro item all used to be claimed by the macro's statement,
    // which then ran on to the next `;`.
    const std::string input = "module m;\n"
                              "initial begin\n"
                              "if (c) `NOP\n"
                              "else x = 0;\n"
                              "foreach (m[k]) `CHECK_EQ(a, 0)\n"
                              "y = 1;\n"
                              "`uvm_info(\"a\", \"b\", UVM_LOW)\n"
                              "foo = 1;\n"
                              "case (w)\n"
                              "2'd0: `NOP\n"
                              "2'd1: `CHECK(o == 0)\n"
                              "`MAX(1, 2) : o = 3;\n"
                              "ST_A: `CHECK(1)\n"
                              "default: ;\n"
                              "endcase\n"
                              "end\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "module m;\n"
                                  "  initial begin\n"
                                  "    if (c)\n"
                                  "      `NOP\n"
                                  "    else\n"
                                  "      x = 0;\n"
                                  "    foreach (m[k])\n"
                                  "      `CHECK_EQ(a, 0)\n"
                                  "    y = 1;\n"
                                  "    `uvm_info(\"a\", \"b\", UVM_LOW)\n"
                                  "    foo = 1;\n"
                                  "    case (w)\n"
                                  "      2'd0: `NOP\n"
                                  "      2'd1: `CHECK(o == 0)\n"
                                  "      `MAX(1, 2): o = 3;\n"
                                  "      ST_A: `CHECK(1)\n"
                                  "      default:;\n"
                                  "    endcase\n"
                                  "  end\n"
                                  "endmodule\n");
}

TEST_CASE("formatter regression: macros that are part of a statement stay on its line", "[formatter][regression]") {
    const std::string input = "module m;\n"
                              "`T_DATA d0, d1;\n"
                              "`MY_ATTR logic a;\n"
                              "`MY_T(8) sig;\n"
                              "assign x = `A_ARGS(1, 2);\n"
                              "initial begin\n"
                              "`REG = 2;\n"
                              "`ARR[0] <= 3;\n"
                              "`FIELD(1).f = 4;\n"
                              "`LOG_INFO((\"x\"));\n"
                              "end\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "module m;\n"
                                  "  `T_DATA d0, d1;\n"
                                  "  `MY_ATTR logic a;\n"
                                  "  `MY_T(8) sig;\n"
                                  "  assign x = `A_ARGS(1, 2);\n"
                                  "  initial begin\n"
                                  "    `REG = 2;\n"
                                  "    `ARR[0] <= 3;\n"
                                  "    `FIELD(1).f = 4;\n"
                                  "    `LOG_INFO((\"x\"));\n"
                                  "  end\n"
                                  "endmodule\n");
}

TEST_CASE("formatter regression: a macro configured as an expression never ends a statement", "[formatter][regression]") {
    FormatOptions opts;
    opts.macros.object_like_expr.push_back("PREFIX");
    opts.macros.function_like_expr.push_back("WRAP");
    const std::string input = "module m;\n"
                              "`WRAP(a) always_comb x = 0;\n"
                              "initial begin\n"
                              "`PREFIX x = 1;\n"
                              "end\n"
                              "endmodule\n";
    const std::string out = format_stable(input, opts);
    CHECK(out.find("`WRAP(a) always_comb") != std::string::npos);
    CHECK(out.find("`PREFIX x = 1;") != std::string::npos);
}

TEST_CASE("formatter regression: a multi-line define keeps its configured role", "[formatter][regression]") {
    // A macro whose `define spans lines is whitespace sensitive; that used to
    // skip role classification, so `declaration_like` had no effect.
    // A bare macro before `logic` is a prefix unless configured otherwise, and
    // only a role can say a macro opens a block -- so both lines below are
    // decided by [format.macros] alone.
    FormatOptions opts;
    opts.macros.declaration_like.push_back("TAG");
    opts.macros.block_begin_like.push_back("BLK_BEGIN");
    opts.macros.block_end_like.push_back("BLK_END");
    const std::string input = "`define TAG \\\n"
                              "  logic tag_q;\n"
                              "`define BLK_BEGIN(n) \\\n"
                              "  begin : n\n"
                              "`define BLK_END \\\n"
                              "  end\n"
                              "module m;\n"
                              "`TAG logic z;\n"
                              "always_comb `BLK_BEGIN(b)\n"
                              "x = 0;\n"
                              "`BLK_END\n"
                              "endmodule\n";
    const std::string out = format_stable(input, opts);
    CHECK(out.find("module m;\n"
                   "  `TAG\n"
                   "  logic z;\n"
                   "  always_comb\n"
                   "    `BLK_BEGIN(b)\n"
                   "      x = 0;\n"
                   "    `BLK_END\n") != std::string::npos);
}

TEST_CASE("formatter regression: nested single-statement controls release every level", "[formatter][regression]") {
    const std::string input = "package k;\n"
                              "function automatic int f(input int x);\n"
                              "for (int i = 0; i < 4; i++) if (x[i]) return i;\n"
                              "for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (x[j]) y = 1; else y = 2;\n"
                              "return 0;\n"
                              "endfunction\n"
                              "endpackage\n"
                              "module after_pkg;\n"
                              "always if (a) if (b) x = 1;\n"
                              "initial forever #5 clk = ~clk;\n"
                              "logic z;\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "package k;\n"
                                  "  function automatic int f(input int x);\n"
                                  "    for (int i = 0; i < 4; i++)\n"
                                  "      if (x[i])\n"
                                  "        return i;\n"
                                  "    for (int i = 0; i < 4; i++)\n"
                                  "      for (int j = 0; j < 4; j++)\n"
                                  "        if (x[j])\n"
                                  "          y = 1;\n"
                                  "        else\n"
                                  "          y = 2;\n"
                                  "    return 0;\n"
                                  "  endfunction\n"
                                  "endpackage\n"
                                  "module after_pkg;\n"
                                  "  always\n"
                                  "    if (a)\n"
                                  "      if (b)\n"
                                  "        x = 1;\n"
                                  "  initial\n"
                                  "    forever\n"
                                  "      #5 clk = ~clk;\n"
                                  "  logic z;\n"
                                  "endmodule\n");
}

TEST_CASE("formatter regression: a design unit cannot inherit a leaked level", "[formatter][regression]") {
    // Whatever goes wrong inside one unit, the next starts at column 0.
    const std::string input = "module a;\n"
                              "initial if (x) `NOP\n"
                              "endmodule\n"
                              "module b;\n"
                              "logic z;\n"
                              "endmodule\n";
    const std::string out = format_stable(input);
    CHECK(out.find("endmodule\nmodule b;\n  logic z;\nendmodule\n") != std::string::npos);
}

TEST_CASE("formatter regression: property, sequence and clocking references open no scope", "[formatter][regression]") {
    const std::string input = "interface i(input clk);\n"
                              "logic v, a, b;\n"
                              "clocking cb @(posedge clk); input v; output a, b; endclocking\n"
                              "default clocking cb;\n"
                              "modport m (input v, clocking cb);\n"
                              "a_x: assert property (@(posedge clk) a |-> b) else $error(\"x\");\n"
                              "c_y: cover sequence (@(posedge clk) a ##1 b);\n"
                              "property p; @(posedge clk) a |=> b; endproperty\n"
                              "sequence s; a ##1 b; endsequence\n"
                              "logic z;\n"
                              "endinterface\n";
    const std::string out = format_stable(input);
    INFO("formatted:\n" << out);
    CHECK(out.find("  clocking cb @(posedge clk);\n"
                   "    input v;\n"
                   "    output a, b;\n"
                   "  endclocking\n"
                   "  default clocking cb;\n"
                   "  modport m (") != std::string::npos);
    CHECK(out.find("\n  property p;\n    @(posedge clk) a |=> b;\n  endproperty\n") != std::string::npos);
    CHECK(out.find("\n  sequence s;\n") != std::string::npos);
    CHECK(out.find("\n  logic z;\nendinterface\n") != std::string::npos);
}

TEST_CASE("formatter regression: randcase and randsequence open their own scope", "[formatter][regression]") {
    const std::string input = "module m;\n"
                              "initial begin\n"
                              "randcase 1: a = 0;\n"
                              "3: a = 1;\n"
                              "endcase\n"
                              "randsequence (main)\n"
                              "main : first second;\n"
                              "first : { a = 1; };\n"
                              "second : { a = 2; };\n"
                              "endsequence\n"
                              "b = 1;\n"
                              "end\n"
                              "final begin b = 2; end\n"
                              "endmodule\n";
    const std::string out = format_stable(input);
    INFO("formatted:\n" << out);
    CHECK(out.find("  initial begin\n"
                   "    randcase\n"
                   "      1: a = 0;\n"
                   "      3: a = 1;\n"
                   "    endcase\n"
                   "    randsequence (main)\n") != std::string::npos);
    CHECK(out.find("      main: first second;\n"
                   "      first: {\n"
                   "        a = 1;\n"
                   "      };\n") != std::string::npos);
    CHECK(out.find("\n    endsequence\n"
                   "    b = 1;\n"
                   "  end\n"
                   "  final begin\n") != std::string::npos);
}

TEST_CASE("formatter regression: a conditional directive inside a dimension ends with its operand", "[formatter][regression]") {
    const std::string input = "module m(\n"
                              "input clk,\n"
                              "`ifdef HAS_RESET\n"
                              "input rst_n,\n"
                              "`endif // HAS_RESET\n"
                              "output q\n"
                              ");\n"
                              "reg [`ifdef WIDE 63 `else 31 `endif :0] r;\n"
                              "always @* begin\n"
                              "case (r)\n"
                              "2'd1: q = r+1;\n"
                              "endcase\n"
                              "end\n"
                              "endmodule\n";
    const std::string out = format_stable(input);
    INFO("formatted:\n" << out);
    CHECK(parses_cleanly(out));
    CHECK(out.find("  reg [`ifdef WIDE 63 `else 31 `endif :0] r;\n"
                   "  always @* begin\n"
                   "    case (r)\n"
                   "      2'd1: q = r + 1;\n") != std::string::npos);
    CHECK(out.find("\n`ifdef HAS_RESET\n") != std::string::npos);
    CHECK(out.find("\n`endif // HAS_RESET\n") != std::string::npos);
}

TEST_CASE("formatter regression: a case label starting with a brace is a label, not a block name", "[formatter][regression]") {
    const std::string input = "module m;\n"
                              "always_comb begin : blk\n"
                              "case (x)\n"
                              "{a, b}: y = 0;\n"
                              "{2'b0, 2'b11} : y <= c ? {a} : {b};\n"
                              "{a, b}, c: y = 1;\n"
                              "endcase\n"
                              "end : blk\n"
                              "endmodule : m\n";
    CHECK(format_stable(input) == "module m;\n"
                                  "  always_comb begin: blk\n"
                                  "    case (x)\n"
                                  "      {a, b}: y = 0;\n"
                                  "      {2'b0, 2'b11}: y <= c ? {a} : {b};\n"
                                  "      {a, b}, c: y = 1;\n"
                                  "    endcase\n"
                                  "  end: blk\n"
                                  "endmodule: m\n");
}

TEST_CASE("formatter regression: a fork keeps its label", "[formatter][regression]") {
    const std::string input = "module m;\n"
                              "initial begin\n"
                              "fork : watchdog\n"
                              "begin #100; $fatal(1, \"timeout\"); end\n"
                              "join_none : watchdog\n"
                              "fork a(); b(); join\n"
                              "disable fork;\n"
                              "end\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "module m;\n"
                                  "  initial begin\n"
                                  "    fork: watchdog\n"
                                  "      begin\n"
                                  "        #100;\n"
                                  "        $fatal(1, \"timeout\");\n"
                                  "      end\n"
                                  "    join_none: watchdog\n"
                                  "    fork\n"
                                  "      a();\n"
                                  "      b();\n"
                                  "    join\n"
                                  "    disable fork;\n"
                                  "  end\n"
                                  "endmodule\n");
}

TEST_CASE("formatter regression: indexes inside concatenations and calls stay closed up", "[formatter][regression]") {
    const std::string input = "module m(input logic a [4], input logic [1:0] b);\n"
                              "localparam int N = 2;\n"
                              "wire [7:0] w = {4{a[0], b[0]}};\n"
                              "wire [7:0] x = {(N){b}, {2'd2{a[1]}}};\n"
                              "logic [7:0] mem [4];\n"
                              "my_t arr [2];\n"
                              "assign req = '{id: 0, addr: {mem[1], 24'h0}};\n"
                              "initial `CHECK_EQ(mem[1], 0)\n"
                              "endmodule\n";
    const std::string out = format_stable(input);
    INFO("formatted:\n" << out);
    CHECK(out.find("wire [7:0] w = {4{a[0], b[0]}};") != std::string::npos);
    CHECK(out.find("wire [7:0] x = {(N){b}, {2'd2{a[1]}}};") != std::string::npos);
    CHECK(out.find("logic [7:0] mem [4];") != std::string::npos);
    CHECK(out.find("my_t arr [2];") != std::string::npos);
    CHECK(out.find("{mem[1], 24'h0}") != std::string::npos);
    CHECK(out.find("`CHECK_EQ(mem[1], 0)") != std::string::npos);
    // An ANSI port's unpacked dimension is still a declaration dimension.
    CHECK(std::regex_search(out, std::regex("input +logic +a +\\[4\\] *,")));
}

TEST_CASE("formatter regression: only an identifier names an instance", "[formatter][regression]") {
    FormatOptions opts;
    opts.macros.object_like_expr.push_back("PREFIX");
    const std::string input = "module m;\n"
                              "`T_DATA `CAT(d, 2);\n"
                              "`MOD_NAME u_c (.a(b));\n"
                              "initial begin\n"
                              "`PREFIX $display(\"x\");\n"
                              "end\n"
                              "endmodule\n";
    CHECK(format_stable(input, opts) == "module m;\n"
                                        "  `T_DATA `CAT(d, 2);\n"
                                        "  `MOD_NAME u_c(\n"
                                        "    .a(b)\n"
                                        "  );\n"
                                        "  initial begin\n"
                                        "    `PREFIX $display(\"x\");\n"
                                        "  end\n"
                                        "endmodule\n");
}

TEST_CASE("formatter regression: statement alignment measures case labels as rendered", "[formatter][regression]") {
    const std::string input = "module m;\n"
                              "always_comb begin\n"
                              "case (s)\n"
                              "4'h1: y = 5;\n"
                              "4'h12: yy = 6;\n"
                              "default: y_long = 7;\n"
                              "endcase\n"
                              "end\n"
                              "endmodule\n";
    FormatOptions opts;
    opts.statement.align = true;
    opts.statement.lhs_min_width = 0;

    SECTION("shared column") {
        opts.statement.align_adaptive = false;
        CHECK(format_stable(input, opts).find("      4'h1: y         = 5;\n"
                                              "      4'h12: yy       = 6;\n"
                                              "      default: y_long = 7;\n") != std::string::npos);
    }
    SECTION("adaptive") {
        opts.statement.align_adaptive = true;
        CHECK(format_stable(input, opts).find("      4'h1: y = 5;\n"
                                              "      4'h12: yy = 6;\n"
                                              "      default: y_long = 7;\n") != std::string::npos);
    }
}

TEST_CASE("formatter regression: a delay after a semicolon starts its own line", "[formatter][regression]") {
    const std::string input = "module d;\n"
                              "initial begin\n"
                              "@(posedge clk); #1 a = 1;\n"
                              "b = 2; #2;\n"
                              "`DISPLAY(\"s\"); #1 `DISPLAY(\"t\");\n"
                              "end\n"
                              "endmodule\n"
                              "module e import p::*; #(parameter int W = 1) (input logic c);\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "module d;\n"
                                  "  initial begin\n"
                                  "    @(posedge clk);\n"
                                  "    #1 a = 1;\n"
                                  "    b = 2;\n"
                                  "    #2;\n"
                                  "    `DISPLAY(\"s\");\n"
                                  "    #1 `DISPLAY(\"t\");\n"
                                  "  end\n"
                                  "endmodule\n"
                                  "module e\n"
                                  "  import p::*;\n"
                                  "#(\n"
                                  "  parameter int W = 1\n"
                                  ")(\n"
                                  "  input logic c\n"
                                  ");\n"
                                  "endmodule\n");
}

TEST_CASE("formatter regression: a name after a port declaration stays on its line", "[formatter][regression]") {
    FormatOptions opts;
    opts.port_declaration.align = false;
    const std::string input = "module m (input wire clk, input wire [7:0] a, b, // two\n"
                              " input c, d = 1'b0, output logic y);\n"
                              "endmodule\n"
                              "interface i;\n"
                              "logic valid, data, ready;\n"
                              "modport mst (output valid, data, input ready);\n"
                              "endinterface\n"
                              "module n (a, b, c);\n"
                              "endmodule\n";
    CHECK(format_stable(input, opts) == "module m(\n"
                                        "  input wire clk,\n"
                                        "  input wire [7:0] a, b, // two\n"
                                        "  input c, d = 1'b0,\n"
                                        "  output logic y\n"
                                        ");\n"
                                        "endmodule\n"
                                        "interface i;\n"
                                        "  logic valid, data, ready;\n"
                                        "  modport mst (\n"
                                        "    output valid, data,\n"
                                        "    input ready\n"
                                        "  );\n"
                                        "endinterface\n"
                                        "module n(\n"
                                        "  a,\n"
                                        "  b,\n"
                                        "  c\n"
                                        ");\n"
                                        "endmodule\n");
}

TEST_CASE("formatter regression: each ifdef branch of a port list is its own item", "[formatter][regression]") {
    const std::string input = "module b (\n"
                              "`ifdef WIDE\n"
                              "output [63:0] q\n"
                              "`else\n"
                              "output [31:0] q\n"
                              "`endif\n"
                              ");\n"
                              "endmodule\n";
    FormatOptions opts;
    opts.port_declaration.align = false;
    CHECK(format_stable(input, opts) == "module b(\n"
                                        "`ifdef WIDE\n"
                                        "  output [63:0] q\n"
                                        "`else\n"
                                        "  output [31:0] q\n"
                                        "`endif\n"
                                        ");\n"
                                        "endmodule\n");

    // Aligned, both branches put `q` in the same column.
    FormatOptions align_on;
    align_on.port_declaration.align = true;
    const std::string aligned = format_stable(input, align_on);
    const size_t first = aligned.find("[63:0]");
    const size_t second = aligned.find("[31:0]");
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    const size_t first_line = aligned.rfind('\n', first);
    const size_t second_line = aligned.rfind('\n', second);
    CHECK(aligned.find('q', first) - first_line == aligned.find('q', second) - second_line);
    CHECK(aligned.find("\n`else\n") != std::string::npos);
}

TEST_CASE("formatter regression: generate and case-inside headers end where they end", "[formatter][regression]") {
    const std::string input = "module g #(parameter P = 1);\n"
                              "generate case (P)\n"
                              "0: begin : a\n"
                              "logic x;\n"
                              "end\n"
                              "default: begin : b\n"
                              "logic y;\n"
                              "end\n"
                              "endcase endgenerate\n"
                              "always_comb begin\n"
                              "priority case (s) inside\n"
                              "[0:1]: y = 0;\n"
                              "default: y = 1;\n"
                              "endcase\n"
                              "end\n"
                              "endmodule\n";
    const std::string out = format_stable(input);
    INFO("formatted:\n" << out);
    CHECK(out.find("  generate\n"
                   "    case (P)\n"
                   "      0: begin: a\n"
                   "        logic x;\n"
                   "      end\n") != std::string::npos);
    CHECK(out.find("    endcase\n"
                   "  endgenerate\n") != std::string::npos);
    CHECK(out.find("    priority case (s) inside\n"
                   "      [0:1]: y = 0;\n"
                   "      default: y = 1;\n") != std::string::npos);
}

TEST_CASE("formatter regression: cycle delays, property events and labels space tightly", "[formatter][regression]") {
    const std::string input = "module p(input clk, a, b);\n"
                              "a_x : assert property (@(posedge clk) a |-> b) else $error(\"x\");\n"
                              "c_y : cover property (@(posedge clk) a ##1 b ##[1:3] a);\n"
                              "covergroup cg @(posedge clk);\n"
                              "cp_a : coverpoint a;\n"
                              "cp_b: coverpoint b;\n"
                              "x : cross cp_a, cp_b;\n"
                              "endgroup\n"
                              "endmodule\n";
    const std::string out = format_stable(input);
    INFO("formatted:\n" << out);
    CHECK(out.find("  a_x: assert property (@(posedge clk) a |-> b)\n") != std::string::npos);
    CHECK(out.find("  c_y: cover property (@(posedge clk) a ##1 b ##[1:3] a);\n") != std::string::npos);
    CHECK(out.find("    cp_a: coverpoint a;\n"
                   "    cp_b: coverpoint b;\n"
                   "    x: cross cp_a, cp_b;\n") != std::string::npos);
}

TEST_CASE("formatter regression: a format-off marker keeps its indent", "[formatter][regression]") {
    const std::string input = "module m;\n"
                              "  // verilog_format: off\n"
                              "  localparam logic [3:0] LUT [4] = '{4'h1, 4'h2,\n"
                              "                                     4'h4, 4'h8};\n"
                              "  // verilog_format: on\n"
                              "logic z;\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "module m;\n"
                                  "  // verilog_format: off\n"
                                  "  localparam logic [3:0] LUT [4] = '{4'h1, 4'h2,\n"
                                  "                                     4'h4, 4'h8};\n"
                                  "  // verilog_format: on\n"
                                  "  logic z;\n"
                                  "endmodule\n");
}

TEST_CASE("formatter regression: port alignment is opt-in with modest adaptive columns", "[formatter][regression]") {
    const std::string input = "module m (input logic clk, input logic [7:0] data, output logic valid);\n"
                              "endmodule\n";
    CHECK(format_stable(input) == "module m(\n"
                                  "  input logic clk,\n"
                                  "  input logic [7:0] data,\n"
                                  "  output logic valid\n"
                                  ");\n"
                                  "endmodule\n");

    FormatOptions opts;
    opts.port_declaration.align = true;
    // Five 12-column sections; the old 10/20/20/30/30 put the comma past
    // column 110.
    CHECK(format_stable(input, opts) == "module m(\n"
                                        "  input       logic                   clk                     ,\n"
                                        "  input       logic       [7:0]       data                    ,\n"
                                        "  output      logic                   valid\n"
                                        ");\n"
                                        "endmodule\n");
}

TEST_CASE("formatter regression: a spaced conditional after a literal stays a conditional", "[formatter][regression]") {
    const std::string out = format_stable("assign y = 4'hc ? a : b;\n");
    CHECK(out == "assign y = 4'hc ? a : b;\n");
}

TEST_CASE("formatter regression: prototypes and interface classes open no indent scope", "[formatter][regression]") {
    const std::string input = R"SV(package dpi_pkg;
  export "DPI-C" function sv_get_status;
  import "DPI-C" context function int c_step(input int n);
  function int sv_get_status();
    return 0;
  endfunction
  interface class resettable;
    pure virtual function void reset();
  endclass
  virtual class base_driver implements resettable;
    function new();
    endfunction : new
    pure virtual task drive(input int n);
    virtual function void reset();
    endfunction
  endclass
endpackage

module after_pkg;
endmodule
)SV";
    const std::string expected = R"SV(package dpi_pkg;
  export "DPI-C" function sv_get_status;
  import "DPI-C" context function int c_step(input int n);
  function int sv_get_status();
    return 0;
  endfunction
  interface class resettable;
    pure virtual function void reset();
  endclass
  virtual class base_driver implements resettable;
    function new();
    endfunction: new
    pure virtual task drive(input int n);
    virtual function void reset();
    endfunction
  endclass
endpackage

module after_pkg;
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: specify paths are not a port list", "[formatter][regression]") {
    const std::string input = R"SV(module dff_timing (input d, clk, output q);
  specify
    specparam tCQ = 1.2;
    (clk => q) = (tCQ, tCQ);
    (posedge clk => (q +: d)) = 1.0;
    $setup(d, posedge clk, 0.5);
  endspecify
endmodule
)SV";
    const std::string expected = R"SV(module dff_timing(
  input d, clk,
  output q
);
  specify
    specparam tCQ = 1.2;
    (clk => q) = (tCQ, tCQ);
    (posedge clk => (q +: d)) = 1.0;
    $setup(d, posedge clk, 0.5);
  endspecify
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: UDP table rows keep their columns and endtable its level", "[formatter][regression]") {
    const std::string input = R"SV(primitive udp_dff (q, d, clk);
  output q; reg q;
  input d, clk;
  // d  clk  : q : q+
  table
     0  (01) : ? : 0 ;
     1  (01) : ? : 1 ;
     ?  (1?) : ? : - ;
  endtable
endprimitive
)SV";
    const std::string expected = R"SV(primitive udp_dff(q, d, clk);
  output q;
  reg q;
  input d, clk;
  // d  clk  : q : q+
  table
    0  (01) : ? : 0;
    1  (01) : ? : 1;
    ?  (1?) : ? : -;
  endtable
endprimitive
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: modport port expressions and prototypes stay inline", "[formatter][regression]") {
    const std::string input = R"SV(interface bus_if;
  logic [31:0] addr, data;
  modport mon (input .a(addr), .d(data));
  modport drv (output addr, import task send(input int n));
  task send(input int n); endtask
endinterface
)SV";
    const std::string expected = R"SV(interface bus_if;
  logic [31:0] addr, data;
  modport mon (
    input .a(addr), .d(data)
  );
  modport drv (
    output addr,
    import task send(input int n)
  );
  task send(input int n);
  endtask
endinterface
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: if-else and case inside a property stay property expressions", "[formatter][regression]") {
    const std::string input = R"SV(module req_checks (input logic clk, req, gnt, busy);
  a_resp: assert property (@(posedge clk) if (req) gnt else !busy);
  a_sel: assert property (@(posedge clk) case (busy) 1'b0: !gnt; default: 1; endcase);
endmodule
)SV";
    const std::string expected = R"SV(module req_checks(
  input logic clk, req, gnt, busy
);
  a_resp: assert property (@(posedge clk) if (req) gnt else !busy);
  a_sel: assert property (@(posedge clk) case (busy) 1'b0: !gnt; default: 1; endcase);
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a do-while with a single-statement body keeps its while", "[formatter][regression]") {
    const std::string input = R"SV(module tb;
  int n;
  initial begin
    do n++; while (n < 10);
    $display("n=%0d", n);
  end
endmodule
)SV";
    const std::string expected = R"SV(module tb;
  int n;
  initial begin
    do
      n++;
    while (n < 10);
    $display("n=%0d", n);
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: nested do-while bodies and a do-while as an if body", "[formatter][regression]") {
    const std::string input = R"SV(module m;
  int n;
  initial begin
    do begin n++; end while (n < 5);
    do do n--; while (n > 2); while (n > 0);
    if (n) do n++; while (n < 9); else n = 0;
    n = 1;
  end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  int n;
  initial begin
    do begin
      n++;
    end while (n < 5);
    do
      do
        n--;
      while (n > 2);
    while (n > 0);
    if (n)
      do
        n++;
      while (n < 9);
    else
      n = 0;
    n = 1;
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: brace-less bodies inside a constraint are indented", "[formatter][regression]") {
    const std::string input = R"SV(class pkt;
  rand int len, kind;
  rand int payload[4];
  constraint c_shape {
    if (kind == 0) len < 4; else { len >= 4; }
    foreach (payload[i]) payload[i] > 0;
  }
endclass
)SV";
    const std::string expected = R"SV(class pkt;
  rand int len, kind;
  rand int payload [4];
  constraint c_shape {
    if (kind == 0)
      len < 4;
    else {
      len >= 4;
    }
    foreach (payload[i])
      payload[i] > 0;
  }
endclass
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a constraint whose only item is a braced block still indents", "[formatter][regression]") {
    const std::string input = R"SV(class c;
  rand int q[4]; int N;
  constraint a { foreach (q[i]) { i < N -> q[i] != 0; } }
  constraint b { foreach (q[i]) { q[i] != 0; } }
  constraint d { foreach (q[i]) { i -> q[i] != 0; } }
  constraint e { foreach (q[i]) { i < N; } }
endclass
)SV";
    const std::string expected = R"SV(class c;
  rand int q [4];
  int N;
  constraint a {
    foreach (q[i]) {
      i < N -> q[i] != 0;
    }
  }
  constraint b {
    foreach (q[i]) {
      q[i] != 0;
    }
  }
  constraint d {
    foreach (q[i]) {
      i -> q[i] != 0;
    }
  }
  constraint e {
    foreach (q[i]) {
      i < N;
    }
  }
endclass
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: an indexed name after a control header or pattern key is not a declarator", "[formatter][regression]") {
    const std::string input = R"SV(module clr (input logic [31:0] a, input logic sel);
  typedef struct packed { logic [15:0] hi, lo; } pair_t;
  logic [7:0] mem [4];
  pair_t p;
  always_comb begin
    foreach (mem[i]) mem[i] = '0;
    if (sel) mem[0] = a[7:0];
    p = '{hi: a[31:16], lo: a[15:0]};
  end
endmodule
)SV";
    const std::string expected = R"SV(module clr(
  input logic [31:0] a,
  input logic sel
);
  typedef struct packed {
    logic [15:0] hi, lo;
  } pair_t;
  logic [7:0] mem [4];
  pair_t p;
  always_comb begin
    foreach (mem[i])
      mem[i] = '0;
    if (sel)
      mem[0] = a[7:0];
    p = '{hi : a[31:16], lo : a[15:0]};
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: the last port's unpacked dimension is spaced like the others", "[formatter][regression]") {
    const std::string input = R"SV(module lanes (input logic [7:0] din [4], output logic [7:0] dout [4]);
endmodule
)SV";
    const std::string expected = R"SV(module lanes(
  input logic [7:0] din [4],
  output logic [7:0] dout [4]
);
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: postfix increments space like operands", "[formatter][regression]") {
    const std::string input = R"SV(module cnt;
  int x, y;
  initial begin
    for (int i = 0; i < 4; i++) y = i;
    if (x++ > 3) y = x-- + 1;
  end
endmodule
)SV";
    const std::string expected = R"SV(module cnt;
  int x, y;
  initial begin
    for (int i = 0; i < 4; i++)
      y = i;
    if (x++ > 3)
      y = x-- + 1;
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: unary signs and reductions bind; binary xnor spaces", "[formatter][regression]") {
    const std::string input = R"SV(module ops (input logic [3:0] a, b, c, output logic [3:0] y, output logic p);
  assign y = -a + (b ^~ c);
  assign p = &c | ^c;
endmodule
)SV";
    const std::string expected = R"SV(module ops(
  input logic [3:0] a, b, c,
  output logic [3:0] y,
  output logic p
);
  assign y = -a + (b ^~ c);
  assign p = &c | ^c;
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: operator position under every binary spacing style", "[formatter][regression]") {
    const std::string input = R"SV(module m (input logic [3:0] a, b, c, output logic [3:0] y);
  int x, v;
  always_comb begin
    y = -a + +b - -c;
    y = (a ^~ b) | (b ~^ a) ^ ~a;
    y = a & |c & ~&c | ^c;
    y = f(-1, -a, ~b);
    v = x++ + ++x;
    v = x-- - --x;
    if (x++ > 3) v = x-- + 1;
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    for (const char* style : {"both", "none"}) {
        FormatOptions opts;
        opts.spacing.binary_operator_spacing = style;
        const std::string out = format_stable(input, opts);
        INFO("style " << style << ":\n" << out);
        CHECK(parses_cleanly(out));
        CHECK(out.find("f(-1, -a, ~b)") != std::string::npos);
    }
}

TEST_CASE("formatter regression: SVA repetition operators bind to their brackets", "[formatter][regression]") {
    const std::string input = R"SV(module handshake (input logic clk, req, ack);
  a_ack: assert property (@(posedge clk) req |-> ack[->1] ##1 !ack[=2]);
endmodule
)SV";
    const std::string expected = R"SV(module handshake(
  input logic clk, req, ack
);
  a_ack: assert property (@(posedge clk) req |-> ack[->1] ##1 !ack[=2]);
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: every SVA repetition form stays closed up", "[formatter][regression]") {
    const std::string input = R"SV(module m (input logic clk, a, b);
  property p; @(posedge clk) a[->1:3] |-> b[=1:$] ##1 b[*2:4] ##1 b[+] ##1 b[*]; endproperty
  assert property (@(posedge clk) a |-> b[->1] ##1 b[=2] ##1 b[*3]);
endmodule
)SV";
    CHECK(parses_cleanly(input));
    const std::string out = format_stable(input);
    CHECK(parses_cleanly(out));
    CHECK(out.find("a[->1:3] |-> b[=1:$] ##1 b[*2:4] ##1 b[+] ##1 b[*];") != std::string::npos);
    CHECK(out.find("a |-> b[->1] ##1 b[=2] ##1 b[*3]);") != std::string::npos);
}

TEST_CASE("formatter regression: a deferred assertion's final opens no block", "[formatter][regression]") {
    const std::string input = R"SV(module deferred (input logic a, b);
  always_comb begin
    a_x: assert final (!(a && b)) else $error("both");
  end
endmodule
)SV";
    const std::string expected = R"SV(module deferred(
  input logic a, b
);
  always_comb begin
    a_x: assert final (!(a && b))
    else
      $error("both");
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a chain of trailing comments after end stays on its line", "[formatter][regression]") {
    const std::string input = R"SV(module fsm;
  always_comb begin
  end /* p_next */ // combinational
endmodule /* fsm */ // end of file
)SV";
    const std::string expected = R"SV(module fsm;
  always_comb begin
  end /* p_next */ // combinational
endmodule /* fsm */ // end of file
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: an attribute on a design unit has its own line", "[formatter][regression]") {
    const std::string input = R"SV((* keep_hierarchy = "yes" *)
module sync2 (input logic clk, d, output logic q);
  (* ASYNC_REG = "TRUE" *) logic s0;
endmodule
)SV";
    const std::string expected = R"SV((* keep_hierarchy = "yes" *)
module sync2(
  input logic clk, d,
  output logic q
);
  (* ASYNC_REG = "TRUE" *) logic s0;
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: new, type and dimension spacing after keywords", "[formatter][regression]") {
    const std::string input = R"SV(module misc;
  int da[], q[$];
  event done;
  initial begin
    da = new[4];
    wait (q.size() > 0);
    q[0] = type(q[0])'(3);
  end
endmodule
)SV";
    const std::string expected = R"SV(module misc;
  int da [], q [$];
  event done;
  initial begin
    da = new[4];
    wait(q.size() > 0);
    q[0] = type(q[0])'(3);
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: binsof and tagged pattern variables", "[formatter][regression]") {
    const std::string input = R"SV(module m;
  typedef union tagged { void Invalid; int Valid; } maybe_t;
  maybe_t v;
  bit [3:0] a, b;
  covergroup cg;
    cp: coverpoint a;
    cq: coverpoint b;
    x: cross cp, cq { ignore_bins i1 = binsof(cp) intersect {1}; }
  endgroup
  initial begin
    case (v) matches
      tagged Valid .n: $display(n);
      default: ;
    endcase
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    const std::string out = format_stable(input);
    CHECK(parses_cleanly(out));
    CHECK(out.find("binsof(cp) intersect {1};") != std::string::npos);
    CHECK(out.find("tagged Valid .n: $display(n);") != std::string::npos);
}

TEST_CASE("formatter regression: a configured statement terminator macro ends its statement", "[formatter][regression]") {
    const std::string input = R"SV(`define SEMI ;
module m (input logic en, rst_n, output logic st);
  always_comb begin
    st = en `SEMI
    if (!rst_n) st = '0;
  end
endmodule
)SV";
    const std::string expected = R"SV(`define SEMI ;
module m(
  input logic en, rst_n,
  output logic st
);
  always_comb begin
    st = en `SEMI
    if (!rst_n)
      st = '0;
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(parses_cleanly(expected));
    FormatOptions opts;
    opts.macros.statement_terminator_like = {"SEMI"};
    CHECK(format_stable(input, opts) == expected);
    // Without the role the macro is an unknown expression leaf, as before.
    CHECK(format_stable(input) != expected);
}

// FORMAT_BUG_FIX3.md: K-1 .. K-15.

TEST_CASE("formatter regression: wrapped call arguments follow statement-alignment padding (hanging)", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
y = f(arg_one, arg_two, arg_three);
long_name_zz = 1;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial begin
        y          = f(arg_one,
                       arg_two,
                       arg_three);
        long_name_zz = 1;
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.statement.align = true;
    opts.statement.lhs_min_width = 10;
    opts.function_call.break_policy = "always";
    opts.function_call.arg_count = 3;
    opts.function_call.layout = "hanging";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: wrapped call arguments follow statement-alignment padding (block)", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
y = f(arg_one, arg_two, arg_three);
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial begin
        y          = f(
                         arg_one,
                         arg_two,
                         arg_three
                     );
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.statement.align = true;
    opts.statement.lhs_min_width = 10;
    opts.function_call.break_policy = "always";
    opts.function_call.arg_count = 3;
    opts.function_call.layout = "block";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: declaration alignment stays off casts, return types and hanging argument lists", "[formatter][regression]") {
    const std::string input = R"SV(class c;
task run();
void'(arr.sum() with (int'(item)));
endtask
static function base#(T, W) create();
endfunction
function void do_it(input int a, output int bbbbbbbbbb);
endfunction
logic [3:0] a;
endclass
)SV";
    const std::string expected = R"SV(class c;
    task run();
        void'(arr.sum() with (int'(item)));
    endtask
    static function base #(T, W) create();
    endfunction
    function void do_it(input int a,
                        output int bbbbbbbbbb);
    endfunction
    logic  [3:0]       a       ;
endclass
)SV";
    FormatOptions opts = indent4();
    opts.var_declaration.align = true;
    opts.var_declaration.section2_min_width = 12;
    opts.var_declaration.section3_min_width = 8;
    opts.function_declaration.layout = "hanging";
    opts.function_declaration.line_length = 30;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an own-line comment inside a continued expression keeps the continuation indent", "[formatter][regression]") {
    const std::string input = R"SV(module m;
assign d = a +
// c
b;
initial begin
x = a +
// c
b;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    assign d = a +
        // c
        b;
    initial begin
        x = a +
            // c
            b;
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a parameter override list with a comment expands", "[formatter][regression]") {
    const std::string input = R"SV(module m;
fifo #(.W(8), .D(4) // depth
) u (.clk(clk));
endmodule
)SV";
    const std::string expected = R"SV(module m;
    fifo #(
        .W(8),
        .D(4) // depth
    ) u(
        .clk(clk)
    );
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an empty positional connection takes the port indent", "[formatter][regression]") {
    const std::string input = R"SV(module m;
sub u (
.a(a),
.b(b)
);
sub u2 (clk,
 a,
 ,
 y);
endmodule
)SV";
    const std::string expected = R"SV(module m;
    sub u(
        .a(a),
        .b(b)
    );
    sub u2(
        clk,
        a,
        ,
        y
    );
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an inline constraint block stays on its line", "[formatter][regression]") {
    const std::string input = R"SV(class c;
task run();
if (!this.randomize() with {data < 5; id == 3;}) $error("f");
endtask
endclass
)SV";
    const std::string expected = R"SV(class c;
    task run();
        if (!this.randomize() with { data < 5; id == 3; })
            $error("f");
    endtask
endclass
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a class specialization parameter list stays inline", "[formatter][regression]") {
    const std::string input = R"SV(class c extends p #(T);
endclass
class d extends p #(.T(byte), .W(4));
endclass
class e #(type T = int) extends p #(T, 4);
endclass
)SV";
    const std::string expected = R"SV(class c extends p #(T);
endclass
class d extends p #(.T(byte), .W(4));
endclass
class e #(
    type T = int
) extends p #(T, 4);
endclass
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a member-select LHS joins assignment alignment", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always_ff @(posedge clk) begin
s.a <= d;
s.bb <= 1'b1;
y <= 1'b0;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    always_ff @(posedge clk) begin
        s.a        <= d;
        s.bb       <= 1'b1;
        y          <= 1'b0;
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.statement.align = true;
    opts.statement.lhs_min_width = 10;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: the first assign in a labelled generate block keeps its select", "[formatter][regression]") {
    const std::string input = R"SV(module m;
for (genvar g = 0; g < 2; g++) begin : gen
assign wl[g] = 1;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    for (genvar g = 0; g < 2; g++) begin: gen
        assign wl[g] = 1;
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an assignment pattern after a key keeps its space", "[formatter][regression]") {
    const std::string input = R"SV(module m;
parameter pkt_t DEF = '{tag: 4'h1, data: '{raw: 8'h0}};
initial x = c ? '{1} : '{2};
endmodule
)SV";
    const std::string expected = R"SV(module m;
    parameter pkt_t DEF = '{tag : 4'h1, data : '{raw : 8'h0}};
    initial
        x = c ? '{1} : '{2};
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: new with a size and an initializer stays joined", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial d = new[10](d);
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial
        d = new[10](d);
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: gate and bind instances are laid out as instances", "[formatter][regression]") {
    const std::string input = R"SV(module m;
and #1 g1 (o1, i1, i2), g2 (o2, i3, i4);
bind fifo chk u_chk (.clk(clk), .a(a));
endmodule
)SV";
    const std::string expected = R"SV(module m;
    and #1 g1(o1, i1, i2), g2(o2, i3, i4);
    bind fifo chk u_chk(
        .clk(clk),
        .a(a)
    );
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.function_call.break_policy = "always";
    opts.function_call.arg_count = 2;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: space_inside_parens is symmetric on overrides and after a concatenation", "[formatter][regression]") {
    const std::string input = R"SV(module m;
fifo #(.WIDTH(8)) u (.d({d[3:0], d[7:4]}), .q(q));
endmodule
)SV";
    const std::string expected = R"SV(module m;
    fifo #( .WIDTH( 8 ) ) u(
        .d( {d[3:0], d[7:4]} ),
        .q( q )
    );
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.spacing.space_inside_parens = true;
    opts.function_call.space_inside_paren = true;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: port alignment covers interface ports and comment-led rows", "[formatter][regression]") {
    const std::string input = R"SV(module p (
input logic a,
interface.slave bus,
bus_if.master mbus,
/* c */ input logic b,
output logic [7:0] q
);
endmodule
)SV";
    const std::string expected = R"SV(module p(
    input   logic           a,
    interface.slave         bus,
    bus_if.master           mbus,
    /* c */ input logic     b,
    output  logic   [7:0]   q
);
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.port_declaration.align = true;
    opts.port_declaration.section1_min_width = 8;
    opts.port_declaration.section2_min_width = 8;
    opts.port_declaration.section3_min_width = 8;
    opts.port_declaration.section4_min_width = 6;
    opts.port_declaration.section5_min_width = 0;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: timing controls align like the assignment they lead", "[formatter][regression]") {
    const std::string input = R"SV(module m;
wire #(1,2) w1, w2;
initial begin
#(2) a = b;
cc = d;
@(posedge clk) e = f;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    wire #(1, 2) w1, w2     ;
    initial begin
        #(2) a           = b;
        cc               = d;
        @(posedge clk) e = f;
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.statement.align = true;
    opts.statement.lhs_min_width = 0;
    opts.var_declaration.align = true;
    opts.var_declaration.section2_min_width = 14;
    opts.var_declaration.section3_min_width = 6;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an event-control star and a case-inside header continue no expression", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always @*
o = clk;
always_comb begin
priority case (s) inside
[0:1]: y = 0;
default: y = 1;
endcase
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    always @*
        o = clk;
    always_comb begin
        priority case (s) inside
            [0:1]: y = 0;
            default: y = 1;
        endcase
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a long interface type widens a non-adaptive port group", "[formatter][regression]") {
    const std::string input = R"SV(module p (
input logic a,
some_long_interface_name.master mbus,
input logic bb
);
endmodule
)SV";
    const std::string expected = R"SV(module p(
    input logic                     a,
    some_long_interface_name.master mbus,
    input logic                     bb
);
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = false;
    opts.port_declaration.section1_min_width = 6;
    opts.port_declaration.section2_min_width = 6;
    opts.port_declaration.section3_min_width = 4;
    opts.port_declaration.section4_min_width = 6;
    opts.port_declaration.section5_min_width = 0;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an SVA not before a sequence instance is no gate", "[formatter][regression]") {
    const std::string input = R"SV(module m;
and g1 (o1, i1, i2);
sequence s1(x); x; endsequence
property pr(a);
not s1(a);
endproperty
endmodule
)SV";
    const std::string expected = R"SV(module m;
    and g1(o1, i1, i2);
    sequence s1(x);
        x;
    endsequence
    property pr(a);
        not s1(a);
    endproperty
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: each ifdef branch continues the expression before it", "[formatter][regression]") {
    const std::string input = R"SV(module m;
localparam int W =
`ifdef WIDE
64;
`else
32;
`endif
endmodule
)SV";
    const std::string expected = R"SV(module m;
    localparam int W =
`ifdef WIDE
        64;
`else
        32;
`endif
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an inline constraint holding a line comment breaks after its brace", "[formatter][regression]") {
    const std::string input = R"SV(class c;
task run();
if (!randomize() with { a < 5; // c
b > 2; }) $error("x");
endtask
endclass
)SV";
    const std::string expected = R"SV(class c;
    task run();
        if (!randomize() with {
            a < 5; // c
            b > 2; })
            $error("x");
    endtask
endclass
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: the implicit event list is spaced like a first token", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always @(*) o = a;
always @( * ) o = b;
endmodule
)SV";
    const std::string expected = R"SV(module m;
    always @(*)
        o = a;
    always @(*)
        o = b;
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: the implicit event list takes the event-control paren padding", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always @(*) o = a;
always @(posedge c) o = b;
endmodule
)SV";
    const std::string expected = R"SV(module m;
    always @( * )
        o = a;
    always @( posedge c )
        o = b;
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.spacing.space_inside_event_control_parens = true;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a concatenation target aligns like a single name", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
{p, q} = r;
x = 1;
'{s, t} = u;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial begin
        {p, q}     = r;
        x          = 1;
        '{s, t}    = u;
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.statement.align = true;
    opts.statement.lhs_min_width = 10;
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a line after a conditional colon or an event or continues the expression", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always @(posedge clk or // c
negedge rst_n)
q <= d;
assign w = sel ? f(a) : // c
g(b);
initial case (s)
1: x = 1;
default: x = 2;
endcase
endmodule
)SV";
    const std::string expected = R"SV(module m;
    always @(posedge clk or // c
        negedge rst_n)
        q <= d;
    assign w = sel ? f(a) : // c
        g(b);
    initial
        case (s)
            1: x = 1;
            default: x = 2;
        endcase
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: hanging arguments after an inline block comment start under the paren", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
x = 1;
zz = /* c */ f(aaaa, bbbb, cccc);
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial begin
        x          = 1;
        zz         = /* c */ f(aaaa,
                               bbbb,
                               cccc);
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.statement.align = true;
    opts.statement.lhs_min_width = 10;
    opts.function_call.break_policy = "always";
    opts.function_call.arg_count = 3;
    opts.function_call.layout = "hanging";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a blank line inside a statement starts a continuation line", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
y = a +

f(aaaa, bbbb, cccc);
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial begin
        y = a +

            f(aaaa,
              bbbb,
              cccc);
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.function_call.break_policy = "always";
    opts.function_call.arg_count = 3;
    opts.function_call.layout = "hanging";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: hanging arguments after a multi-line block comment start from its last line", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
x = 1;
y = /* a
b */ func(aaaa, bbbb, cccc);
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial begin
        x          = 1;
        y          = /* a
b */ func(aaaa,
          bbbb,
          cccc);
    end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.statement.align = true;
    opts.statement.lhs_min_width = 10;
    opts.function_call.break_policy = "always";
    opts.function_call.arg_count = 3;
    opts.function_call.layout = "hanging";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a line led by the conditional colon or after a property or continues the expression", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial y = sel ? a // why
: b;
property p;
a or // c
b;
endproperty
endmodule
)SV";
    const std::string expected = R"SV(module m;
    initial
        y = sel ? a // why
            : b;
    property p;
        a or // c
            b;
    endproperty
endmodule
)SV";
    FormatOptions opts = indent4();
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a generic interface port opens no scope", "[formatter][regression]") {
    const std::string input = R"SV(module m (interface g);
wire a;
endmodule
module n;
wire c;
endmodule
module m2 (interface.mst g, input logic x);
wire a;
endmodule
class c; virtual interface bus_if vif; endclass
)SV";
    const std::string expected = R"SV(module m(
  interface g
);
  wire a;
endmodule
module n;
  wire c;
endmodule
module m2(
  interface.mst g,
  input logic x
);
  wire a;
endmodule
class c;
  virtual interface bus_if vif;
endclass
)SV";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a nested module closes at its own depth", "[formatter][regression]") {
    const std::string input = R"SV(module outer;
module inner;
wire a;
endmodule
wire b;
endmodule
module next;
wire c;
endmodule
)SV";
    const std::string expected = R"SV(module outer;
  module inner;
    wire a;
  endmodule
  wire b;
endmodule
module next;
  wire c;
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: alternative module headers under ifdef are one unit", "[formatter][regression]") {
    const std::string input = R"SV(`ifdef A
module m (input a);
`else
module m (input b);
`endif
wire w;
endmodule
module n;
wire c;
endmodule
)SV";
    const std::string expected = R"SV(`ifdef A
module m(
  input a
);
`else
module m(
  input b
);
`endif
  wire w;
endmodule
module n;
  wire c;
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: always inside a property is an operator, not a block", "[formatter][regression]") {
    const std::string input = R"SV(module m;
a1: assert property (@(posedge clk) s_eventually always a);
a2: assert property (@(posedge clk) a |-> always [1:3] b);
always @(posedge clk) x <= y;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  a1: assert property (@(posedge clk) s_eventually always a);
  a2: assert property (@(posedge clk) a |-> always [1:3] b);
  always @(posedge clk)
    x <= y;
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: every ifdef branch of a brace-less body keeps its indent", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always_comb
`ifdef FAST
x = 1;
`else
x = 2;
`endif
initial
if (a)
`ifdef FAST
x = 1;
`elsif MID
x = 3;
`else
x = 2;
`endif
assign y = 1;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  always_comb
`ifdef FAST
    x = 1;
`else
    x = 2;
`endif
  initial
    if (a)
`ifdef FAST
      x = 1;
`elsif MID
      x = 3;
`else
      x = 2;
`endif
  assign y = 1;
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: instances as generate bodies or with attributes are instances", "[formatter][regression]") {
    const std::string input = R"SV(module m;
if (N > 2)
sub u_b (.a(a));
for (genvar k = 0; k < 4; k++)
sub u_c [3:0] (.a(a));
(* keep *) sub u_d (.a(a));
endmodule
)SV";
    const std::string expected = R"SV(module m;
  if (N > 2)
    sub u_b(
      .a(a)
    );
  for (genvar k = 0; k < 4; k++)
    sub u_c[3:0](
      .a(a)
    );
  (* keep *) sub u_d(
    .a(a)
  );
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a hanging call whose paren is past the limit is not broken", "[formatter][regression]") {
    const std::string input =
        "module m;\n"
        "assign y = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa + bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb + "
        "cccccccccccccccccccc + g(d);\n"
        "endmodule\n";
    const std::string expected =
        "module m;\n"
        "  assign y = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa + bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb + "
        "cccccccccccccccccccc + g(d);\n"
        "endmodule\n";
    for (const char* layout : {"hanging", "block"}) {
        FormatOptions opts;
        opts.function_call.layout = layout;
        INFO("layout = " << layout);
        CHECK(format_stable(input, opts) == expected);
    }
}

TEST_CASE("formatter regression: every unpacked declarator is spaced from its name", "[formatter][regression]") {
    const std::string input = R"SV(module m;
logic a [4];
logic b [2][3];
int e [2], f [3][4];
initial m2[1][2] = 0;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  logic a [4];
  logic b [2][3];
  int e [2], f [3][4];
  initial
    m2[1][2] = 0;
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a macro-call declarator is aligned as one name", "[formatter][regression]") {
    const std::string input = R"SV(module m;
logic [3:0] `CAT(foo, _q);
logic [7:0] bar;
endmodule
)SV";
    const std::string expected = R"SV(module m;
logic               [3:0]               `CAT(foo, _q)                       ;
logic               [7:0]               bar                                 ;
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.default_indent_level_inside_outmost_block = 0;
    opts.var_declaration.align = true;
    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a leading comma after an ifdef stays with its item", "[formatter][regression]") {
    const std::string input = R"SV(module m #(
parameter int W = 8
`ifdef WIDE
, parameter int X = 64
`endif
) (
input logic a
`ifdef WIDE
, input logic b
`endif
);
endmodule
)SV";
    const std::string expected = R"SV(module m #(
  parameter int W = 8
`ifdef WIDE
  , parameter int X = 64
`endif
)(
  input logic a
`ifdef WIDE
  , input logic b
`endif
);
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a comma inside a bins item does not split the item", "[formatter][regression]") {
    const std::string input = R"SV(module m;
covergroup cg @(posedge clk);
cp: coverpoint a {
bins tr = (1 => 2), (4 => 5);
bins s = {1, 2}, t = {3};
}
endgroup
endmodule
)SV";
    const std::string expected = R"SV(module m;
  covergroup cg @(posedge clk);
    cp: coverpoint a {
      bins tr = (1 => 2), (4 => 5);
      bins s = {1, 2}, t = {3};
    }
  endgroup
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: every procedural event control follows the options", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
@(posedge clk);
@(ev1 or ev2);
@(negedge clk) x = 1;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
initial begin
    @ ( posedge clk );
    @ ( ev1 or ev2 );
    @ ( negedge clk ) x = 1;
end
endmodule
)SV";
    FormatOptions opts = indent4();
    opts.default_indent_level_inside_outmost_block = 0;
    opts.spacing.procedural_event_control_at_spacing = "both";
    opts.spacing.space_inside_event_control_parens = true;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a statement label after an end label is a label", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial begin
begin : blk end : blk
lbl: x = 3;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  initial begin
    begin: blk
    end: blk
    lbl: x = 3;
  end
endmodule
)SV";
    CHECK(parses_cleanly(input));
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: an attribute is spaced as one unit", "[formatter][regression]") {
    const std::string input = R"SV(module m;
sub (* keep *) u (.a(a));
function (* noinline *) int f(); return 1; endfunction
(* keep *) wire w;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  sub (* keep *) u(
    .a(a)
  );
  function (* noinline *) int f();
    return 1;
  endfunction
  (* keep *) wire w;
endmodule
)SV";
    CHECK(format_stable(input) == expected);

    FormatOptions opts;
    opts.spacing.binary_operator_spacing = "none";
    const std::string none = format_stable(input, opts);
    CHECK(none.find("(* keep *)") != std::string::npos);
    CHECK(none.find("(* noinline *)") != std::string::npos);
}

TEST_CASE("formatter regression: SVA repetition brackets bind whatever the dimension options", "[formatter][regression]") {
    const std::string input = R"SV(module m;
sequence s; a ##1 b [*1:3] ##1 c [->2] ##1 d [=3]; endsequence
assign z = q[1:0];
endmodule
)SV";
    const std::string expected = R"SV(module m;
  sequence s;
    a ##1 b[*1:3] ##1 c[->2] ##1 d[=3];
  endsequence
  assign z = q[ 1 : 0 ];
endmodule
)SV";
    FormatOptions opts;
    opts.spacing.space_inside_dimension_brackets = true;
    opts.spacing.range_colon_spacing = "both";
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: empty for clauses add no padding", "[formatter][regression]") {
    const std::string input = R"SV(module m;
initial for (;;) break;
initial for (i = 0; i < 3;) i++;
endmodule
)SV";
    for (const char* mode : {"none", "after", "both"}) {
        FormatOptions opts;
        opts.spacing.semicolon_spacing = mode;
        INFO("semicolon_spacing = " << mode);
        const std::string out = format_stable(input, opts);
        CHECK(out.find("for (;;)") != std::string::npos);
        // The last clause is empty: nothing between its `;` and the `)`.
        CHECK(out.find(";)") != std::string::npos);
        CHECK(out.find("; )") == std::string::npos);
    }
}

TEST_CASE("formatter regression: an operator is never glued to a comment", "[formatter][regression]") {
    const std::string input = R"SV(module m;
assign y = a + // carry-in
b;
assign z = a && /* gate */ b;
always_ff /* c */ @(posedge clk) q <= d;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  assign y=a+ // carry-in
    b;
  assign z=a&& /* gate */ b;
  always_ff /* c */ @(posedge clk)
    q<=d;
endmodule
)SV";
    FormatOptions opts;
    opts.spacing.binary_operator_spacing = "none";
    opts.spacing.assignment_operator_spacing = "none";
    opts.spacing.procedural_event_control_at_spacing = "none";
    CHECK(format_stable(input, opts) == expected);
}

// Round 5 (M-1..M-12).

TEST_CASE("formatter regression: a select target with <= or += is not a declaration", "[formatter][regression]") {
    const std::string input = R"SV(module m;
logic [7:0] q;
always_ff @(posedge clk) begin
mem[w] <= d;
cnt[i] += d;
pkg::arr[i] = d;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  logic   [7:0]   q       ;
  always_ff @(posedge clk) begin
    mem[w] <= d;
    cnt[i] += d;
    pkg::arr[i] = d;
  end
endmodule
)SV";
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 8;
    opts.var_declaration.section2_min_width = 8;
    opts.var_declaration.section3_min_width = 8;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an own-line comment before a brace-less body is indented with it", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always_comb begin
if (a)
// c1
x = 1;
else
// c2
x = 0;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  always_comb begin
    if (a)
      // c1
      x = 1;
    else
      // c2
      x = 0;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a case item's statement is a controlled body", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always_comb begin
case (s)
A: if (go) n = B; else n = A;
B:
// c
z = 2;
endcase
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  always_comb begin
    case (s)
      A: if (go)
          n = B;
        else
          n = A;
      B:
        // c
        z = 2;
    endcase
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: an ANSI list led by an interface port is not packed as non-ANSI", "[formatter][regression]") {
    const std::string input = R"SV(module m (axi_if.master m_axi, input logic b, output logic c);
endmodule
)SV";
    const std::string expected = R"SV(module m(
  axi_if.master m_axi,
  input logic b,
  output logic c
);
endmodule
)SV";
    FormatOptions opts;
    opts.module.non_ansi_port_per_line_enabled = true;
    opts.module.non_ansi_port_per_line = 3;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: the statement after a format-off region is aligned", "[formatter][regression]") {
    const std::string input = R"SV(module m;
always_comb begin
x = 1;
// verilog_format: off
q   =    r;
// verilog_format: on
yy = zz;
yyy = zzz;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  always_comb begin
    x    = 1;
// verilog_format: off
q   =    r;
// verilog_format: on
    yy   = zz;
    yyy  = zzz;
  end
endmodule
)SV";
    FormatOptions opts;
    opts.statement.align = true;
    opts.statement.lhs_min_width = 4;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: calls inside brackets are not broken per argument", "[formatter][regression]") {
    const std::string input = R"SV(module m;
logic [clog(A,B,C)-1:0] w;
logic [7:0] q;
always_comb y = mem[hash(a,b,c)];
endmodule
)SV";
    const std::string expected = R"SV(module m;
  logic [clog(A, B, C)-1:0] w;
  logic [7:0] q;
  always_comb
    y = mem[hash(a, b, c)];
endmodule
)SV";
    FormatOptions opts;
    opts.function_call.break_policy = "always";
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: scoped and var-led declarations are aligned", "[formatter][regression]") {
    const std::string input = R"SV(module m;
pkg::cfg_t cfg;
var logic vl;
logic [7:0] data_q;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  pkg::cfg_t       cfg    ;
  var logic        vl     ;
  logic      [7:0] data_q ;
endmodule
)SV";
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 4;
    opts.var_declaration.section2_min_width = 4;
    opts.var_declaration.section3_min_width = 4;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: instance port name width counts the dot", "[formatter][regression]") {
    const std::string input = R"SV(module m;
sub u (.a(a), .long_port(long_signal), .z(z));
sub v (.abcdefghijklmnopqrs(x), .b(y));
endmodule
)SV";
    const std::string expected = R"SV(module m;
  sub u (
    .a                  (a          ),
    .long_port          (long_signal),
    .z                  (z          )
  );
  sub v (
    .abcdefghijklmnopqrs (x),
    .b                   (y)
  );
endmodule
)SV";
    FormatOptions opts;
    opts.instance.align = true;
    opts.instance.instance_port_name_width = 20;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: hanging function arguments get no port-list dimension padding", "[formatter][regression]") {
    const std::string input = R"SV(module m;
function automatic logic f(input logic a, input logic [7:0] b);
return a;
endfunction
endmodule
)SV";
    const std::string expected = R"SV(module m;
  function automatic logic f(input logic a,
                             input logic [7:0] b);
    return a;
  endfunction
endmodule
)SV";
    FormatOptions opts;
    opts.function_declaration.layout = "hanging";
    opts.function_declaration.line_length = 40;
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = false;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: strength parens and comment-only parens space like code", "[formatter][regression]") {
    const std::string input = R"SV(module m;
wire (pull1, pull0) [3:0] pw = 4'h0;
trireg (small) [7:0] tr;
sub u (.a(/* unused */), .b(q));
endmodule
)SV";
    const std::string expected = R"SV(module m;
  wire (pull1, pull0) [3:0] pw = 4'h0;
  trireg (small) [7:0] tr;
  sub u(
    .a(/* unused */),
    .b(q)
  );
endmodule
)SV";
    CHECK(format_stable(input) == expected);
    CHECK(parses_cleanly(expected));
}

TEST_CASE("formatter regression: a scoped name before a select is not a declaration", "[formatter][regression]") {
    const std::string input = R"SV(module m;
pkg::type_t arr [4];
always_comb pkg::arr[i] = d;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  pkg::type_t arr [4];
  always_comb
    pkg::arr[i] = d;
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a parameter's unpacked dimension is spaced like a variable's", "[formatter][regression]") {
    const std::string input = R"SV(module m #(parameter int P[2] = '{0, 1}, parameter type T = logic [3:0]) ();
localparam logic [7:0] LUT[4] = '{1, 2, 3, 4};
parameter int A[2] = '{0, 1};
localparam int B[2][3] = '{default: 0};
localparam int W = C[1] + $size(A[0]);
parameter int X = 1, Y[2] = '{0, 1};
logic t[2];
endmodule
)SV";
    const std::string expected = R"SV(module m #(
  parameter int P [2] = '{0, 1},
  parameter type T = logic [3:0]
)();
  localparam logic [7:0] LUT [4] = '{1, 2, 3, 4};
  parameter int A [2] = '{0, 1};
  localparam int B [2][3] = '{default : 0};
  localparam int W = C[1] + $size(A[0]);
  parameter int X = 1, Y [2] = '{0, 1};
  logic t [2];
endmodule
)SV";
    CHECK(format_stable(input) == expected);
    CHECK(parses_cleanly(expected));
}

// ---------------------------------------------------------------------------
// Round 6 (FORMAT_BUG_FIX6.md)
// ---------------------------------------------------------------------------

TEST_CASE("formatter regression: a CRLF multi-line define stays verbatim", "[formatter][regression]") {
    // N-1: `\` `\r` `\n` is a continuation.  Missing it reformatted the body
    // as module text, moved the `\` to its own line and spaced the `` paste.
    const std::string lf = "`define REG(n) \\\n"
                           "\tlogic n``_q; \\\n"
                           "  always_ff @(posedge clk) n``_q <= n``_d;\n"
                           "module m;\n"
                           "endmodule\n";
    std::string crlf;
    for (char c : lf) {
        if (c == '\n')
            crlf += '\r';
        crlf += c;
    }
    CHECK(format_stable(lf) == lf);
    // format_source() emits LF; the caller restores the buffer's line ending.
    std::string out = format_stable(crlf);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    CHECK(out == lf);
}

TEST_CASE("formatter regression: default-width declaration alignment uses the first declarator", "[formatter][regression]") {
    // N-2: with section1_min_width = 0 (the default) the name column went to
    // the last identifier -- `ready` in `logic valid, ready;`, `N` in
    // `d [N]` -- and was measured from each line's own keyword.
    FormatOptions opts;
    opts.var_declaration.align = true;
    const std::string input = R"SV(module m;
int a;
logic b;
int unsigned c;
logic [3:0] d [N];
logic valid, ready;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  int                                 a                             ;
  logic                               b                             ;
  int unsigned                        c                             ;
  logic [3:0]                         d [N]                         ;
  logic                               valid, ready                  ;
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a long override list breaks per override and not inside one", "[formatter][regression]") {
    // N-3: the `#(...)` list was never measured, so the only paren the
    // line-length rule found was `.USER_WIDTH(`, broken as a one-argument
    // call and hung at column 98.
    const std::string input = R"SV(module m;
axi_xbar #(.NUM_MASTERS(4), .NUM_SLAVES(8), .ADDR_WIDTH(64), .DATA_WIDTH(512), .ID_WIDTH(8), .USER_WIDTH(16)) u_xbar (.clk(clk));
axi_xbar #(.NUM_MASTERS(4), .NUM_SLAVES(8)) u_short (.clk(clk));
endmodule
)SV";
    const std::string expected = R"SV(module m;
  axi_xbar #(
    .NUM_MASTERS(4),
    .NUM_SLAVES(8),
    .ADDR_WIDTH(64),
    .DATA_WIDTH(512),
    .ID_WIDTH(8),
    .USER_WIDTH(16)
  ) u_xbar(
    .clk(clk)
  );
  axi_xbar #(.NUM_MASTERS(4), .NUM_SLAVES(8)) u_short(
    .clk(clk)
  );
endmodule
)SV";
    CHECK(format_stable(input) == expected);
    CHECK(parses_cleanly(expected));
}

TEST_CASE("formatter regression: an intra-assignment repeat is not a loop", "[formatter][regression]") {
    // N-4: `repeat (n)` right after `<=`/`=` times the assignment; its body
    // was broken onto a line of its own as though it were a loop's.
    const std::string input = R"SV(module m;
initial begin
q <= repeat (2) @(posedge clk) d;
q = repeat (3) @(negedge clk) d;
repeat (4) x = x + 1;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  initial begin
    q <= repeat (2) @(posedge clk) d;
    q = repeat (3) @(negedge clk) d;
    repeat (4)
      x = x + 1;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a call in a brace-less body is measured from its own line", "[formatter][regression]") {
    // N-5: the body wrap ran after the call-break loop, so the call was
    // measured as though it still followed its `if (...)` header.
    const std::string input = R"SV(module m;
initial begin
if (some_long_condition_signal && another_long_condition_signal && third_cond) report_error("ID", "failed");
if (some_long_condition_signal && another_long_condition_signal && third_cond) `uvm_error("ID", "failed")
end
always @(posedge clk_with_a_long_name or negedge rst_with_a_long_name_n or posedge x) q <= compute(a, b);
function (* noinline *) int f(input int x); return x; endfunction
endmodule
)SV";
    const std::string expected = R"SV(module m;
  initial begin
    if (some_long_condition_signal && another_long_condition_signal && third_cond)
      report_error("ID", "failed");
    if (some_long_condition_signal && another_long_condition_signal && third_cond)
      `uvm_error("ID", "failed")
  end
  always @(posedge clk_with_a_long_name or negedge rst_with_a_long_name_n or posedge x)
    q <= compute(a, b);
  function (* noinline *) int f(input int x);
    return x;
  endfunction
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: sequence property and let formals are declarations and not calls", "[formatter][regression]") {
    // N-6: `name(` after `sequence`/`property`/`let` was broken per argument
    // under `function_call.break_policy = "always"`.  They follow
    // `function_declaration.*` now, like a function's formals.
    const std::string input = R"SV(module m;
sequence s_req(sig, n);
sig ##n !sig;
endsequence
property p_x(a, b);
@(posedge clk) a |-> s_req(a, 2);
endproperty
let max(a, b) = (a > b) ? a : b;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  sequence s_req(sig, n);
    sig ##n !sig;
  endsequence
  property p_x(a, b);
    @(posedge clk) a |-> s_req(
                           a,
                           2
                         );
  endproperty
  let max(a, b) = (a > b) ? a : b;
endmodule
)SV";
    FormatOptions opts;
    opts.function_call.break_policy = "always";
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: hanging items line up with the first item after a spaced paren", "[formatter][regression]") {
    // N-7: the hanging column was taken right after `(`, ignoring the space
    // the inside-paren options put there, so every continuation line sat
    // one column left of the first item.
    const std::string input = R"SV(module exprs #(parameter int W = 32, parameter type T = logic) (input logic a);
function int add(int alpha, int beta); return alpha + beta; endfunction
initial x = foo(alpha_arg, beta_arg);
endmodule
)SV";
    const std::string expected = R"SV(module exprs #( parameter int W = 32,
                parameter type T = logic )(
  input logic a
);
  function int add( int alpha,
                    int beta );
    return alpha + beta;
  endfunction
  initial
    x = foo( alpha_arg,
             beta_arg );
endmodule
)SV";
    FormatOptions opts;
    opts.function_call.layout = "hanging";
    opts.function_call.space_inside_paren = true;
    opts.function_call.line_length = 20;
    opts.function_declaration.layout = "hanging";
    opts.function_declaration.line_length = 20;
    opts.module.parameter_layout = "hanging";
    opts.spacing.space_inside_parens = true;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: the port list paren after a header import sits with its header", "[formatter][regression]") {
    // N-8: the `(` starting a line after `import p::*;` took the body's
    // indent, so it shared a column with its ports and `);` sat left of it.
    const std::string input = R"SV(module pkg_user
import my_pkg::*;
(
input logic a
);
endmodule
module outer;
module inner import p::*; (input a); endmodule
endmodule
)SV";
    const std::string expected = R"SV(module pkg_user
  import my_pkg::*;
(
  input logic a
);
endmodule
module outer;
  module inner
    import p::*;
  (
    input a
  );
  endmodule
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a select after a semicolonless macro line is not a declaration", "[formatter][regression]") {
    // N-9: the macro and the assignment after it read as one
    // `type name [dim]` declaration, so the select was spaced (`mem [0]`).
    // A macro-named type with a plain `;` after its dimensions still is one.
    const std::string input = R"SV(module m;
initial begin
`INIT_PROLOG
mem[0] = 1;
`MY_STMT
mem[1] = 2;
end
`WORD_T mem3 [4];
endmodule
)SV";
    const std::string expected = R"SV(module m;
  initial begin
    `INIT_PROLOG
    mem[0] = 1;
    `MY_STMT
    mem[1] = 2;
  end
  `WORD_T mem3 [4];
endmodule
)SV";
    FormatOptions opts;
    opts.macros.statement_like.push_back("MY_STMT");
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: rand and randc properties are declarations", "[formatter][regression]") {
    // N-10: `rand`/`randc` qualify a property as `static` does, but neither
    // the declaration aligner nor the declarator-dimension spacing took a
    // line starting with them as a declaration.
    const std::string input = R"SV(class c;
int da[];
rand int ra[];
rand bit [7:0] rb;
randc logic [3:0] rc;
static int s;
endclass
)SV";
    const std::string expected = R"SV(class c;
  int da [];
  rand int ra [];
  rand bit [7:0] rb;
  randc logic [3:0] rc;
  static int s;
endclass
)SV";
    CHECK(format_stable(input) == expected);

    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 12;
    opts.var_declaration.section2_min_width = 8;
    opts.var_declaration.section3_min_width = 6;
    opts.var_declaration.section4_min_width = 4;
    const std::string aligned = R"SV(class c;
  int                 da    []  ;
  rand int            ra    []  ;
  rand bit    [7:0]   rb        ;
  randc logic [3:0]   rc        ;
  static int          s         ;
endclass
)SV";
    CHECK(format_stable(input, opts) == aligned);
}

TEST_CASE("formatter regression: a relational less-equal on the right is spaced as a comparison", "[formatter][regression]") {
    // N-11: only a `<=` inside parentheses counted as a comparison, so the
    // one after `=`/`assign`/another `<=` took the assignment spacing.
    const std::string input = R"SV(module m;
assign z = a <= b;
always_ff @(posedge clk) begin
q <= d;
q <= a <= b;
y = (a <= b) && c;
end
endmodule
)SV";
    FormatOptions opts;
    opts.spacing.binary_operator_spacing = "none";
    CHECK(format_stable(input, opts) == R"SV(module m;
  assign z = a<=b;
  always_ff @(posedge clk) begin
    q <= d;
    q <= a<=b;
    y = (a<=b)&&c;
  end
endmodule
)SV");

    FormatOptions assign_none;
    assign_none.spacing.assignment_operator_spacing = "none";
    CHECK(format_stable(input, assign_none) == R"SV(module m;
  assign z=a <= b;
  always_ff @(posedge clk) begin
    q<=d;
    q<=a <= b;
    y=(a <= b) && c;
  end
endmodule
)SV");
}

TEST_CASE("formatter regression: procedural event at spacing none does not glue an intra-statement event", "[formatter][regression]") {
    // N-12: the `@` option is about the keyword an event control follows
    // (`always@(e)`).  It also closed the `@` up against an assignment
    // operator, a `wait (...)`, a delay and an intra-assignment `repeat`.
    const std::string input = R"SV(module m;
always @(posedge clk) q <= d;
initial begin
q <= @(posedge clk) d;
q = @(negedge clk) d;
q <= repeat (2) @(posedge clk) d;
wait (a) @(posedge clk) q = 2;
#5 @(posedge clk) q = 3;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  always@(posedge clk)
    q <= d;
  initial begin
    q <= @(posedge clk) d;
    q = @(negedge clk) d;
    q <= repeat (2) @(posedge clk) d;
    wait(a) @(posedge clk) q = 2;
    #5 @(posedge clk) q = 3;
  end
endmodule
)SV";
    FormatOptions opts;
    opts.spacing.procedural_event_control_at_spacing = "none";
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a replicated assignment pattern binds its multiplier", "[formatter][regression]") {
    // N-13: only a `{` could open a replication, so `'{4{8'hAA}}` came out
    // as `'{4 {8'hAA}}` beside an untouched `{4{8'hAA}}`.
    const std::string input = R"SV(module m;
initial begin
arr = '{4{8'hAA}};
arr = '{2{a, b}};
arr = '{(N){1'b0}};
cat = {4{8'hAA}};
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  initial begin
    arr = '{4{8'hAA}};
    arr = '{2{a, b}};
    arr = '{(N){1'b0}};
    cat = {4{8'hAA}};
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: min typ max colons are spaced as a range and not a ternary", "[formatter][regression]") {
    // N-14: every colon outside `[...]` took the ternary's spacing, so a
    // min:typ:max triple came out as `#(1 : 2 : 3)`.  It follows
    // `range_colon_spacing` now; a real ternary in a delay keeps its own.
    const std::string input = R"SV(module m;
bufif0 #(1:2:3, 4:5:6) b1 (o1, i1, en);
assign #(1:2:3) w = x;
assign #(c ? 1 : 2) v = x;
specify
(posedge CK => (Q +: D)) = (0.15:0.2:0.25, 0.18:0.22:0.3);
endspecify
endmodule
)SV";
    const std::string expected = R"SV(module m;
  bufif0 #(1:2:3, 4:5:6) b1(o1, i1, en);
  assign #(1:2:3) w = x;
  assign #(c ? 1 : 2) v = x;
  specify
    (posedge CK => (Q +: D)) = (0.15:0.2:0.25, 0.18:0.22:0.3);
  endspecify
endmodule
)SV";
    CHECK(format_stable(input) == expected);

    FormatOptions both;
    both.spacing.range_colon_spacing = "both";
    CHECK(format_stable("module m;\nassign #(1:2:3) w = x;\nendmodule\n", both) ==
          "module m;\n  assign #(1 : 2 : 3) w = x;\nendmodule\n");
}

TEST_CASE("formatter regression: binary spacing none separates a unary operator only where it would merge", "[formatter][regression]") {
    // N-15: every unary operator after a binary one kept a space, although
    // only a pair that lexes as one token (`a- -b`, `a& &b`, `a^ ~b`) needs it.
    const std::string input = R"SV(module m;
assign y = a * -b + c & ~d;
assign y = ~&a | ~|b;
assign y = a - -b;
assign y = a + +b;
assign y = a & &b;
assign y = a && &b;
assign y = a ^ ~b;
assign y = a | |b;
assign y = a - !b;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  assign y = a*-b+c&~d;
  assign y = ~&a|~|b;
  assign y = a- -b;
  assign y = a+ +b;
  assign y = a& &b;
  assign y = a&& &b;
  assign y = a^ ~b;
  assign y = a| |b;
  assign y = a-!b;
endmodule
)SV";
    FormatOptions opts;
    opts.spacing.binary_operator_spacing = "none";
    CHECK(format_stable(input, opts) == expected);
    CHECK(parses_cleanly(expected));
}

TEST_CASE("formatter regression: an instance array spaces its paren like a plain instance", "[formatter][regression]") {
    // N-16: `u_one(` follows `function_call.space_before_paren`, but an
    // array's `(` after `]` fell through to the default space.
    const std::string input = R"SV(module m;
stage u_arr [3:0] (.a(a));
stage u_one (.a(a));
and g_arr [3:0] (o, a, b);
endmodule
)SV";
    const std::string expected = R"SV(module m;
  stage u_arr[3:0](
    .a(a)
  );
  stage u_one(
    .a(a)
  );
  and g_arr[3:0](o, a, b);
endmodule
)SV";
    CHECK(format_stable(input) == expected);

    FormatOptions spaced;
    spaced.function_call.space_before_paren = true;
    const std::string out = format_stable(input, spaced);
    CHECK(out.find("u_arr[3:0] (") != std::string::npos);
    CHECK(out.find("u_one (") != std::string::npos);
}

TEST_CASE("formatter regression: the last enum item's trailing comment is aligned", "[formatter][regression]") {
    // N-17: enum alignment pads through each item's comma, and the last item
    // has none, so its trailing comment sat right after the value.
    const std::string input = R"SV(typedef enum logic [1:0] {
S_IDLE = 2'b00, // idle
S_RUN = 2'b01, // run
S_ERR = 2'b11 // error
} st_t;
)SV";
    const std::string expected = R"SV(typedef enum logic [1:0] {
  S_IDLE = 2'b00 , // idle
  S_RUN  = 2'b01 , // run
  S_ERR  = 2'b11   // error
} st_t;
)SV";
    FormatOptions opts;
    opts.enum_declaration.align = true;
    opts.enum_declaration.enum_name_min_width = 0;
    opts.enum_declaration.enum_value_min_width = 0;
    CHECK(format_stable(input, opts) == expected);
}

// ---------------------------------------------------------------------------
// Round 7 (FORMAT_BUG_FIX7.md)
// ---------------------------------------------------------------------------

TEST_CASE("formatter regression: a list after a format-on marker keeps its indent", "[formatter][regression]") {
    // P-1: a verbatim region ends a line.  The first statement after
    // `// verilog_format: on` was measured and indented from inside the region,
    // so its wrapped list sat at column 0.
    const std::string input = R"SV(module m;
  // verilog_format: off
  wire   a  =  1;
  // verilog_format: on
  sub u (
    .a(a),
    .d(d)
  );
  initial begin
    // verilog_format: off
    x  =  1;
    // verilog_format: on
    foo(aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb, cccccccccccccccccccccccccc, dddddddddddd);
  end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  // verilog_format: off
  wire   a  =  1;
  // verilog_format: on
  sub u(
    .a(a),
    .d(d)
  );
  initial begin
    // verilog_format: off
    x  =  1;
    // verilog_format: on
    foo(
      aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,
      bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,
      cccccccccccccccccccccccccc,
      dddddddddddd
    );
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: ifdef branches that each open a begin count one scope", "[formatter][regression]") {
    // P-2: both branches open the same `begin`; only one is ever compiled, so
    // the scope is opened once.  The second input is the neighbour that must not
    // move: a brace-less body split across the branches.
    {
        const std::string input = R"SV(module m;
`ifdef A
  always_ff @(posedge clk) begin
`else
  always_ff @(posedge clk or negedge rst_n) begin
`endif
    q <= d;
  end
  assign z = 1;
endmodule
)SV";
        const std::string expected = R"SV(module m;
`ifdef A
  always_ff @(posedge clk) begin
`else
  always_ff @(posedge clk or negedge rst_n) begin
`endif
    q <= d;
  end
  assign z = 1;
endmodule
)SV";
        CHECK(format_stable(input) == expected);
    }
    {
        const std::string input = R"SV(module m;
always_comb
`ifdef A
x = 1;
`elsif B
x = 3;
`else
x = 2;
`endif
assign z = 1;
endmodule
)SV";
        const std::string expected = R"SV(module m;
  always_comb
`ifdef A
    x = 1;
`elsif B
    x = 3;
`else
    x = 2;
`endif
  assign z = 1;
endmodule
)SV";
        CHECK(format_stable(input) == expected);
    }
}

TEST_CASE("formatter regression: a case label with a select is not a declaration", "[formatter][regression]") {
    // P-3: `x[0]: y = 1;` read as type `x`, packed dimension `[0]` and
    // declarator `y`.
    const std::string input = R"SV(module m;
logic [7:0] x;
int unsigned cnt;
always_comb begin
case (1'b1)
x[0]: y = 1;
mem[i][j]: y = 2;
default: y = 0;
endcase
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  logic               [7:0]               x                                   ;
  int unsigned                            cnt                                 ;
  always_comb begin
    case (1'b1)
      x[0]: y = 1;
      mem[i][j]: y = 2;
      default: y = 0;
    endcase
  end
endmodule
)SV";
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a parenthesised expression after a clocking or iff paren is not a call", "[formatter][regression]") {
    // P-5: `disable iff (!rst_n) (req && ...)` -- the second paren follows a
    // `)`, which is what an argument list does, but this `)` closes an `iff` or
    // an event control and what follows is an expression.
    {
        const std::string input = R"SV(module m;
ap1: assert property (@(posedge clk) disable iff (!rst_n) (req && some_long_signal_name && another_long_signal_name) |-> ##[1:10] (gnt || timeout_signal_name));
endmodule
)SV";
        const std::string expected = R"SV(module m;
  ap1: assert property (@(posedge clk) disable iff (!rst_n) (req && some_long_signal_name && another_long_signal_name) |-> ##[1:10] (gnt || timeout_signal_name));
endmodule
)SV";
        CHECK(format_stable(input) == expected);
    }
    {
        const std::string input = R"SV(module m;
property p; @(posedge clk) (req, v = d) |-> gnt; endproperty
initial foo(a, b);
endmodule
)SV";
        const std::string expected = R"SV(module m;
  property p;
    @(posedge clk) (req, v = d) |-> gnt;
  endproperty
  initial
    foo(
      a,
      b
    );
endmodule
)SV";
        FormatOptions opts;
        opts.function_call.break_policy = "always";
        CHECK(format_stable(input, opts) == expected);
    }
}

TEST_CASE("formatter regression: an attribute in front of a brace-less body does not end it", "[formatter][regression]") {
    // P-6: the controlled body was measured from the attribute, so it ended
    // at `*)` and the statement behind it lost its level.
    const std::string input = R"SV(module m;
always @(posedge clk)
  (* full_case *) case (x)
    1: y = 1;
  endcase
always_comb (* mark *) if (a) b = 1; else b = 2;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  always @(posedge clk)
    (* full_case *) case (x)
      1: y = 1;
    endcase
  always_comb
    (* mark *) if (a)
      b = 1;
    else
      b = 2;
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a select in a sequence or property is not an unpacked dimension", "[formatter][regression]") {
    // P-7: `b[0];` and `d[0] [=2];` end a property expression, not a
    // declaration, and take no space before `[`.
    const std::string input = R"SV(module m;
property p; @(posedge clk) a |-> b[0]; endproperty
sequence s; !gnt ##1 d[0] [=2]; endsequence
endmodule
)SV";
    const std::string expected = R"SV(module m;
  property p;
    @(posedge clk) a |-> b[0];
  endproperty
  sequence s;
    !gnt ##1 d[0][=2];
  endsequence
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a macro-sized literal takes no space before its base", "[formatter][regression]") {
    // P-8: `` `WIDTH'h3 `` is one literal the way `8'h3` is.
    const std::string input = R"SV(module m;
assign a = `WIDTH'h3;
assign b = `W(8)'d1;
assign d = 8'hFF;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  assign a = `WIDTH'h3;
  assign b = `W(8)'d1;
  assign d = 8'hFF;
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a packed dimension after a struct body stays on the brace line", "[formatter][regression]") {
    // P-9: `} [1:0] pair_t;` -- the dimension belongs with the `}` the way
    // the name does.
    const std::string input = R"SV(typedef struct packed { logic a; logic b; } [1:0] pair_t;
typedef struct packed { logic a; } one_t;
)SV";
    const std::string expected = R"SV(typedef struct packed {
  logic a;
  logic b;
} [1:0] pair_t;
typedef struct packed {
  logic a;
} one_t;
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: the default-width aligner takes user types and qualifiers", "[formatter][regression]") {
    // P-10: only a leading type keyword was a declaration here, so `req_t r;`
    // and `rand int r;` were skipped.  A wider user type widens its own run of
    // declarations and nothing else: the `logic` lines of module `n` keep the
    // keyword column.  `memory u_mem();` is an instance and is left alone.
    const std::string input = R"SV(module m;
logic [7:0] x;
int a;
req_t req_q, req_d;
pkg::cfg_t cfg;
rand bit [3:0] k;
memory u_mem();
endmodule
module n;
logic [7:0] x;
int a;
endmodule
)SV";
    const std::string expected = R"SV(module m;
  logic      [7:0]                         x                             ;
  int                                      a                             ;
  req_t                                    req_q, req_d                  ;
  pkg::cfg_t                               cfg                           ;
  rand bit   [3:0]                         k                             ;
  memory u_mem();
endmodule
module n;
  logic [7:0]                         x                             ;
  int                                 a                             ;
endmodule
)SV";
    FormatOptions opts;
    opts.var_declaration.align = true;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: net types, genvar, class qualifiers and parameterized types are declarations", "[formatter][regression]") {
    // P-11: none of these were aligned.  `virtual task`, `virtual class` and
    // a parameterized instance must stay as they are.
    const std::string input = R"SV(virtual class base;
local int b;
protected bit [3:0] p;
virtual my_if vif;
mailbox #(item_c) mbx;
base_c #(int)::this_t h;
virtual task t();
endtask
endclass
module m;
logic [7:0] x;
tri0 t0;
supply0 gnd;
tri [3:0] tb;
genvar g;
memory #(8) u_mem();
endmodule
)SV";
    const std::string expected = R"SV(virtual class base;
  local int                               b                                   ;
  protected bit       [3:0]               p                                   ;
  virtual my_if                           vif                                 ;
  mailbox #(item_c)                       mbx                                 ;
  base_c #(int)::this_t                   h                                   ;
  virtual task t();
  endtask
endclass
module m;
  logic               [7:0]               x                                   ;
  tri0                                    t0                                  ;
  supply0                                 gnd                                 ;
  tri                 [3:0]               tb                                  ;
  genvar                                  g                                   ;
  memory #(8) u_mem();
endmodule
)SV";
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: an unpacked dimension is spaced the same in every declaration", "[formatter][regression]") {
    // P-12: the space before `[` was lost after `endfunction`, in a typedef
    // and behind an attribute.
    const std::string input = R"SV(module m;
function void g();
endfunction
int q[$], da[];
typedef int aa_t[string];
typedef req_t r_t[4];
(* ram_style = "block" *) reg [7:0] mem[0:255];
logic u[4];
endmodule
)SV";
    const std::string expected = R"SV(module m;
  function void g();
  endfunction
  int q [$], da [];
  typedef int aa_t [string];
  typedef req_t r_t [4];
  (* ram_style = "block" *) reg [7:0] mem [0:255];
  logic u [4];
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a comment after a function header's paren keeps the arguments indented", "[formatter][regression]") {
    // P-13: the comment forces the list onto separate lines; the arguments
    // take the list indent and `);` the header's.
    const std::string input = R"SV(module m;
function int f( // after paren
    int a,
    int b
);
return a + b;
endfunction
endmodule
)SV";
    const std::string expected = R"SV(module m;
  function int f( // after paren
    int a,
    int b
  );
    return a + b;
  endfunction
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: an ifdef inside an event control continues the line", "[formatter][regression]") {
    // P-14: `or negedge rst_n` on its own line inside the `@(...)`.
    const std::string input = R"SV(module m;
always @(posedge clk
`ifdef HAS_RST
         or negedge rst_n
`endif
) begin
q <= d;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  always @(posedge clk
`ifdef HAS_RST
    or negedge rst_n
`endif
  ) begin
    q <= d;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: an own-line comment inside a continued expression takes its indent", "[formatter][regression]") {
    // P-15: the comment sat at statement level between two continuation
    // lines.
    const std::string input = R"SV(module m;
initial begin
y = a + // plus
    b - c
    // own-line in expr
    + d;
// statement level
z = 1;
end
endmodule
)SV";
    const std::string expected = R"SV(module m;
  initial begin
    y = a + // plus
      b - c
      // own-line in expr
      + d;
    // statement level
    z = 1;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: the last port's trailing comment is aligned", "[formatter][regression]") {
    // P-16: the port analogue of N-17 -- the last port has no comma to pad
    // through.
    const std::string input = R"SV(module m (
  input [W-1:0] d, // data in
  output reg [W-1:0] q // data out
);
endmodule
module n (
  input a, // a
  output logic b [3:0] /* blk */
);
endmodule
)SV";
    const std::string expected = R"SV(module m(
  input                   [W-1:0]     d                       , // data in
  output      reg         [W-1:0]     q                         // data out
);
endmodule
module n(
  input                               a                       , // a
  output      logic                   b           [3:0]         /* blk */
);
endmodule
)SV";
    FormatOptions opts;
    opts.port_declaration.align = true;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: a leading comma after a line comment stays with its item", "[formatter][regression]") {
    // P-17: the comma cannot move up behind the `//` comment, and breaking
    // after it left `,` alone on a line.
    const std::string input = R"SV(module n (input a // c
  , input b
);
endmodule
)SV";
    const std::string expected = R"SV(module n(
  input a // c
  , input b
);
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a second declarator inside a parameter list is not an assignment line", "[formatter][regression]") {
    // P-18: `D2 = 4` continues the `parameter` above it; statement alignment
    // padded its `=` as if it began a statement.
    const std::string input = R"SV(module m #(
  parameter W2 = DATA_W * 2, D2 = 4,
  parameter real R = 1.5
) (input logic clk);
assign a = 1;
assign bbb = 2;
endmodule
)SV";
    const std::string expected = R"SV(module m #(
  parameter W2 = DATA_W * 2,
  D2 = 4,
  parameter real R = 1.5
)(
  input logic clk
);
  assign a   = 1;
  assign bbb = 2;
endmodule
)SV";
    FormatOptions opts;
    opts.statement.align = true;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: first_match takes no space before its paren", "[formatter][regression]") {
    // P-20: `first_match(s)` is spelled like a call.
    const std::string input = R"SV(module m;
sequence s; first_match(a ##[1:3] b) ##1 c; endsequence
endmodule
)SV";
    const std::string expected = R"SV(module m;
  sequence s;
    first_match(a ##[1:3] b) ##1 c;
  endsequence
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: begin_newline moves a struct or union brace, by design", "[formatter][regression]") {
    // P-21 was reported as a bug and is not one: `begin_newline` puts every
    // block-opening brace on its own line, a struct or union body's included.
    // An enum's `{` holds a list, not a block, and stays.  Do not "fix" this.
    const std::string input = R"SV(typedef struct packed { logic a; } s_t;
typedef union packed { logic [1:0] a; logic [1:0] b; } u_t;
typedef enum logic { A, B } e_t;
module m;
always_comb begin
x = 1;
end
endmodule
)SV";
    const std::string expected = R"SV(typedef struct packed
{
  logic a;
} s_t;
typedef union packed
{
  logic [1:0] a;
  logic [1:0] b;
} u_t;
typedef enum logic {
  A,
  B
} e_t;
module m;
  always_comb
  begin
    x = 1;
  end
endmodule
)SV";
    FormatOptions opts;
    opts.statement.begin_newline = true;
    CHECK(format_stable(input, opts) == expected);
}

TEST_CASE("formatter regression: the call length check counts the line's indentation", "[formatter][regression]") {
    // P-4: WrapPass measured a line from its first token, so a call was only
    // broken at `line_length` plus its own indent.  The first call ends at
    // column 101 and is broken; the second ends at column 100 and is not.
    {
        const std::string input = R"SV(module m;
function automatic int f();
return compute_something(argument_one_long, argument_two_long, argument_three_long_xyz, argument);
return compute_something(argument_one_long, argument_two_long, argument_three_long_xyz, argumen);
endfunction
endmodule
)SV";
        const std::string expected = R"SV(module m;
  function automatic int f();
    return compute_something(
             argument_one_long,
             argument_two_long,
             argument_three_long_xyz,
             argument
           );
    return compute_something(argument_one_long, argument_two_long, argument_three_long_xyz, argumen);
  endfunction
endmodule
)SV";
        CHECK(format_stable(input) == expected);
    }
    {
        const std::string input = R"SV(package p;
function automatic int f();
if (a) begin
return compute_something(argument_one_long, argument_two_long, argument_three_long_xyz, argume);
return compute_something(argument_one_long, argument_two_long, argument_three_long_xyz, argum);
end
endfunction
endpackage
)SV";
        const std::string expected = R"SV(package p;
  function automatic int f();
    if (a) begin
      return compute_something(
               argument_one_long,
               argument_two_long,
               argument_three_long_xyz,
               argume
             );
      return compute_something(argument_one_long, argument_two_long, argument_three_long_xyz, argum);
    end
  endfunction
endpackage
)SV";
        CHECK(format_stable(input) == expected);
    }
}

TEST_CASE("formatter regression: several leading comments break in one run", "[formatter][regression]") {
    // Q-1: only the first comment was own-line, so each run split one more.
    const std::string input = R"SV(module m;
/* a */ /* b */ /* c */ assign x = 1;
/* d */ // e
assign y = 1; /* f */ /* g */
endmodule
)SV";
    const std::string expected = R"SV(module m;
  /* a */
  /* b */
  /* c */
  assign x = 1;
  /* d */
  // e
  assign y = 1; /* f */ /* g */
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: a select in a later argument is not a declarator's dimension", "[formatter][regression]") {
    // Q-3: `w[i][j]` counted as three names, which made `a[i]` a later
    // declarator of a declaration and spaced it `a [i]`.
    const std::string input = R"SV(module m;
initial begin
f(w[i][j], a[i], a[j]);
x = g(w[i], a[i][j], b[k]);
end
and g1(w[i][j], a[i], a[j]);
logic a [3], b [4];
my_t [N-1:0] c [2], d [3];
endmodule
)SV";
    const std::string expected = R"SV(module m;
  initial begin
    f(w[i][j], a[i], a[j]);
    x = g(w[i], a[i][j], b[k]);
  end
  and g1(w[i][j], a[i], a[j]);
  logic a [3], b [4];
  my_t [N-1:0] c [2], d [3];
endmodule
)SV";
    CHECK(format_stable(input) == expected);
}

TEST_CASE("formatter regression: an expression statement led by a select is not a declaration", "[formatter][regression]") {
    // Q-2: `data[0] < data[1];` read as type `data`, packed dimension `[0]`
    // and a declarator, and was padded into the declaration columns.
    const std::string input = R"SV(class c;
rand bit [7:0] data[4];
my_t [3:0] x;
constraint c_d { data[0] < data[1]; foreach (data[i]) { if (i > 0) data[i] > data[i-1]; } }
endclass
module m;
sequence s; a[0] ##1 b[1]; endsequence
property p; a[0] |-> b[1]; endproperty
endmodule
)SV";
    for (const bool widths : {false, true}) {
        FormatOptions opts;
        opts.var_declaration.align = true;
        if (widths) {
            opts.var_declaration.align_adaptive = true;
            opts.var_declaration.section1_min_width = 20;
            opts.var_declaration.section2_min_width = 20;
            opts.var_declaration.section3_min_width = 20;
            opts.var_declaration.section4_min_width = 16;
        }
        const std::string out = format_stable(input, opts);
        INFO(out);
        CHECK(out.find("    data[0] < data[1];\n") != std::string::npos);
        CHECK(out.find("        data[i] > data[i-1];\n") != std::string::npos);
        CHECK(out.find("    a[0] ##1 b[1];\n") != std::string::npos);
        CHECK(out.find("    a[0] |-> b[1];\n") != std::string::npos);
        // The real declarations beside them are still aligned.
        CHECK(out.find("  my_t [3:0] x;") == std::string::npos);
        CHECK(out.find("  rand bit [7:0] data") == std::string::npos);
    }
}

TEST_CASE("formatter regression: an instance after a keyword-closed item is an instance", "[formatter][regression]") {
    // Q-5: `endclocking` was not an item boundary, so the instance after it
    // was laid out as a call and stayed on one line.
    const std::string input = R"SV(module m;
clocking cb @(posedge clk); input a; endclocking
sub u2(.a(a), .b(b));
clocking cc @(posedge clk); input a; endclocking : cc
sub u3(.a(a), .b(b));
covergroup cg; coverpoint x; endgroup
sub u4(.a(a), .b(b));
property p; a |-> b; endproperty
sub u5(.a(a), .b(b));
sequence s; a ##1 b; endsequence
sub u6(.a(a), .b(b));
specify (a => b) = 1; endspecify
sub u7(.a(a), .b(b));
class k; endclass
sub u8(.a(a), .b(b));
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  clocking cb @(posedge clk);
    input a;
  endclocking
  sub u2(
    .a(a),
    .b(b)
  );
  clocking cc @(posedge clk);
    input a;
  endclocking: cc
  sub u3(
    .a(a),
    .b(b)
  );
  covergroup cg;
    coverpoint x;
  endgroup
  sub u4(
    .a(a),
    .b(b)
  );
  property p;
    a |-> b;
  endproperty
  sub u5(
    .a(a),
    .b(b)
  );
  sequence s;
    a ##1 b;
  endsequence
  sub u6(
    .a(a),
    .b(b)
  );
  specify
    (a => b) = 1;
  endspecify
  sub u7(
    .a(a),
    .b(b)
  );
  class k;
  endclass
  sub u8(
    .a(a),
    .b(b)
  );
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: covergroup and checker formals are a declaration's", "[formatter][regression]") {
    // Q-8: under break_policy "always" the formals were broken per argument
    // like a call's, while a function's beside them were left alone.
    const std::string input = R"SV(module m;
covergroup cg(int lo, int hi) @(posedge clk); coverpoint x; endgroup
function int f(int lo, int hi); return lo; endfunction
initial begin cg_inst = new(1, 2); foo(a, b); end
chk u_chk(clk, a, b);
endmodule
checker chk(logic clk, logic a, b); endchecker
)SV";
    FormatOptions opts;
    opts.function_call.break_policy = "always";
    const std::string expected = R"SV(
module m;
  covergroup cg(int lo, int hi) @(posedge clk);
    coverpoint x;
  endgroup
  function int f(int lo, int hi);
    return lo;
  endfunction
  initial begin
    cg_inst = new(1, 2);
    foo(
      a,
      b
    );
  end
  chk u_chk(
    clk,
    a,
    b
  );
endmodule
checker chk(logic clk, logic a, b);
endchecker
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
}

TEST_CASE("formatter regression: strong and weak take no space, a tagged pattern keeps one", "[formatter][regression]") {
    // Q-13: `strong (s)` beside `first_match(s)`, and `tagged Pair'{...}`
    // written as if `Pair` were a cast's type.
    const std::string input = R"SV(module m;
property p6; req |-> strong(##[1:$] gnt); endproperty
property p7; req |-> weak(gnt[*1:$]); endproperty
initial begin u = tagged Pair '{.a, .b}; v = tagged Valid 5; w = my_t'{1, 2}; x = int'(y); end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  property p6;
    req |-> strong(##[1:$] gnt);
  endproperty
  property p7;
    req |-> weak(gnt[*1:$]);
  endproperty
  initial begin
    u = tagged Pair '{.a, .b};
    v = tagged Valid 5;
    w = my_t'{1, 2};
    x = int'(y);
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: the widest instance connection closes without padding", "[formatter][regression]") {
    // Q-7: a connection holding a select or a call was measured wider than
    // it renders, so the widest row came out as `.a (a[n] )`.
    const std::string input = R"SV(module m;
sub u (.a(a[n]), .b(b));
sub v (.a(x), .b(f(x, y)));
sub w (.c(c[3:0]), .d(d));
sub x (.c({a, b[1]}), .d(d), .e(), .f(p.q[2] + 1), .g(g(h(1), 2)));
endmodule
)SV";
    FormatOptions opts;
    opts.instance.align = true;
    const std::string expected = R"SV(
module m;
  sub u (
    .a (a[n]),
    .b (b   )
  );
  sub v (
    .a (x      ),
    .b (f(x, y))
  );
  sub w (
    .c (c[3:0]),
    .d (d     )
  );
  sub x (
    .c ({a, b[1]} ),
    .d (d         ),
    .e (          ),
    .f (p.q[2] + 1),
    .g (g(h(1), 2))
  );
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));

    // With padding inside the parens the column includes it, or the widest
    // row overruns the others.
    opts.spacing.space_inside_parens = true;
    opts.spacing.space_inside_dimension_brackets = true;
    const std::string spaced = format_stable("module m;\nsub u (.a(a[0]), .b(b[1][2]), .c(c));\nendmodule\n", opts);
    INFO(spaced);
    CHECK(spaced.find("    .a ( a[ 0 ]      ),\n"
                      "    .b ( b[ 1 ][ 2 ] ),\n"
                      "    .c ( c           )\n") != std::string::npos);
}

TEST_CASE("formatter regression: a user-typed parameter joins its alignment group", "[formatter][regression]") {
    // Q-9: `localparam my_t B = 2;` holds two names and was skipped like
    // `packet_t v = f();`, which also split the group around it.
    const std::string input = R"SV(module m #(parameter my_t P = 1, parameter int QQQ = 2) ();
localparam int A = 1;
localparam my_t BBBB = 2;
localparam int CC = 3;
localparam int unsigned DDDDD = 3;
localparam pkg::t D = 4;
parameter state_e INIT = IDLE;
packet_t v = f();
x = 1;
endmodule
)SV";
    FormatOptions opts;
    opts.statement.align = true;
    const std::string expected = R"SV(
module m #(
  parameter my_t P = 1,
  parameter int QQQ = 2
)();
  localparam int A              = 1;
  localparam my_t BBBB          = 2;
  localparam int CC             = 3;
  localparam int unsigned DDDDD = 3;
  localparam pkg::t D           = 4;
  parameter state_e INIT        = IDLE;
  packet_t v = f();
  x = 1;
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
}

TEST_CASE("formatter regression: a DPI alias is not an assignment line", "[formatter][regression]") {
    // Q-11: the `=` of `import "DPI-C" c_name = function ...` was aligned
    // with the assignments below it and set their column.
    const std::string input = R"SV(module m;
import "DPI-C" c_name = function void sv_alias(string s);
export "DPI-C" c_exp = function sv_fn;
assign aaaa = 1;
assign b = 2;
endmodule
)SV";
    FormatOptions opts;
    opts.statement.align = true;
    const std::string expected = R"SV(
module m;
  import "DPI-C" c_name = function void sv_alias(string s);
  export "DPI-C" c_exp = function sv_fn;
  assign aaaa = 1;
  assign b    = 2;
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
}

TEST_CASE("formatter regression: a modport comma follows a multi-line prototype", "[formatter][regression]") {
    // Q-12: the prototype's whole width went into the signal column, so
    // the comma after its closing `)` was padded far to the right.
    const std::string input = R"SV(interface i;
modport mp (input a, import task send(input int x, output bit ok), output b);
modport m2 (input a, import task t(input int x), output bbbbbbbbbbbbb, cc);
endinterface
)SV";
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.modport.align = true;
    opts.modport.align_adaptive = true;
    opts.modport.direction_min_width = 15;
    opts.modport.signal_min_width = 10;
    opts.function_declaration.layout = "block";
    opts.function_declaration.line_length = 13;
    const std::string expected = R"SV(
interface i;
modport mp (
    input          a         ,
    import         task send(
        input int x,
        output bit ok
    ),
    output         b
);
modport m2 (
    input          a         ,
    import         task t(
        input int x
    ),
    output         bbbbbbbbbbbbb, cc
);
endinterface
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));

    // A prototype that stays on its line is still an aligned item.
    FormatOptions plain;
    plain.modport.align = true;
    const std::string out = format_stable(input, plain);
    INFO(out);
    CHECK(out.find("    import task t(input int x) ,\n"
                   // Names sharing a row keep their commas (S-4).
                   "    output bbbbbbbbbbbbb, cc\n") != std::string::npos);
}

TEST_CASE("formatter regression: a comment ending its line stays with the comma before it", "[formatter][regression]") {
    // Q-6: `rst_n, /* reset */` moved in front of the next port and read as
    // describing it.  A comment with code after it on its line still leads
    // that code.
    const std::string input = R"SV(module m (
  input logic clk,
  input logic rst_n, /* reset */
  output logic q, /* lead */ output logic r,
  /* own */ output logic s
);
typedef enum { A, /* ea */
  B, /* eb */ C } e_t;
sub u (.a(a), /* ca */
  .b(b));
initial foo(a, b, /* second */
  c);
endmodule
)SV";
    FormatOptions opts;
    opts.function_call.break_policy = "always";
    const std::string expected = R"SV(
module m(
  input logic clk,
  input logic rst_n, /* reset */
  output logic q,
  /* lead */ output logic r,
  /* own */ output logic s
);
  typedef enum {
    A, /* ea */
    B,
    /* eb */ C
  } e_t;
  sub u(
    .a(a), /* ca */
    .b(b)
  );
  initial
    foo(
      a,
      b, /* second */
      c
    );
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
}

TEST_CASE("formatter regression: a comment between call arguments breaks the whole list", "[formatter][regression]") {
    // Q-10: the comment forced one break and the rest of the list was
    // joined after it at a single indent.
    const std::string input = R"SV(module m;
initial begin
foo(a, // first
  b, c);
x = bar(aaaa, bbbb, // why
  cccc, dddd, eeee);
baz(a, b); // after
qux(a,
  // own
  b);
end
endmodule
)SV";
    const std::string block = R"SV(
module m;
  initial begin
    foo(
      a, // first
      b,
      c
    );
    x = bar(
          aaaa,
          bbbb, // why
          cccc,
          dddd,
          eeee
        );
    baz(a, b); // after
    qux(
      a,
      // own
      b
    );
  end
endmodule
)SV";
    CHECK(format_stable(input) == block.substr(1));

    FormatOptions opts;
    opts.function_call.layout = "hanging";
    const std::string hanging = R"SV(
module m;
  initial begin
    foo(a, // first
        b,
        c);
    x = bar(aaaa,
            bbbb, // why
            cccc,
            dddd,
            eeee);
    baz(a, b); // after
    qux(a,
        // own
        b);
  end
endmodule
)SV";
    CHECK(format_stable(input, opts) == hanging.substr(1));
}

TEST_CASE("formatter regression: var alignment leaves a body port declaration to the port aligner", "[formatter][regression]") {
    // Q-4: with port_declaration.align off, a non-ANSI `input a, b;` was
    // half placed by the variable aligner: `input       a,  b          ;`.
    const std::string input = R"SV(module m(a, b, c, d, e, f);
input a, b;
input [7:0] c;
output d;
input logic e;
output logic [3:0] f, g;
wire [3:0] w;
function automatic int fn;
input [3:0] x;
reg [3:0] t;
fn = x;
endfunction
endmodule
)SV";
    FormatOptions opts;
    opts.var_declaration.align = true;
    const std::string expected = R"SV(
module m(
  a,
  b,
  c,
  d,
  e,
  f
);
  input a, b;
  input [7:0] c;
  output d;
  input logic e;
  output logic [3:0] f, g;
  wire   [3:0]                         w                             ;
  function automatic int fn;
    input [3:0] x;
    reg    [3:0]                         t                             ;
    fn = x;
  endfunction
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));

    // With the port aligner on, the same lines take its columns as before.
    opts.port_declaration.align = true;
    const std::string both = R"SV(
module m(
  a,
  b,
  c,
  d,
  e,
  f
);
  input                               a                       , b                       ;
  input                   [7:0]       c                       ;
  output                              d                       ;
  input       logic                   e                       ;
  output      logic       [3:0]       f                       , g                       ;
  wire   [3:0]                         w                             ;
  function automatic int fn;
    input                   [3:0]       x                       ;
    reg    [3:0]                         t                             ;
    fn = x;
  endfunction
endmodule
)SV";
    CHECK(format_stable(input, opts) == both.substr(1));
}

TEST_CASE("formatter regression: an attribute-led declaration joins the declaration columns", "[formatter][regression]") {
    // Q-14: the line starts with `(*`, so the declaration aligners passed
    // over it and it sat unaligned between its neighbours.
    const std::string input = R"SV(module m;
logic [7:0] a;
(* keep *) logic dbg;
logic signed [3:0] bb;
(* ram_style = "block", very_long_attribute_name = "x" *) reg [7:0] mem [256];
(* dont_touch *) sub u_s (.a(a));
(* keep *) my_t [1:0] ut = 0;
(* full_case *) case (x) 1: y = 1; endcase
endmodule
)SV";
    FormatOptions opts;
    opts.var_declaration.align = true;
    const std::string defaults = R"SV(
module m;
  logic [7:0]                         a                             ;
  (* keep *) logic                    dbg                           ;
  logic signed [3:0]                  bb                            ;
  (* ram_style = "block", very_long_attribute_name = "x" *) reg [7:0] mem [256];
  (* dont_touch *) sub u_s(
    .a(a)
  );
  (* keep *) my_t [1:0]               ut                            = 0;
  (* full_case *) case (x)
    1: y = 1;
  endcase
endmodule
)SV";
    CHECK(format_stable(input, opts) == defaults.substr(1));

    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    const std::string sections = R"SV(
module m;
  logic               [7:0]               a                                   ;
  (* keep *) logic                        dbg                                 ;
  logic signed        [3:0]               bb                                  ;
  (* ram_style = "block", very_long_attribute_name = "x" *) reg [7:0] mem [256];
  (* dont_touch *) sub u_s(
    .a(a)
  );
  (* keep *) my_t     [1:0]               ut                  = 0             ;
  (* full_case *) case (x)
    1: y = 1;
  endcase
endmodule
)SV";
    CHECK(format_stable(input, opts) == sections.substr(1));
}

TEST_CASE("formatter regression: every comparison, power and equivalence operator is binary", "[formatter][regression]") {
    // R-3: `===`, `!==`, `==?`, `!=?`, `**` and `<->` were missing from the
    // binary-operator set, so a line broken at one got no continuation
    // indent and `binary_operator_spacing` skipped them.
    const std::string input = R"SV(module m;
assign a = b && // c1
c;
assign a = b === // c2
c;
assign a = b // c3
<-> c;
assign a = b ** // c4
c;
assign a = b // c5
!=? c;
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  assign a = b && // c1
    c;
  assign a = b === // c2
    c;
  assign a = b // c3
    <-> c;
  assign a = b ** // c4
    c;
  assign a = b // c5
    !=? c;
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));

    FormatOptions opts;
    opts.spacing.binary_operator_spacing = "none";
    const std::string tight_input = R"SV(module m;
assign a = b != c; assign a = b !== c; assign a = b ==? c; assign a = b !=? c;
assign a = b ** c; assign a = b <-> c; assign a = b === -c; assign a = b ** -1;
endmodule
)SV";
    const std::string tight = R"SV(
module m;
  assign a = b!=c;
  assign a = b!==c;
  assign a = b==?c;
  assign a = b!=?c;
  assign a = b**c;
  assign a = b<->c;
  assign a = b===-c;
  assign a = b**-1;
endmodule
)SV";
    CHECK(format_stable(tight_input, opts) == tight.substr(1));
}

TEST_CASE("formatter regression: a method named by a keyword is called without a space", "[formatter][regression]") {
    // R-5: `unique`, `and`, `or` and `xor` are array methods as well as
    // keywords, and the call rule only knew identifier-like callees.
    const std::string input = R"SV(module m;
initial begin
u = q.unique(); v = q.and(); w = q.or() with (item); x = q.xor(); y = q.min();
unique case (a) 1: b = 1; endcase
@(a or (b));
end
sub u_s (.a(a), .b (b));
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  initial begin
    u = q.unique();
    v = q.and();
    w = q.or() with (item);
    x = q.xor();
    y = q.min();
    unique case (a)
      1: b = 1;
    endcase
    @(a or (b));
  end
  sub u_s(
    .a(a),
    .b(b)
  );
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));

    FormatOptions opts;
    opts.function_call.space_before_paren = true;
    CHECK(format_stable("module m;\ninitial u = q.unique();\nendmodule\n", opts) ==
          "module m;\n  initial\n    u = q.unique ();\nendmodule\n");
}

TEST_CASE("formatter regression: an unpacked dimension is spaced whatever type leads the declaration", "[formatter][regression]") {
    // R-9: `logic a [2]` but `tri t[2]`, `local int x[2]`, `my_if.mp i[2]`,
    // `mailbox #(int) mb[2]` and `struct {...} s[4]`.
    const std::string input = R"SV(module m;
logic a[2]; my_t mt[2];
tri t[2]; supply0 g[2]; var v[2]; interconnect ic[2]; my_if.mp ifa[2]; mailbox #(int) mb[2];
struct { int x; } sb[4];
enum {P, Q} en[2];
initial begin
foo.bar[3:0] = x; w = obj.arr[i].fld[3:0]; q[0] <= d; #(D) m[0] = x; x = a.b[2];
end
sub #(.W(8)) u[3:0] (.a(a));
endmodule
class c;
static local int z[2]; local int x[2]; protected bit y[2];
endclass
)SV";
    const std::string expected = R"SV(
module m;
  logic a [2];
  my_t mt [2];
  tri t [2];
  supply0 g [2];
  var v [2];
  interconnect ic [2];
  my_if.mp ifa [2];
  mailbox #(int) mb [2];
  struct {
    int x;
  } sb [4];
  enum {
    P,
    Q
  } en [2];
  initial begin
    foo.bar[3:0] = x;
    w = obj.arr[i].fld[3:0];
    q[0] <= d;
    #(D) m[0] = x;
    x = a.b[2];
  end
  sub #(.W(8)) u[3:0](
    .a(a)
  );
endmodule
class c;
  static local int z [2];
  local int x [2];
  protected bit y [2];
endclass
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a comment after an opening brace keeps its space", "[formatter][regression]") {
    // R-10: `typedef enum {// open` -- the no-space-after-`{` rule reached
    // the comment, which a struct's block brace already escaped.
    const std::string input = R"SV(module m;
typedef enum { // open
A0, A1 } e_t;
typedef struct packed { // s
logic a; } s_t;
assign x = {/* c */ a, b}; assign y = {/* only */}; assign w = {a, /* d */ b};
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  typedef enum { // open
    A0,
    A1
  } e_t;
  typedef struct packed { // s
    logic a;
  } s_t;
  assign x = { /* c */ a, b};
  assign y = {/* only */};
  assign w = {a, /* d */ b};
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: the item after a statement block's brace starts a line", "[formatter][regression]") {
    // R-2: `} name` was kept together for every `}`, which is right only for
    // the declarator after a struct, union or enum body.
    const std::string input = R"SV(class c;
constraint c2 { foreach (arr[i]) { arr[i] > 0; } a == 1; if (a) { b == 1; } else { b == 2; } d == 3; a -> { b == 1; } e == 4; }
covergroup cg;
cx: cross cp, cp2 { bins x = binsof(cp); } cy: cross cp, cp2;
endgroup
struct { int x; } sa; struct packed { logic y; } [1:0] sb; enum { A, B } ea;
endclass
)SV";
    const std::string expected = R"SV(
class c;
  constraint c2 {
    foreach (arr[i]) {
      arr[i] > 0;
    }
    a == 1;
    if (a) {
      b == 1;
    }
    else {
      b == 2;
    }
    d == 3;
    a -> {
      b == 1;
    }
    e == 4;
  }
  covergroup cg;
    cx: cross cp, cp2 {
      bins x = binsof(cp);
    }
    cy: cross cp, cp2;
  endgroup
  struct {
    int x;
  } sa;
  struct packed {
    logic y;
  } [1:0] sb;
  enum {
    A,
    B
  } ea;
endclass
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: an empty constraint or coverpoint body stays closed", "[formatter][regression]") {
    // R-4: `constraint c {}` was split after `{`, and its `}` -- closing a
    // brace nothing had classified as a block -- took a continuation indent.
    const std::string input = R"SV(class c;
constraint c6 {}
constraint c7 { }
constraint c9 {
// own
}
covergroup cg; cp: coverpoint a {} cq: coverpoint b { } cr: coverpoint c; endgroup
endclass
)SV";
    const std::string expected = R"SV(
class c;
  constraint c6 {}
  constraint c7 {}
  constraint c9 {
    // own
  }
  covergroup cg;
    cp: coverpoint a {}
    cq: coverpoint b {}
    cr: coverpoint c;
  endgroup
endclass
)SV";
    CHECK(format_stable(input) == expected.substr(1));
    FormatOptions opts;
    opts.statement.begin_newline = true;
    const std::string bnl = format_stable(input, opts);
    CHECK(bnl.find("constraint c6 {}\n") != std::string::npos);
    CHECK(bnl.find("  {\n    // own\n  }\n") != std::string::npos);
}

TEST_CASE("formatter regression: a dist list inside an inline constraint stays on its line", "[formatter][regression]") {
    // R-6: the inline `with { ... }` of a condition is kept on one line, and
    // the `dist` list inside it was expanded anyway.
    const std::string input = R"SV(class c;
task t();
if (!randomize(x) with { x < 9; x dist {1:=1, 2:=2}; }) $error("f");
randomize(x) with { x dist {1:=1, 2:=2}; };
endtask
endclass
)SV";
    const std::string expected = R"SV(
class c;
  task t();
    if (!randomize(x) with { x < 9; x dist {1 := 1, 2 := 2}; })
      $error("f");
    randomize(x) with {
      x dist {
        1 := 1,
        2 := 2
      };
    };
  endtask
endclass
)SV";
    CHECK(format_stable(input) == expected.substr(1));
    FormatOptions opts;
    opts.statement.begin_newline = true;
    CHECK(format_stable(input, opts).find("with { x < 9; x dist {1 := 1, 2 := 2}; })\n") != std::string::npos);
}

TEST_CASE("formatter regression: a block event's begin and end are not blocks", "[formatter][regression]") {
    // R-1: `@@(begin f)` names the entry of a block.  Treated as a block
    // keyword, the unmatched `begin` indented everything after it.
    const std::string input = R"SV(module m;
covergroup cg3 @@(begin f1 or end f2); cp: coverpoint a; endgroup
covergroup cg4 @@( begin  f1 ); endgroup
always @(posedge clk) begin a <= b; end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  covergroup cg3 @@(begin f1 or end f2);
    cp: coverpoint a;
  endgroup
  covergroup cg4 @@(begin f1);
  endgroup
  always @(posedge clk) begin
    a <= b;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
    FormatOptions opts;
    opts.statement.begin_newline = true;
    CHECK(format_stable(input, opts).find("  covergroup cg3 @@(begin f1 or end f2);\n") != std::string::npos);
}

TEST_CASE("formatter regression: a broken qualified call indents from where the callee starts", "[formatter][regression]") {
    // R-7: block layout measured from the last name segment, so a longer
    // `obj.sub.` / `pkg::cls::` prefix pushed the arguments further right.
    FormatOptions opts;
    opts.function_call.break_policy = "always";
    const std::string input = R"SV(module m;
initial begin
foo(a, b);
uvm_config_db#(virtual my_if)::set(a, b);
obj.sub.meth(a, b);
x = pkg::cls::fn(a, b);
arr[i].q.push_back(a, b);
y = a.b(c, d).e(f, g);
end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  initial begin
    foo(
      a,
      b
    );
    uvm_config_db #(virtual my_if)::set(
      a,
      b
    );
    obj.sub.meth(
      a,
      b
    );
    x = pkg::cls::fn(
          a,
          b
        );
    arr[i].q.push_back(
      a,
      b
    );
    y = a.b(
          c,
          d
        ).e(
          f,
          g
        );
  end
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
}

TEST_CASE("formatter regression: a modport-typed declaration joins the declaration columns", "[formatter][regression]") {
    // R-11: `my_if.mp ifp;` -- an interface and its modport are one type.
    // The `.` made the line read as a statement and it was left unaligned.
    FormatOptions opts;
    opts.var_declaration.align = true;
    const std::string input = R"SV(module m;
my_if.mp ifp;
my_if ifq;
logic [3:0] a;
my_if.mp arr [2];
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  my_if.mp                                 ifp                           ;
  my_if                                    ifq                           ;
  logic      [3:0]                         a                             ;
  my_if.mp                                 arr [2]                       ;
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
    // A member select is not a type: nothing names a declarator after it.
    const std::string stmt = "module m;\n  initial begin\n    obj.field = 1;\n    obj.run();\n  end\nendmodule\n";
    CHECK(format_stable(stmt, opts) == stmt);
}

TEST_CASE("formatter regression: the operand of a prefix operator split by a directive is a continuation", "[formatter][regression]") {
    // R-12: `` `ifdef INV ~ `endif clk; `` -- `clk` is the operand of `~`, and
    // it fell back to the statement's indent because `~` is no binary operator.
    const std::string input = R"SV(module m;
assign y = `ifdef INV ~ `endif clk;
assign z = a &
`ifdef X
b &
`endif
c;
always_comb begin
x =
`ifdef A
1;
`else
2;
`endif
y = 3;
end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  assign y =
`ifdef INV
    ~
`endif
    clk;
  assign z = a &
`ifdef X
    b &
`endif
    c;
  always_comb begin
    x =
`ifdef A
      1;
`else
      2;
`endif
    y = 3;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a block comment's later lines move with its first", "[formatter][regression]") {
    // R-8: only the line holding `/*` was re-indented, so the ` *` column
    // under it stayed wherever the source had it.
    const std::string input = R"SV(module m;
/**
 * doc comment
 *   keeps stars
 */
logic a;
always_comb begin
        /*
         * deep

      less
         */
a = 2; /* t1
   t2 */
end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  /**
   * doc comment
   *   keeps stars
   */
  logic a;
  always_comb begin
    /*
     * deep

  less
     */
    a = 2; /* t1
   t2 */
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
    // Already where it belongs: nothing moves.
    CHECK(format_stable(expected.substr(1)) == expected.substr(1));
}

TEST_CASE("formatter regression: a select on a concatenation has no space", "[formatter][regression]") {
    // S-6: the rule that closes up an index listed a name, `]` and `)` as the
    // token before it, and not the `}` of a concatenation.  A struct body ends
    // no expression, so its packed dimension keeps the gap.
    const std::string input = R"SV(module m;
assign a = {b, c} [3:0];
assign d = {2{e}}[1];
initial x = '{1, 2} [0];
typedef struct packed {logic f;} [3:0] s_t;
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  assign a = {b, c}[3:0];
  assign d = {2{e}}[1];
  initial
    x = '{1, 2}[0];
  typedef struct packed {
    logic f;
  } [3:0] s_t;
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a repetition in a transition bin is not a dimension", "[formatter][regression]") {
    // S-7: `[->2]` and `[=1:3]` were recognised as repetitions only inside a
    // property, so in a covergroup `->` and `=` were spaced as operators.
    const std::string input = R"SV(module m;
covergroup cg @(posedge clk);
cp: coverpoint v {
bins t1 = (3 [*2] => 4);
bins t2 = (1 [->2] => 0);
bins t3 = (2 [=1:3] => 5), ([1:2] => 3);
bins b[4] = {[0:15]};
}
endgroup
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  covergroup cg @(posedge clk);
    cp: coverpoint v {
      bins t1 = (3 [*2] => 4);
      bins t2 = (1 [->2] => 0);
      bins t3 = (2 [=1:3] => 5), ([1:2] => 3);
      bins b[4] = {[0:15]};
    }
  endgroup
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
    // The dimension options pad a range and leave a repetition alone.
    FormatOptions opts;
    opts.spacing.space_inside_dimension_brackets = true;
    const std::string padded = format_stable(input, opts);
    CHECK(padded.find("(3 [*2] => 4)") != std::string::npos);
    CHECK(padded.find("(1 [->2] => 0)") != std::string::npos);
    CHECK(padded.find("{[ 0:15 ]}") != std::string::npos);
}

TEST_CASE("formatter regression: a virtual interface's specialization is not a header's parameter list", "[formatter][regression]") {
    // S-5: the optional `interface` keyword made `#(16)` look like an
    // interface declaration's parameter port list, one parameter per line.
    const std::string input = R"SV(class drv;
virtual interface bus_if #(16) vif;
virtual bus_if #(16) vif2;
virtual interface bus_if #(.AW(16), .DW(8)).mp vif3;
function new(virtual interface bus_if #(16) v, int a); endfunction
endclass
interface bus_if #(parameter W = 8) (input clk);
endinterface
)SV";
    const std::string expected = R"SV(
class drv;
  virtual interface bus_if #(16) vif;
  virtual bus_if #(16) vif2;
  virtual interface bus_if #(.AW(16), .DW(8)).mp vif3;
  function new(virtual interface bus_if #(16) v, int a);
  endfunction
endclass
interface bus_if #(
  parameter W = 8
)(
  input clk
);
endinterface
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a port list led by an interface port keeps names on their declaration", "[formatter][regression]") {
    // S-3: names stayed on their declaration's line only when the *first*
    // port started with a keyword, so `bus_if.master bus` ahead of the
    // directed ports split every later name onto its own line.
    const std::string input = R"SV(module m (bus_if.master bus, input logic clk, rst_n, output logic [7:0] q, r);
endmodule
module m3 (my_t t, u, interface g, h, input x, y);
endmodule
module m4 (.a(x), b, input c, d);
endmodule
module m2 (a, b, c);
endmodule
)SV";
    const std::string expected = R"SV(
module m(
  bus_if.master bus,
  input logic clk, rst_n,
  output logic [7:0] q, r
);
endmodule
module m3(
  my_t t, u,
  interface g, h,
  input x, y
);
endmodule
module m4(
  .a(x),
  b,
  input c, d
);
endmodule
module m2(
  a,
  b,
  c
);
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: rand join in a production closes no block", "[formatter][regression]") {
    // S-1: `join` is a block closer by kind.  After `rand` in a randsequence
    // production it closes nothing, and every later line lost a level.
    const std::string input = R"SV(module m;
initial begin
randsequence (main)
main: rand join first second;
alt: rand join (0.5) first second;
first: { a = 1; };
endsequence
fork a = 1; join
c = 1;
end
assign d = e;
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  initial begin
    randsequence (main)
      main: rand join first second;
      alt: rand join (0.5) first second;
      first: {
        a = 1;
      };
    endsequence
    fork
      a = 1;
    join
    c = 1;
  end
  assign d = e;
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: an empty instance port list spaces like a filled one", "[formatter][regression]") {
    // S-11: the instance gap was keyed on the list WrapPass tags, and an
    // empty `()` is no list, so `u0()` sat beside `u1 (`.
    const std::string input = R"SV(module m;
sub u0 ();
sub u1 (.a(a));
sub u2 (), u3 (.*);
sub #() u4 ();
sub arr [3:0] ();
initial f();
endmodule
)SV";
    FormatOptions opts;
    opts.instance.align = true;
    const std::string expected = R"SV(
module m;
  sub u0 ();
  sub u1 (
    .a (a)
  );
  sub u2 (), u3 (
    .*
  );
  sub #() u4 ();
  sub arr[3:0] ();
  initial
    f();
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
    // Without the option an instance spaces like a call, array or not.
    const std::string plain = format_stable(input);
    CHECK(plain.find("  sub u0();\n") != std::string::npos);
    CHECK(plain.find("  sub arr[3:0]();\n") != std::string::npos);
}

TEST_CASE("formatter regression: statement.align leaves enum members to their own option", "[formatter][regression]") {
    // S-13: `NAME = value,` read as an assignment, so `statement.align`
    // aligned enum values with `enum_declaration.align` off -- in runs that
    // restarted at every member without a value.
    const std::string input = R"SV(module m;
typedef enum logic [2:0] { IDLE = 3'd0, RUNNING = 3'd1, DONE, ERR = 3'd7, LAST_ONE = 3'd6 } st_t;
initial begin
a = 1;
long_name = 2;
end
endmodule
)SV";
    FormatOptions opts;
    opts.statement.align = true;
    const std::string expected = R"SV(
module m;
  typedef enum logic [2:0] {
    IDLE = 3'd0,
    RUNNING = 3'd1,
    DONE,
    ERR = 3'd7,
    LAST_ONE = 3'd6
  } st_t;
  initial begin
    a         = 1;
    long_name = 2;
  end
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
    // The enum option still aligns them, with or without statement.align.
    FormatOptions only_enum;
    only_enum.enum_declaration.align = true;
    FormatOptions both = only_enum;
    both.statement.align = true;
    const std::string enum_part = format_stable(input, only_enum);
    const std::string both_part = format_stable(input, both);
    CHECK(enum_part.substr(0, enum_part.find("  initial")) == both_part.substr(0, both_part.find("  initial")));
}

TEST_CASE("formatter regression: a clocking or covergroup name is separated from its event", "[formatter][regression]") {
    // S-8: the `@` option is about the keyword an event control follows.  It
    // also glued the event to the name a clocking block or covergroup declares.
    const std::string input = R"SV(module m;
clocking cb @(posedge clk);
endclocking
global clocking gcb @(posedge clk);
endclocking
covergroup cg @(posedge clk);
endgroup
covergroup cg2 @@(begin f);
endgroup
always @(posedge clk) q <= d;
endmodule
)SV";
    FormatOptions opts;
    opts.spacing.procedural_event_control_at_spacing = "none";
    const std::string expected = R"SV(
module m;
  clocking cb @(posedge clk);
  endclocking
  global clocking gcb @(posedge clk);
  endclocking
  covergroup cg @(posedge clk);
  endgroup
  covergroup cg2 @@(begin f);
  endgroup
  always@(posedge clk)
    q <= d;
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
    opts.spacing.procedural_event_control_at_spacing = "after";
    const std::string after = format_stable(input, opts);
    CHECK(after.find("  clocking cb @ (posedge clk);\n") != std::string::npos);
    CHECK(after.find("  covergroup cg @(posedge clk);\n") != std::string::npos);
    CHECK(after.find("  always@ (posedge clk)\n") != std::string::npos);
}

TEST_CASE("formatter regression: a leading-comma connection row keeps the paren column", "[formatter][regression]") {
    // S-12: a comma that follows a `//` comment stays in front of the next
    // connection.  The `(` column was computed as if it were not there.
    const std::string input = R"SV(module m;
sub u (
    .clk(clk) // clock
  , .rst_n(rst_n) // reset
  , .q(q)
);
sub v (.a(a), .bb(b));
endmodule
)SV";
    FormatOptions opts;
    opts.instance.align = true;
    const std::string expected = R"SV(
module m;
  sub u (
    .clk     (clk  ) // clock
    , .rst_n (rst_n) // reset
    , .q     (q    )
  );
  sub v (
    .a  (a),
    .bb (b)
  );
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
}

TEST_CASE("formatter regression: port and modport alignment keep a row's names together", "[formatter][regression]") {
    // S-4: only the first name of `input logic clk, rst_n` was measured, so
    // its comma went to the comma column and the other names trailed it:
    // `clk          , rst_n,`.  The names sharing a row are one field.
    FormatOptions opts;
    opts.port_declaration.align = true;
    const std::string input = R"SV(module m (input logic clk, rst_n, output logic [7:0] q, r, input wire en);
endmodule
)SV";
    const std::string expected = R"SV(
module m(
  input       logic                   clk, rst_n              ,
  output      logic       [7:0]       q, r                    ,
  input       wire                    en
);
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));

    FormatOptions mp;
    mp.modport.align = true;
    const std::string mp_input = R"SV(interface bus_if;
modport master (output addr, wdata, valid, input rdata, ready);
modport s (input a, output bb);
endinterface
)SV";
    const std::string mp_expected = R"SV(
interface bus_if;
  modport master (
    output addr, wdata, valid,
    input  rdata, ready
  );
  modport s (
    input  a ,
    output bb
  );
endinterface
)SV";
    CHECK(format_stable(mp_input, mp) == mp_expected.substr(1));
}

TEST_CASE("formatter regression: a declarator after a commented comma is a continuation", "[formatter][regression]") {
    // S-2: one declarator per line with a comment each.  The comment forces
    // the break, and the next declarator landed at the statement's own
    // indent, where it read as a new statement.
    const std::string input = R"SV(module m;
logic [3:0] g, // gg
            h; // hh
localparam A = 1, // a
           B = 2;
assign x = y, // c
       z = w ? 1 : 0;
sub u1 (.a(a)), // c
    u2 (.b(b));
always_comb begin
case (s)
S1, // c
S2: x = 1;
default: x = 0;
endcase
end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  logic [3:0] g, // gg
    h; // hh
  localparam A = 1, // a
    B = 2;
  assign x = y, // c
    z = w ? 1 : 0;
  sub u1(
    .a(a)
  ), // c
  u2(
    .b(b)
  );
  always_comb begin
    case (s)
      S1, // c
      S2: x = 1;
      default: x = 0;
    endcase
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a property's case is laid out as a block", "[formatter][regression]") {
    // S-9: each item of a property `case` ends its line at its `;`, but the
    // keyword was treated as an operator, so the first item stayed on the
    // header's line and the others were not indented.
    const std::string input = R"SV(module m;
property p;
@(posedge clk) case (mode)
2'd0: req |-> gnt;
default: 1'b1;
endcase
endproperty
property r; @(posedge clk) a |-> case (m) 0: a; default: if (b) c else d; endcase; endproperty
a1: assert property (@(posedge clk) case (m) 0: a; default: b; endcase);
a2: assert property (@(posedge clk) if (a) b else c);
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  property p;
    @(posedge clk) case (mode)
      2'd0: req |-> gnt;
      default: 1'b1;
    endcase
  endproperty
  property r;
    @(posedge clk) a |-> case (m)
      0: a;
      default: if (b) c else d;
    endcase;
  endproperty
  a1: assert property (@(posedge clk) case (m) 0: a; default: b; endcase);
  a2: assert property (@(posedge clk) if (a) b else c);
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a randsequence production is not laid out as statements", "[formatter][regression]") {
    // S-10: `if`, `else` and `repeat` in a production choose between
    // productions.  They were broken like the procedural keywords, and a
    // brace-less `initial randsequence` ended its body at the first `;`.
    const std::string input = R"SV(module m;
initial
randsequence (main)
main: first second;
first: if (a) x else y;
second: repeat (3) x;
third: case (b) 0: x; default: y; endcase;
fourth: { if (a) b = 1; else b = 2; };
endsequence
initial begin
repeat (3) c = 1;
end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  initial
    randsequence (main)
      main: first second;
      first: if (a) x else y;
      second: repeat (3) x;
      third: case (b)
        0: x;
        default: y;
      endcase;
      fourth: {
        if (a)
          b = 1;
        else
          b = 2;
      };
    endsequence
  initial begin
    repeat (3)
      c = 1;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: default sequence in a bin opens no block", "[formatter][regression]") {
    // T-1: `bins b = default sequence;` is the catch-all transition bin.  The
    // keyword was read as the start of a sequence declaration, and with no
    // `endsequence` to close it every later line gained an indent level.
    const std::string input = R"SV(module m;
covergroup cg @(posedge clk);
cp: coverpoint a {
bins seq = default sequence;
bins lo = {0};
}
cq: coverpoint b;
endgroup
sequence s;
a ##1 b;
endsequence
default clocking cb @(posedge clk);
endclocking
logic z;
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  covergroup cg @(posedge clk);
    cp: coverpoint a {
      bins seq = default sequence;
      bins lo = {0};
    }
    cq: coverpoint b;
  endgroup
  sequence s;
    a ##1 b;
  endsequence
  default clocking cb @(posedge clk);
  endclocking
  logic z;
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: space_before_paren finds the header past a type(x) return type", "[formatter][regression]") {
    // T-10: the first `(` after `function` was taken to be the header's.  With
    // a `type(x)` return type the space went into the type and the function
    // name got none.
    FormatOptions opts;
    opts.function_declaration.space_before_paren = true;
    const std::string input = R"SV(class c;
function automatic type(x) tfn(int a); endfunction
function void g(int a = h(1)); y = h(2); endfunction
extern function type(x) proto(int a);
endclass
)SV";
    const std::string expected = R"SV(
class c;
  function automatic type(x) tfn (int a);
  endfunction
  function void g (int a = h(1));
    y = h(2);
  endfunction
  extern function type(x) proto (int a);
endclass
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));
}

TEST_CASE("formatter regression: an attribute keeps a space before the connection it annotates", "[formatter][regression]") {
    // T-12: `(* keep *) .p(x)` was closed up to `(* keep *).p(x)`, the only
    // place an attribute instance had no space after it.
    const std::string input = R"SV(module m;
sub u ((* keep *) .p(x), (* a = 1 *).q(y), .r(z));
sub #((* b *) .W(1)) v (.p(x));
(* keep *) logic w;
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  sub u(
    (* keep *) .p(x),
    (* a = 1 *) .q(y),
    .r(z)
  );
  sub #((* b *) .W(1)) v(
    .p(x)
  );
  (* keep *) logic w;
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a config use clause keeps cell:config closed up", "[formatter][regression]") {
    // T-9: the colon of `lib.cell:cfg` was spaced like a conditional's.
    const std::string input = R"SV(config c;
design lib.top;
default liblist lib;
instance top.a use lib.cell:cfg;
cell lib.b use other : config;
endconfig
module m;
assign y = s ? a:b;
initial begin : blk
x = c ? 1:0;
end
endmodule
)SV";
    const std::string expected = R"SV(
config c;
  design lib.top;
  default liblist lib;
  instance top.a use lib.cell:cfg;
  cell lib.b use other:config;
endconfig
module m;
  assign y = s ? a : b;
  initial begin: blk
    x = c ? 1 : 0;
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a coverpoint's concatenation is not its bins body", "[formatter][regression]") {
    // T-2: any `{` after `coverpoint` was laid out as the body, so the
    // concatenation being sampled was broken one signal per line and the real
    // body after it landed on a line of its own.
    const std::string input = R"SV(module m;
covergroup cg @(posedge clk);
cp: coverpoint {a, b};
cq: coverpoint {a, b} iff (v) {
bins z = {0};
}
cr: coverpoint x + {a, b} { bins lo = {0}; bins hi = {1}; }
cs: coverpoint a[1:0] { bins lo = {0}; }
ct: coverpoint f(x) iff (v) { bins lo = {0}; }
endgroup
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  covergroup cg @(posedge clk);
    cp: coverpoint {a, b};
    cq: coverpoint {a, b} iff (v) {
      bins z = {0};
    }
    cr: coverpoint x + {a, b} {
      bins lo = {0};
      bins hi = {1};
    }
    cs: coverpoint a[1:0] {
      bins lo = {0};
    }
    ct: coverpoint f(x) iff (v) {
      bins lo = {0};
    }
  endgroup
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));

    // The body brace still moves under begin_newline; the concatenation's
    // does not.
    FormatOptions opts;
    opts.statement.begin_newline = true;
    const std::string allman = R"SV(
module m;
  covergroup cg @(posedge clk);
    cq: coverpoint {a, b} iff (v)
    {
      bins z = {0};
    }
  endgroup
endmodule
)SV";
    CHECK(format_stable(R"SV(module m;
covergroup cg @(posedge clk);
cq: coverpoint {a, b} iff (v) { bins z = {0}; }
endgroup
endmodule
)SV", opts) == allman.substr(1));
}

TEST_CASE("formatter regression: a single argument with a comment breaks like a list", "[formatter][regression]") {
    // T-8: the comment rule for calls needed more than one argument, so
    // `foo(a // why` kept its argument on the call's line and dropped the `)`
    // to the statement's indent.
    const std::string input = R"SV(module m;
initial begin
foo(a // why
);
foo(a, // why
b);
foo(a); // after
foo(a /* in */);
bar(
// own line
a);
end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  initial begin
    foo(
      a // why
    );
    foo(
      a, // why
      b
    );
    foo(a); // after
    foo(a /* in */);
    bar(
      // own line
      a
    );
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: a comment after the opener turns a hanging list into a block", "[formatter][regression]") {
    // T-4: a hanging list lines up under its first item, on the opener's
    // line.  With a comment there instead, the items hung one column to the
    // right of nothing and the `)` stayed on the last one.
    FormatOptions params;
    params.module.parameter_layout = "hanging";
    const std::string module_input = R"SV(module a #( // params
parameter A = 1,
parameter B = 2
) (input x);
endmodule
module b #(parameter A = 1, // one
parameter B = 2
) (input x);
endmodule
)SV";
    const std::string module_expected = R"SV(
module a #( // params
  parameter A = 1,
  parameter B = 2
)(
  input x
);
endmodule
module b #(parameter A = 1, // one
           parameter B = 2)(
  input x
);
endmodule
)SV";
    CHECK(format_stable(module_input, params) == module_expected.substr(1));

    FormatOptions calls;
    calls.function_call.layout = "hanging";
    const std::string call_input = R"SV(module m;
initial begin
foo( // why
a, b);
foo(a, // why
b);
end
endmodule
)SV";
    const std::string call_expected = R"SV(
module m;
  initial begin
    foo( // why
      a,
      b
    );
    foo(a, // why
        b);
  end
endmodule
)SV";
    CHECK(format_stable(call_input, calls) == call_expected.substr(1));
}

TEST_CASE("formatter regression: a line led by ? after a directive continues its statement", "[formatter][regression]") {
    // T-7: after a conditional directive a line led by `&` or by the
    // conditional's `:` kept its continuation indent, and one led by `?` fell
    // back to the statement's own.
    const std::string input = R"SV(module m;
assign q = en
`ifdef INV
? ~d
`else
? d
`endif
: 0;
assign r = en
`ifdef INV
& ~d
`endif
;
always_comb
casez (s)
3'b1??: y = 1;
default: y = 0;
endcase
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  assign q = en
`ifdef INV
    ? ~d
`else
    ? d
`endif
    : 0;
  assign r = en
`ifdef INV
    & ~d
`endif
  ;
  always_comb
    casez (s)
      3'b1??: y = 1;
      default: y = 0;
    endcase
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: space_inside_paren pads a constructor like any call", "[formatter][regression]") {
    // T-11: `new` is a keyword, so its `(` is not an argument list to the
    // wrapping passes, and the option skipped it -- in a call and in the
    // constructor's own header.
    FormatOptions opts;
    opts.function_call.space_inside_paren = true;
    const std::string input = R"SV(class c;
function new(string name); super.new(name); endfunction
function void g(int a); x = new(a); y = f(a); q = new[4]; z = new; w = (a + b); endfunction
endclass
)SV";
    const std::string expected = R"SV(
class c;
  function new( string name );
    super.new( name );
  endfunction
  function void g( int a );
    x = new( a );
    y = f( a );
    q = new[4];
    z = new;
    w = (a + b);
  endfunction
endclass
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));

    const std::string plain = R"SV(
class c;
  function new(string name);
    super.new(name);
  endfunction
endclass
)SV";
    CHECK(format_stable(R"SV(class c;
function new(string name); super.new(name); endfunction
endclass
)SV") == plain.substr(1));
}

TEST_CASE("formatter regression: port alignment keeps an explicit port whole", "[formatter][regression]") {
    // T-3: `.name(expr)` after a direction was read as a declaration: the `.`
    // went to the type column and a `[` inside the expression was padded apart
    // as an unpacked dimension.
    FormatOptions opts;
    opts.port_declaration.align = true;
    const std::string input = R"SV(module dut (input logic [3:0] x, input .named({a, b}), output .o(y[1]), input wire z);
endmodule
)SV";
    const std::string expected = R"SV(
module dut(
  input       logic       [3:0]       x                       ,
  input                               .named({a, b})          ,
  output                              .o(y[1])                ,
  input       wire                    z
);
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));

    const std::string plain = R"SV(
module dut(
  input logic [3:0] x,
  input .named({a, b}),
  output .o(y[1]),
  input wire z
);
endmodule
)SV";
    CHECK(format_stable(input) == plain.substr(1));
}

TEST_CASE("formatter regression: a comment inside a pattern or concatenation breaks it at every element", "[formatter][regression]") {
    // T-5: a pattern stays on its line, and a `//` inside one forced a single
    // break: the first element stayed glued to the opener and the rest landed
    // at the statement's continuation indent.
    const std::string input = R"SV(module m;
localparam int SIZES [3] = '{
8,  // byte
16, // half
32  // word
};
assign bus = {hdr, // header
payload};
localparam t T = '{'{1, // one
2}, '{3, 4}};
initial begin
u = '{
// own line
a, b};
w = {a, b}; // after
v = '{a, /* in */ b};
p = '{mode: A, sub: '{en: 1, w: 4}, default: 0};
end
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  localparam int SIZES [3] = '{
    8, // byte
    16, // half
    32 // word
  };
  assign bus = {
    hdr, // header
    payload
  };
  localparam t T = '{'{
    1, // one
    2
  }, '{3, 4}};
  initial begin
    u = '{
      // own line
      a,
      b
    };
    w = {a, b}; // after
    v = '{a, /* in */ b};
    p = '{mode : A, sub : '{en : 1, w : 4}, default : 0};
  end
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}

TEST_CASE("formatter regression: wrap_end_else_clauses joins end else as it joins } else", "[formatter][regression]") {
    // T-6: the option was documented as joining `end else` when off, and
    // only `} else` followed it: `end` ended its line under either value.
    // Both kinds of block follow it now.  The default is on, which is the
    // layout `end` always had.
    const std::string input = R"SV(module m;
always_comb begin
if (a) begin
x = 1;
end else begin
x = 0;
end
end
constraint c { if (a) { x == 1; } else { x == 0; } }
endmodule
)SV";
    const std::string joined = R"SV(
module m;
  always_comb begin
    if (a) begin
      x = 1;
    end else begin
      x = 0;
    end
  end
  constraint c {
    if (a) {
      x == 1;
    } else {
      x == 0;
    }
  }
endmodule
)SV";
    const std::string wrapped = R"SV(
module m;
  always_comb begin
    if (a) begin
      x = 1;
    end
    else begin
      x = 0;
    end
  end
  constraint c {
    if (a) {
      x == 1;
    }
    else {
      x == 0;
    }
  }
endmodule
)SV";
    FormatOptions opts;
    opts.statement.wrap_end_else_clauses = false;
    CHECK(format_stable(input, opts) == joined.substr(1));
    opts.statement.wrap_end_else_clauses = true;
    CHECK(format_stable(input, opts) == wrapped.substr(1));
    CHECK(format_stable(input) == wrapped.substr(1));
}

TEST_CASE("formatter regression: an alignment group takes the widest line, not the widest prefix plus the widest field", "[formatter][regression]") {
    // U-10: the group column was the longest `assign `/case-label prefix plus
    // the longest LHS field, which belong to different lines.  `defparam` has
    // no prefix and a long field, so beside `assign` every `=` moved a whole
    // `assign ` right of the longest line.
    FormatOptions opts;
    opts.statement.align = true;
    const std::string input = R"SV(module m;
assign a = 1;
defparam u.P = 1, u.QQQQ = 2;
assign zz = 1;
always_comb case (s)
pkg::RUN, pkg::WAIT: n = 1;
default: next_state = 0;
endcase
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  assign a     = 1;
  defparam u.P = 1, u.QQQQ = 2;
  assign zz    = 1;
  always_comb
    case (s)
      pkg::RUN, pkg::WAIT: n = 1;
      default: next_state    = 0;
    endcase
endmodule
)SV";
    CHECK(format_stable(input, opts) == expected.substr(1));

    // Lines that share a prefix are unchanged.
    const std::string only_assign = R"SV(
module m;
  assign a  = 1, b = 2;
  assign zz = 1;
endmodule
)SV";
    CHECK(format_stable("module m;\nassign a = 1, b = 2;\nassign zz = 1;\nendmodule\n", opts) == only_assign.substr(1));

    // A minimum field still starts after each line's own prefix.
    opts.statement.lhs_min_width = 6;
    const std::string with_min = R"SV(
module m;
  assign a      = 1;
  assign zz     = 1;
endmodule
)SV";
    CHECK(format_stable("module m;\nassign a = 1;\nassign zz = 1;\nendmodule\n", opts) == with_min.substr(1));
}

TEST_CASE("formatter regression: an index after a property's else is not a dimension", "[formatter][regression]") {
    // U-7: `if (a) s[1] else s[2];` has no `;` between its branches, so the
    // search for a declaration's start ran back past `else` and found two
    // names -- a type and a declarator -- and spaced `s [2]`.
    const std::string input = R"SV(module m;
property p;
@(posedge clk) if (s[0]) s[1] else s[2];
endproperty
property q;
if (a) b[0] else c[1];
endproperty
int d [2];
always_comb if (a) y[0] = 1; else y[1] = 2;
endmodule
)SV";
    const std::string expected = R"SV(
module m;
  property p;
    @(posedge clk) if (s[0]) s[1] else s[2];
  endproperty
  property q;
    if (a) b[0] else c[1];
  endproperty
  int d [2];
  always_comb
    if (a)
      y[0] = 1;
    else
      y[1] = 2;
endmodule
)SV";
    CHECK(format_stable(input) == expected.substr(1));
}
