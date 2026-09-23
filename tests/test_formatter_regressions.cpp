// Regressions found by stress-formatting macro-heavy, UVM, Verilog-95,
// OpenTitan and CIRCT-style RTL.  Every case asserts the exact output and
// that formatting it again changes nothing; cases that once produced code
// slang rejects also parse the output.
#include "features/formatter.hpp"
#include "config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/syntax/SyntaxTree.h>
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
                 "    /* block\n"
                 "   comment */\n"
                 "    reg k;\n"
                 "    /* another\n"
                 "   one */\n"
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
  rand int payload[4];
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
  rand int q[4];
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
