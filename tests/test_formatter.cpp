#include <catch2/catch_test_macros.hpp>
#include "features/formatter.hpp"

TEST_CASE("formatter: function calls support block layout", "[formatter]") {
    FormatOptions opts;
    opts.function.break_policy = "always";
    opts.function.layout = "block";
    opts.indent_size = 4;

    CHECK(format_source("result = my_func(arg1, arg2, arg3);\n", opts) ==
          "result = my_func(\n"
          "             arg1,\n"
          "             arg2,\n"
          "             arg3\n"
          "         );\n");
}

TEST_CASE("formatter: function calls inside if conditions use configured layout", "[formatter]") {
    FormatOptions opts;
    opts.function.break_policy = "always";
    opts.function.layout = "block";
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "if(add_number(.a(a), .b(b), .result(result))) begin\n"
                        "a = 3;\n"
                        "end\n"
                        "if(add_number(.a(a), .b(b), .result(result)))\n"
                        "a = 3;\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "if (add_number(\n"
          "        .a(a),\n"
          "        .b(b),\n"
          "        .result(result)\n"
          "    )) begin\n"
          "    a = 3;\n"
          "end\n"
          "if (add_number(\n"
          "        .a(a),\n"
          "        .b(b),\n"
          "        .result(result)\n"
          "    ))\n"
          "    a = 3;\n"
          "endmodule\n");
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

TEST_CASE("formatter: inline line comments stay on their original line", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "assign a = b; // keep this inline\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "assign a = b; // keep this inline\n"
          "endmodule\n");
}

TEST_CASE("formatter: inline block comments stay on their original line", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "assign a = b; /* keep this inline */\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "assign a = b; /* keep this inline */\n"
          "endmodule\n");
}

TEST_CASE("formatter: block function call after inline block comment indents from call", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.function.break_policy = "always";
    opts.function.layout = "block";

    CHECK(format_source("module top;\n"
                        "always_comb begin\n"
                        "/**/ add_number(.a(a3), .b(b), .result(result));\n"
                        "end\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "always_comb begin\n"
          "    /**/ add_number(\n"
          "             .a(a3),\n"
          "             .b(b),\n"
          "             .result(result)\n"
          "         );\n"
          "end\n"
          "endmodule\n");
}

TEST_CASE("formatter: declaration comment separator is one space", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "logic a; // one space before comment\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "logic a                             ; // one space before comment\n"
          "endmodule\n");
}

TEST_CASE("formatter: adaptive var declaration section4 does not use block max", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 8;
    opts.var_declaration.section2_min_width = 8;
    opts.var_declaration.section3_min_width = 8;
    opts.var_declaration.section4_min_width = 8;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "logic [7:0] a = 1;\n"
                        "logic [7:0] b = very_long_expression;\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "logic   [7:0]   a       = 1     ;\n"
          "logic   [7:0]   b       = very_long_expression;\n"
          "endmodule\n");
}

TEST_CASE("formatter: net declarations are aligned with var declarations", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "logic [7:0] data;\n"
                        "wire [1:0] addr;\n"
                        "logic address;\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "logic [7:0]                         data                          ;\n"
          "wire  [1:0]                         addr                          ;\n"
          "logic                               address                       ;\n"
          "endmodule\n");
}

TEST_CASE("formatter: var decls with initializers aligned together", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    opts.statement.align = true;
    opts.default_indent_level_inside_outmost_block = 0;

    std::string result = format_source(
        "module top;\n"
        "logic [7:0] dout = 8'hFF;\n"
        "logic [8:0] douteeeeeee = 8'hFF;\n"
        "endmodule\n", opts);
    INFO("formatted:\n" << result);
    // Both lines must be aligned — compact output on either line is a bug
    CHECK(result.find("logic [7:0] dout") == std::string::npos);
    CHECK(result.find("logic [8:0] douteeeeeee") == std::string::npos);
}

