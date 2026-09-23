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

TEST_CASE("formatter regression: a spaced conditional after a literal stays a conditional", "[formatter][regression]") {
    const std::string out = format_stable("assign y = 4'hc ? a : b;\n");
    CHECK(out == "assign y = 4'hc ? a : b;\n");
}
