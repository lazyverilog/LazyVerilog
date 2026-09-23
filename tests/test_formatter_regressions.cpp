// Regressions found by stress-formatting macro-heavy, UVM, Verilog-95,
// OpenTitan and CIRCT-style RTL.  Every case asserts the exact output and
// that formatting it again changes nothing; cases that once produced code
// slang rejects also parse the output.
#include "features/formatter.hpp"
#include "config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/syntax/SyntaxTree.h>
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

TEST_CASE("formatter regression: a spaced conditional after a literal stays a conditional", "[formatter][regression]") {
    const std::string out = format_stable("assign y = 4'hc ? a : b;\n");
    CHECK(out == "assign y = 4'hc ? a : b;\n");
}