TEST_CASE("formatter: var decls idempotent when already wide-formatted", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    opts.statement.align = true;
    opts.default_indent_level_inside_outmost_block = 0;

    // Input mimics demo file: one compact + one already wide (formatter should normalize both)
    std::string input =
        "module top;\n"
        "logic [7:0] dout = 8'hFF;\n"
        "logic               [8:0]               douteeeeeee         = 8'hFF         ;\n"
        "endmodule\n";
    std::string result = format_source(input, opts);
    INFO("formatted:\n" << result);
    // Both lines must use aligned format
    CHECK(result.find("logic [7:0] dout") == std::string::npos);
    CHECK(result.find("logic [8:0] douteeeeeee") == std::string::npos);
    // Formatter must be idempotent
    CHECK(format_source(result, opts) == result);
}

TEST_CASE("formatter: var decls after port decls aligned", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    opts.port_declaration.align = true;
    opts.port_declaration.section1_min_width = 6;
    opts.port_declaration.section2_min_width = 20;
    opts.port_declaration.section3_min_width = 20;
    opts.port_declaration.section4_min_width = 20;
    opts.port_declaration.section5_min_width = 20;
    opts.statement.align = true;
    opts.port.non_ansi_port_per_line_enabled = true;
    opts.port.non_ansi_port_per_line = 3;
    opts.default_indent_level_inside_outmost_block = 0;

    // Mimics memory_top.sv: non-ANSI module with port decls followed by var decls
    std::string input =
        "module top(a, b);\n"
        "output logic [0:0] a;\n"
        "output packet_tttttttttttttt [0:0] b;\n"
        "\n"
        "logic [7:0] dout = 8'hFF;\n"
        "logic [8:0] douteeeeeee = 8'hFF;\n"
        "endmodule\n";
    std::string result = format_source(input, opts);
    INFO("formatted:\n" << result);
    // Both var decl lines must be aligned wide
    CHECK(result.find("logic [7:0] dout") == std::string::npos);
    CHECK(result.find("logic [8:0] douteeeeeee") == std::string::npos);
}

TEST_CASE("formatter: var decl with backtick macro dim is idempotent", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    opts.statement.align = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.compact_indexing_and_selections = true;
    opts.safe_mode = true;

    // `WIDTH is a macro — formatter must not expand it (would change non-whitespace content)
    std::string input =
        "`define WIDTH 32\n"
        "module top;\n"
        "logic [7:0] dout = 8'hFF;\n"
        "logic [`WIDTH - 1:0] data;\n"
        "endmodule\n";
    std::string result;
    REQUIRE_NOTHROW(result = format_source(input, opts));
    INFO("formatted:\n" << result);
    // Must not expand the macro
    CHECK(result.find("`WIDTH") != std::string::npos);
    // Idempotent
    CHECK(format_source(result, opts) == result);
}

TEST_CASE("formatter: user-defined output port type stays in type section", "[formatter]") {
    FormatOptions opts;
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = true;
    opts.port_declaration.section1_min_width = 6;
    opts.port_declaration.section2_min_width = 12;
    opts.port_declaration.section3_min_width = 8;
    opts.port_declaration.section4_min_width = 8;
    opts.default_indent_level_inside_outmost_block = 0;

    std::string formatted = format_source(
        "module top(test, VSS);\n"
        "output logic unsigned [0:0] VDD, VSS;\n"
        "output                                        packet_t            [0:0] test          , VSS                                     ;\n"
        "endmodule\n", opts);

    INFO("formatted output:\n" << formatted);
    CHECK(formatted.find("output                    packet_t") == std::string::npos);
    CHECK(formatted.find("output packet_t") != std::string::npos);
    CHECK(formatted.find("output packet_t   [0:0]") != std::string::npos);
}

TEST_CASE("formatter: var declarations inside typedef struct are aligned", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;

    std::string formatted = format_source(
        "typedef struct {\n"
        "logic [7:0] addr;\n"
        "logic valid;\n"
        "} packet_t;\n", opts);

    INFO("formatted output:\n" << formatted);
    CHECK(formatted.find("logic [7:0] addr") == std::string::npos);
    CHECK(formatted.find("logic               [7:0]") != std::string::npos);
    CHECK(formatted.find("logic                                   valid") != std::string::npos);
}

