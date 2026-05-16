#include <catch2/catch_test_macros.hpp>
#include "features/formatter.hpp"

TEST_CASE("formatter: function calls support block layout", "[formatter]") {
    FormatOptions opts;
    opts.function.break_policy = "always";
    opts.function.layout = "block";
    opts.indent_size = 4;

    CHECK(format_source("result = my_func(arg1, arg2, arg3);\n", opts) ==
          "result = my_func(\n"
          "    arg1,\n"
          "    arg2,\n"
          "    arg3\n"
          ");\n");
}

TEST_CASE("formatter: function calls support hanging layout", "[formatter]") {
    FormatOptions opts;
    opts.function.break_policy = "always";
    opts.function.layout = "hanging";

    CHECK(format_source("result = my_func(arg1, arg2, arg3);\n", opts) ==
          "result = my_func(arg1,\n"
          "                 arg2,\n"
          "                 arg3);\n");
}

TEST_CASE("formatter: var declaration initializers not aligned by statement pass", "[formatter]") {
    FormatOptions opts;
    opts.statement.align = true;
    opts.var_declaration.align = false;

    // Two var decls with different name lengths — should NOT be aligned by align_assign_pass
    CHECK(format_source(
        "logic [7:0] dout = 8'hFF;\n"
        "logic [8:0] douteeeeeeeeeeeeeeeeeee = 8'hFF;\n", opts) ==
        "logic [7:0] dout = 8'hFF;\n"
        "logic [8:0] douteeeeeeeeeeeeeeeeeee = 8'hFF;\n");
}

TEST_CASE("formatter: user-defined type var decl not aligned by statement pass", "[formatter]") {
    FormatOptions opts;
    opts.statement.align = true;
    opts.var_declaration.align = false;

    // User-defined type var decls should NOT be aligned by align_assign_pass
    CHECK(format_source(
        "packet_t test_init = 8'hFF;\n"
        "packet_t test_init2 = 8'hFF;\n", opts) ==
        "packet_t test_init = 8'hFF;\n"
        "packet_t test_init2 = 8'hFF;\n");
}

TEST_CASE("formatter: define macro body not reformatted", "[formatter]") {
    FormatOptions opts;
    opts.function.break_policy = "always";
    const std::string src =
        "`define print_bytes(ARR, STARTBYTE, NUMBYTES) \\\n"
        "    for (int ii=STARTBYTE; ii<STARTBYTE+NUMBYTES; ii++) begin \\\n"
        "        $display(\"0x%x \", ARR[ii]); \\\n"
        "    end\n";
    CHECK(format_source(src, opts) == src);
}

TEST_CASE("formatter: adaptive var declaration section4 does not use block max", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 8;
    opts.var_declaration.section2_min_width = 8;
    opts.var_declaration.section3_min_width = 8;
    opts.var_declaration.section4_min_width = 8;
    opts.default_indent_level_inside_module_block = 0;

    CHECK(format_source("module top;\n"
                        "logic [7:0] a = 1;\n"
                        "logic [7:0] b = very_long_expression;\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "logic   [7:0]   a       = 1     ;\n"
          "logic   [7:0]   b       = very_long_expression;\n"
          "endmodule\n");
}

TEST_CASE("formatter: user-defined output port type stays in type section", "[formatter]") {
    FormatOptions opts;
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = true;
    opts.port_declaration.section1_min_width = 6;
    opts.port_declaration.section2_min_width = 12;
    opts.port_declaration.section3_min_width = 8;
    opts.port_declaration.section4_min_width = 8;
    opts.default_indent_level_inside_module_block = 0;

    std::string formatted = format_source(
        "module top(test, VSS);\n"
        "output logic unsigned [0:0] VDD, VSS;\n"
        "output                                        packet_t            [0:0] test          , VSS                                     ;\n"
        "endmodule\n", opts);

    CHECK(formatted.find("output                    packet_t") == std::string::npos);
    CHECK(formatted.find("output packet_t") != std::string::npos);
    CHECK(formatted.find("output packet_t   [0:0]") != std::string::npos);
}