TEST_CASE("formatter: outmost indent option applies to module interface and package", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("interface bus;\nlogic valid;\nendinterface\n", opts) ==
          "interface bus;\n"
          "logic valid;\n"
          "endinterface\n");

    CHECK(format_source("package pkg;\nparameter int W = 8;\nendpackage\n", opts) ==
          "package pkg;\n"
          "parameter int W = 8;\n"
          "endpackage\n");
}

TEST_CASE("formatter: var declarations after expanded module header are aligned", "[formatter]") {
    FormatOptions opts;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    opts.port.non_ansi_port_per_line_enabled = true;
    opts.port.non_ansi_port_per_line = 1;
    opts.default_indent_level_inside_outmost_block = 0;

    std::string formatted = format_source(
        "module top(a, b, c);\n"
        "logic [7:0] testxrp;\n"
        "logic d;\n"
        "endmodule\n", opts);

    INFO("formatted output:\n" << formatted);
    CHECK(formatted.find("logic [7:0] testxrp") == std::string::npos);
    CHECK(formatted.find("logic               [7:0]") != std::string::npos);
    CHECK(formatted.find("logic                                   d") != std::string::npos);
}

TEST_CASE("formatter: instance port name width is measured from dot to paren", "[formatter]") {
    FormatOptions opts;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    CHECK(format_source("module top;\n"
                        "memory u_mem(.address(addr), .data_in(data), .chip_en(en));\n"
                        "endmodule\n", opts) ==
          "module top;\n"
          "memory u_mem (\n"
          "    .address  (addr      ),\n"
          "    .data_in  (data      ),\n"
          "    .chip_en  (en        )\n"
          ");\n"
          "endmodule\n");
}

TEST_CASE("formatter: expands consecutive single-line instances after multiline instance", "[formatter]") {
    FormatOptions opts;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    std::string formatted = format_source(
        "module top;\n"
        "memory #(.MEM_SIZE(3)) u_memory(\n"
        "                           .address(addr),\n"
        "                           .data_in(intercontest)\n"
        "                       );\n"
        "memory u_mem(.i_clk(testxrp), .address(addressss), .data_in(threeshit));\n"
        "memory u_mem1(.address(), .data_in(zzzry), .dataut());\n"
        "endmodule\n", opts);

    INFO("formatted output:\n" << formatted);
    CHECK(formatted.find("memory u_mem(.i_clk") == std::string::npos);
    CHECK(formatted.find("memory u_mem1(.address") == std::string::npos);
    CHECK(formatted.find("memory u_mem (\n") != std::string::npos);
    CHECK(formatted.find("memory u_mem1 (\n") != std::string::npos);
}

TEST_CASE("formatter: expands instances after declarations like memory_top", "[formatter]") {
    FormatOptions opts;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    opts.port.non_ansi_port_per_line_enabled = true;
    opts.port.non_ansi_port_per_line = 3;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    std::string formatted = format_source(
        "module memory_top #(parameter int WIDTH = 4, parameter int DEPTH = 8) (\n"
        "    i_clk, i_rst_n, i_data,\n"
        "    VDD, VSS, test\n"
        ");\n"
        "input i_clk;\n"
        "logic [7:0] testxrp;\n"
        "logic [7:0] intercontest;\n"
        "assign d = a + 1;\n"
        "memory #(.MEM_SIZE(3)) u_memory(\n"
        "                           .address(addr),\n"
        "                           .data_in(intercontest),\n"
        "                           .read_write(read_wsssrite),\n"
        "                           .chip_en(tt)\n"
        "                       );\n"
        "memory u_mem(.i_clk(testxrp), .address(addressss), .data_in(threeshit), .chip_en(chip_en), .www333(www333), .zzfuk(zzfuk));\n"
        "endmodule\n", opts);

    INFO("formatted output:\n" << formatted);
    CHECK(formatted.find("memory u_mem(.i_clk") == std::string::npos);
    CHECK(formatted.find("memory u_mem (\n") != std::string::npos);
}
