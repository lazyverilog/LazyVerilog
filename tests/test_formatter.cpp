#include "features/formatter.hpp"
#include "config.hpp"
#include <cctype>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include <vector>

TEST_CASE("formatter: function calls support block layout", "[formatter]") {
    FormatOptions opts;
    opts.function.break_policy = "always";
    opts.function.layout = "block";
    opts.indent_size = 4;

    CHECK(format_source("result = my_func(arg1, arg2, arg3);\n", opts) == "result = my_func(\n"
                                                                          "             arg1,\n"
                                                                          "             arg2,\n"
                                                                          "             arg3\n"
                                                                          "         );\n");
}

TEST_CASE("formatter: autoarg comment in multiline module header is safe", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.module.parameter_layout = "hanging";
    opts.module.non_ansi_port_per_line_enabled = true;
    opts.module.non_ansi_port_per_line = 3;

    std::string input =
        "module memory_top #( parameter int WIDTH = 4, parameter int DEPTH = 8 ) ( /*autoarg*/\n"
        "    i_clk, i_rst_n,\n"
        "    i_data, i_data2\n"
        ");\n"
        "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(input, opts));
    CHECK(formatted == "module memory_top #(parameter int WIDTH = 4,\n"
                       "                    parameter int DEPTH = 8)( /*autoarg*/\n"
                       "    i_clk, i_rst_n, i_data,\n"
                       "    i_data2\n"
                       ");\n"
                       "endmodule\n");
}

TEST_CASE("formatter: module parameter layout block", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.module.parameter_layout = "block";
    opts.port_declaration.align = false;

    CHECK(format_source("module register #(parameter type T = logic [7:0], parameter int DEPTH = "
                        "8, parameter int SIZE = 4, parameter logic [7:0] TABLE[SIZE] = "
                        "'{8'h00, 8'h11, 8'h22, 8'h33})(input logic clk);\n"
                        "endmodule\n",
                        opts) ==
          "module register #(\n"
          "    parameter type T = logic [7:0],\n"
          "    parameter int DEPTH = 8,\n"
          "    parameter int SIZE = 4,\n"
          "    parameter logic [7:0] TABLE[SIZE] = '{8'h00, 8'h11, 8'h22, 8'h33}\n"
          ")(\n"
          "    input logic clk\n"
          ");\n"
          "endmodule\n");
}

TEST_CASE("formatter: module parameter layout hanging", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.module.parameter_layout = "hanging";
    opts.port_declaration.align = false;

    CHECK(format_source("module register #(parameter type T = logic [7:0], parameter int DEPTH = "
                        "8, parameter int SIZE = 4, parameter logic [7:0] TABLE[SIZE] = "
                        "'{8'h00, 8'h11, 8'h22, 8'h33})(input logic clk);\n"
                        "endmodule\n",
                        opts) ==
          "module register #(parameter type T = logic [7:0],\n"
          "                  parameter int DEPTH = 8,\n"
          "                  parameter int SIZE = 4,\n"
          "                  parameter logic [7:0] TABLE[SIZE] = '{8'h00, 8'h11, 8'h22, 8'h33})(\n"
          "    input logic clk\n"
          ");\n"
          "endmodule\n");
}

TEST_CASE("formatter: module parameter comments with commas are not split", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.module.parameter_layout = "hanging";

    const std::string src = "module m #(\n"
                            "  // Size of the instruction memory, in bytes\n"
                            "  parameter int ImemSizeByte = 4096,\n"
                            "  // Size of the data memory, in bytes\n"
                            "  parameter int DmemSizeByte = 4096\n"
                            ")();\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    CHECK(formatted.find("// Size of the instruction memory, in bytes") != std::string::npos);
    CHECK(formatted.find("\nin bytes\n") == std::string::npos);
    CHECK(formatted.find("// Size of the data memory, in bytes") != std::string::npos);
}

TEST_CASE("formatter: block module parameter comments do not emit trailing whitespace",
          "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.module.parameter_layout = "block";

    const std::string src = "module m #(\n"
                            "  // Size of the instruction memory, in bytes\n"
                            "  parameter int ImemSizeByte = 4096,\n"
                            "  // Size of the data memory, in bytes\n"
                            "  parameter int DmemSizeByte = 4096\n"
                            ")();\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    CHECK(formatted.find("// Size of the instruction memory, in bytes") != std::string::npos);
    CHECK(formatted.find("\nin bytes\n") == std::string::npos);
    std::istringstream lines(formatted);
    std::string line;
    while (std::getline(lines, line))
        CHECK((line.empty() || (line.back() != ' ' && line.back() != '\t')));
}

TEST_CASE("formatter: multiline ANSI module header preserves line comments", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = false;

    CHECK(format_source("module adc_ctrl_reg_top(input clk_i, input rst_ni, "
                        "input tlul_pkg::tl_h2d_t tl_i, output tlul_pkg::tl_d2h_t tl_o, // To HW\n"
                        "output adc_ctrl_reg_pkg::adc_ctrl_reg2hw_t reg2hw, // Write\n"
                        "input adc_ctrl_reg_pkg::adc_ctrl_hw2reg_t hw2reg, // Read\n"
                        "\n"
                        "// Integrity check errors\n"
                        "output logic intg_err_o);\n"
                        "endmodule\n",
                        opts) == "module adc_ctrl_reg_top(\n"
                                 "    input                                           clk_i,\n"
                                 "    input                                           rst_ni,\n"
                                 "    input     tlul_pkg::tl_h2d_t                    tl_i,\n"
                                 "    output    tlul_pkg::tl_d2h_t                    tl_o, // To HW\n"
                                 "    output    adc_ctrl_reg_pkg::adc_ctrl_reg2hw_t   reg2hw, // Write\n"
                                 "    input     adc_ctrl_reg_pkg::adc_ctrl_hw2reg_t   hw2reg, // Read\n"
                                 "    // Integrity check errors\n"
                                 "    output    logic                                 intg_err_o\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: instance parameter comments do not trip safe mode", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.instance.align = true;

    const std::string src = "module tb;\n"
                            "prim_lfsr #(\n"
                            "  .LfsrType(LfsrType),\n"
                            "  .DefaultSeed(k'(SEED)),\n"
                            "  // Preserve this comment before StatePermEn.\n"
                            "  .StatePermEn(1'b1),\n"
                            "  .MaxLenSVA(1'b1)\n"
                            ") i_prim_lfsr (\n"
                            "  .clk_i(clk)\n"
                            ");\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    size_t comment_pos = formatted.find("// Preserve this comment before StatePermEn.");
    size_t param_pos = formatted.find(".StatePermEn");
    REQUIRE(comment_pos != std::string::npos);
    REQUIRE(param_pos != std::string::npos);
    CHECK(comment_pos < param_pos);
}

TEST_CASE("formatter: imported parameterized ANSI header is idempotent", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.tab_align = true;
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = true;
    opts.module.parameter_layout = "hanging";

    const std::string src = "module m import p::*; #(parameter int W = 1) (\n"
                            "  input logic clk_i,\n"
                            "  input logic rst_ni,\n"
                            "\n"
                            "  // Global enable\n"
                            "  input logic enable_i,\n"
                            "\n"
                            "  // Status\n"
                            "  output logic done_o\n"
                            ");\n"
                            "endmodule\n";

    std::string once;
    REQUIRE_NOTHROW(once = format_source(src, opts));
    CHECK(once.find("#(parameter int W = 1)(\n") != std::string::npos);
    CHECK(once.find("\n    // Global enable\n") != std::string::npos);
    CHECK(once.find("clk_i,") != std::string::npos);
    CHECK(once.find("done_o") != std::string::npos);
    CHECK(format_source(once, opts) == once);
}

TEST_CASE("formatter: ANSI module header after package import is aligned", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = false;

    CHECK(format_source("module m import p::*;\n"
                        "(input logic clk_i, input logic rst_ni,\n"
                        "\n"
                        "// Input handshake signals\n"
                        "input logic in_valid_i, output logic in_ready_o);\n"
                        "endmodule\n",
                        opts) == "module m import p::*;\n"
                                 "(\n"
                                 "    input     logic               clk_i,\n"
                                 "    input     logic               rst_ni,\n"
                                 "    // Input handshake signals\n"
                                 "    input     logic               in_valid_i,\n"
                                 "    output    logic               in_ready_o\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: module header closing line comment is preserved", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.port_declaration.align = true;

    CHECK(format_source("module m (\n"
                        "  output logic y\n"
                        "); // instance\n"
                        "endmodule\n",
                        opts) == "module m(\n"
                                 "    output    logic               y\n"
                                 "); // instance\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: final ANSI port with line comment does not gain comma", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;
    opts.port_declaration.align = true;

    CHECK(format_source("interface i (\n"
                        "  input logic a, // a\n"
                        "  input logic b  // b\n"
                        ");\n"
                        "endinterface\n",
                        opts) == "interface i(\n"
                                 "    input     logic               a, // a\n"
                                 "    input     logic               b // b\n"
                                 ");\n"
                                 "endinterface\n");
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
                        "endmodule\n",
                        opts) == "module top;\n"
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
    CHECK(format_source("logic [7:0] dout = 8'hFF;\n"
                        "logic [8:0] douteeeeeeeeeeeeeeeeeee = 8'hFF;\n",
                        opts) == "logic [7:0] dout = 8'hFF;\n"
                                 "logic [8:0] douteeeeeeeeeeeeeeeeeee = 8'hFF;\n");
}

TEST_CASE("formatter: user-defined type var decl not aligned by statement pass", "[formatter]") {
    FormatOptions opts;
    opts.statement.align = true;
    opts.var_declaration.align = false;

    // User-defined type var decls should NOT be aligned by align_assign_pass
    CHECK(format_source("packet_t test_init = 8'hFF;\n"
                        "packet_t test_init2 = 8'hFF;\n",
                        opts) == "packet_t test_init = 8'hFF;\n"
                                 "packet_t test_init2 = 8'hFF;\n");
}

TEST_CASE("formatter: parameter declarations are aligned by statement pass", "[formatter]") {
    FormatOptions opts;
    opts.statement.align = true;
    opts.indent_size = 4;
    opts.tab_align = true;

    CHECK(format_source("parameter int DATA_WIDTH = 32;\n", opts) ==
          "parameter int DATA_WIDTH    = 32;\n");
}

TEST_CASE("formatter: define macro body not reformatted", "[formatter]") {
    FormatOptions opts;
    opts.function.break_policy = "always";
    const std::string src = "`define print_bytes(ARR, STARTBYTE, NUMBYTES) \\\n"
                            "    for (int ii=STARTBYTE; ii<STARTBYTE+NUMBYTES; ii++) begin \\\n"
                            "        $display(\"0x%x \", ARR[ii]); \\\n"
                            "    end\n";
    // Backslash alignment pass aligns '\' to common column; body content unchanged.
    const std::string expected =
        "`define print_bytes(ARR, STARTBYTE, NUMBYTES)                 \\\n"
        "    for (int ii=STARTBYTE; ii<STARTBYTE+NUMBYTES; ii++) begin \\\n"
        "        $display(\"0x%x \", ARR[ii]);                           \\\n"
        "    end\n";
    CHECK(format_source(src, opts) == expected);
}

TEST_CASE("formatter: instance formatting skips define macro body", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;

    const std::string src = "`define CONNECT \\\n"
                            "  alert_esc_if alert_if[N](.clk(clk), .rst_n(rst_n)); \\\n"
                            "  logic done;\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    CHECK(formatted.find("alert_if[N](.clk(clk), .rst_n(rst_n));") != std::string::npos);

    auto strip_ws = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (!std::isspace((unsigned char)c))
                r += c;
        }
        return r;
    };
    CHECK(strip_ws(formatted) == strip_ws(src));
}

TEST_CASE("formatter: macro calls with empty arguments are preserved", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;

    const std::string src = "module top;\n"
                            "  `DV_CHECK_FATAL(expr, , msg_id)\n"
                            "endmodule\n";

    REQUIRE_NOTHROW(format_source(src, opts));
}


TEST_CASE("formatter: multiline macro calls with comments are preserved and idempotent",
          "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;

    const std::string src = "module top;\n"
                            "initial begin\n"
                            "`DV_CHECK_RANDOMIZE_WITH_FATAL(req,\n"
                            "                               a == 1;\n"
                            "                               b == 0; // inline comment\n"
                            "                               )\n"
                            "`uvm_info(`gfn, $sformatf(\"msg\",\n"
                            "                          a, b), UVM_MEDIUM)\n"
                            "finish_item(req);\n"
                            "end\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    CHECK(formatted.find("// inline comment\n") != std::string::npos);
    CHECK(formatted.find("// inline comment )`uvm_info") == std::string::npos);
    CHECK(formatted.find("// inline comment ) finish_item") == std::string::npos);
    CHECK(formatted.find("finish_item(req);") != std::string::npos);
    CHECK(format_source(formatted, opts) == formatted);
}

TEST_CASE("formatter: constraint brace blocks are idempotent", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.indent_size = 4;

    const std::string src =
        "class c;\n"
        "  rand bit parity_err;\n"
        "  rand bit parity;\n"
        "\n"
        "  constraint parity_c {\n"
        "    if (parity_err) {\n"
        "      parity != `GET_PARITY(data, p_sequencer.cfg.odd_parity);\n"
        "    } else {\n"
        "      parity == `GET_PARITY(data, p_sequencer.cfg.odd_parity);\n"
        "    }\n"
        "  }\n"
        "  `uvm_object_new\n"
        "endclass\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    CHECK(format_source(formatted, opts) == formatted);
    CHECK(formatted.find("}\n    `uvm_object_new") != std::string::npos);
}

TEST_CASE("formatter: begin_newline applies to begin and constraint braces", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.indent_size = 4;
    opts.statement.begin_newline = false;
    opts.statement.wrap_end_else_clauses = false;

    const std::string src =
        "class c;\n"
        "  constraint cstr {\n"
        "    if (a) {\n"
        "      x == 1;\n"
        "    } else {\n"
        "      x == 0;\n"
        "    }\n"
        "  }\n"
        "  task t();\n"
        "    if (a) begin\n"
        "      x = 1;\n"
        "    end\n"
        "  endtask\n"
        "endclass\n";

    std::string formatted = format_source(src, opts);
    CHECK(formatted.find("if (a) begin") != std::string::npos);
    CHECK(formatted.find("if (a) {") != std::string::npos);
    CHECK(formatted.find("} else {") != std::string::npos);

    opts.statement.begin_newline = true;
    opts.statement.wrap_end_else_clauses = true;
    formatted = format_source(src, opts);
    CHECK(formatted.find("if (a)\n        begin") != std::string::npos);
    CHECK(formatted.find("if (a)\n        {") != std::string::npos);
    CHECK(formatted.find("}\n        else\n        {") != std::string::npos);
}

TEST_CASE("formatter: adjacent operators keep source token boundaries", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;

    const std::string formatted = format_source("module top;\n"
                                                "always_comb begin\n"
                                                "y0 = ~ &a;\n"
                                                "y1 = ~ |a;\n"
                                                "y2 = ~ ^a;\n"
                                                "y3 = a ^ ~b;\n"
                                                "y4 = a & ~b;\n"
                                                "y5 = a | ~b;\n"
                                                "end\n"
                                                "endmodule\n",
                                                opts);

    CHECK(formatted.find("~ &") != std::string::npos);
    CHECK(formatted.find("~ |") != std::string::npos);
    CHECK(formatted.find("~ ^") != std::string::npos);
    CHECK(formatted.find("~&") == std::string::npos);
    CHECK(formatted.find("~|") == std::string::npos);
    CHECK(formatted.find("~^") == std::string::npos);
    CHECK(formatted.find("a ^ ~b") != std::string::npos);
    CHECK(formatted.find("a & ~b") != std::string::npos);
    CHECK(formatted.find("a | ~b") != std::string::npos);
    CHECK(format_source(formatted, opts) == formatted);
}

TEST_CASE("formatter: import declarations do not indent on function or task", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.indent_size = 4;

    CHECK(format_source("import \"DPI-C\" function chandle usbdpi_create(input string name, input int loglevel);\n"
                        "import \"DPI-C\" context task usbdpi_wait(input chandle ctx);\n"
                        "import \"DPI-C\" function void usbdpi_close(input chandle ctx);\n",
                        opts) ==
          "import \"DPI-C\" function chandle usbdpi_create(input string name, input int loglevel);\n"
          "import \"DPI-C\" context task usbdpi_wait(input chandle ctx);\n"
          "import \"DPI-C\" function void usbdpi_close(input chandle ctx);\n");
}

TEST_CASE("formatter: extern class method declarations do not indent following declarations",
          "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;

    const std::string formatted = format_source(
        "class esc_monitor;\n"
        "`uvm_component_utils(esc_monitor)\n"
        "extern function new (string name, uvm_component parent);\n"
        "extern virtual task run_phase(uvm_phase phase);\n"
        "extern virtual function bit get_esc();\n"
        "endclass: esc_monitor\n",
        opts);

    CHECK(formatted == "class esc_monitor;\n"
                       "  `uvm_component_utils(esc_monitor)\n"
                       "  extern function new (string name, uvm_component parent);\n"
                       "  extern virtual task run_phase(uvm_phase phase);\n"
                       "  extern virtual function bit get_esc();\n"
                       "endclass: esc_monitor\n");
    CHECK(format_source(formatted, opts) == formatted);
}

TEST_CASE("formatter: inside expression keeps required word-operator spaces", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.spacing.binary_operator_spacing = "none";

    const std::string formatted =
        format_source("if (!(req.esc_handshake_sta inside {A, B})) begin\nend\n", opts);

    CHECK(formatted.find("req.esc_handshake_sta inside {A, B}") != std::string::npos);
    CHECK(formatted.find("req.esc_handshake_stainside") == std::string::npos);
    CHECK(format_source(formatted, opts) == formatted);
}

TEST_CASE("formatter: typedef class forward declarations do not indent following declarations",
          "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("package p;\n"
                        "typedef class item;\n"
                        "typedef class cfg;\n"
                        "typedef enum {A, B} kind_e;\n"
                        "endpackage\n",
                        opts) == "package p;\n"
                                 "typedef class item;\n"
                                 "typedef class cfg;\n"
                                 "typedef enum {\n"
                                 "  A,\n"
                                 "  B\n"
                                 "} kind_e;\n"
                                 "endpackage\n");
}

TEST_CASE("formatter: labeled endtask stays on one line", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("class c;\n"
                        "task body();\n"
                        "endtask: body\n"
                        "endclass\n",
                        opts) == "class c;\n"
                                 "  task body();\n"
                                 "  endtask: body\n"
                                 "endclass\n");
}

TEST_CASE("formatter: labeled endtask keeps following task on next line", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("class c;\n"
                        "task a();\n"
                        "endtask: a\n"
                        "task b();\n"
                        "endtask: b\n"
                        "endclass\n",
                        opts) == "class c;\n"
                                 "  task a();\n"
                                 "  endtask: a\n"
                                 "  task b();\n"
                                 "  endtask: b\n"
                                 "endclass\n");
}

TEST_CASE("formatter: disable statement target stays on one line", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("class c;\n"
                        "task body();\n"
                        "disable my_block;\n"
                        "disable fork;\n"
                        "endtask\n"
                        "endclass\n",
                        opts) == "class c;\n"
                                 "  task body();\n"
                                 "    disable my_block;\n"
                                 "    disable fork;\n"
                                 "  endtask\n"
                                 "endclass\n");
}

TEST_CASE("formatter: macro call indent follows surrounding block", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;

    const std::string src = "module top;\n"
                            "foreach (mems[i]) begin\n"
                            "dv_base_mem dv_mem;\n"
                            "`downcast(dv_mem, mems[i], , , msg_id)\n"
                            "end\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    std::vector<std::string> lines;
    std::istringstream ss(formatted);
    for (std::string line; std::getline(ss, line);)
        lines.push_back(line);
    REQUIRE(lines.size() >= 4);
    CHECK(lines[2].find_first_not_of(' ') == lines[3].find_first_not_of(' '));
}

TEST_CASE("formatter: comments with parentheses are not treated as calls", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.function.break_policy = "always";

    const std::string src = "module top;\n"
                            "// If the memory supports write (WO or RW) then keep this comment.\n"
                            "endmodule\n";

    REQUIRE_NOTHROW(format_source(src, opts));
}

TEST_CASE("formatter: verilog_format on marker does not gain blank line", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.function.break_policy = "always";

    const std::string src = "module top;\n"
                            "// verilog_format: off\n"
                            "sum(.i_a(i_a2),\n"
                            "    .i_b(i_b));\n"
                            "\n"
                            "\n"
                            "\n"
                            "// verilog_format: on\n"
                            "sum(.i_a(1),\n"
                            "    .i_b(2));\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    CHECK(formatted.find("// verilog_format: off\n"
                         "sum(.i_a(i_a2),\n"
                         "    .i_b(i_b));\n"
                         "\n"
                         "\n"
                         "\n"
                         "// verilog_format: on\n") != std::string::npos);
    CHECK(format_source(formatted, opts) == formatted);
}

TEST_CASE("formatter: calls with empty positional arguments are preserved", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.function.break_policy = "always";

    const std::string src = "module top;\n"
                            "  issue_data(req.data, rsp.data, , num_bits);\n"
                            "endmodule\n";

    REQUIRE_NOTHROW(format_source(src, opts));
}

TEST_CASE("formatter: port header comments do not gain semicolons", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.port_declaration.align = true;

    const std::string src = "module top import pkg::*;\n"
                            "(\n"
                            "  input logic clk_i,\n"
                            "  output logic done_o // done\n"
                            ");\n"
                            "endmodule\n";

    REQUIRE_NOTHROW(format_source(src, opts));
}

TEST_CASE("formatter: inline line comments stay on their original line", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "assign a = b; // keep this inline\n"
                        "endmodule\n",
                        opts) == "module top;\n"
                                 "assign a = b; // keep this inline\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: inline block comments stay on their original line", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("module top;\n"
                        "assign a = b; /* keep this inline */\n"
                        "endmodule\n",
                        opts) == "module top;\n"
                                 "assign a = b; /* keep this inline */\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: block function call after inline block comment indents from call",
          "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.function.break_policy = "always";
    opts.function.layout = "block";

    CHECK(format_source("module top;\n"
                        "always_comb begin\n"
                        "/**/ add_number(.a(a3), .b(b), .result(result));\n"
                        "end\n"
                        "endmodule\n",
                        opts) == "module top;\n"
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
                        "endmodule\n",
                        opts) ==
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
                        "endmodule\n",
                        opts) == "module top;\n"
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
                        "endmodule\n",
                        opts) ==
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

    std::string result = format_source("module top;\n"
                                       "logic [7:0] dout = 8'hFF;\n"
                                       "logic [8:0] douteeeeeee = 8'hFF;\n"
                                       "endmodule\n",
                                       opts);
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
    opts.module.non_ansi_port_per_line_enabled = true;
    opts.module.non_ansi_port_per_line = 3;
    opts.default_indent_level_inside_outmost_block = 0;

    // Mimics memory_top.sv: non-ANSI module with port decls followed by var decls
    std::string input = "module top(a, b);\n"
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
    opts.safe_mode = true;

    // `WIDTH is a macro — formatter must not expand it (would change non-whitespace content)
    std::string input = "`define WIDTH 32\n"
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

    std::string formatted =
        format_source("module top(test, VSS);\n"
                      "output logic unsigned [0:0] VDD, VSS;\n"
                      "output                                        packet_t            [0:0] "
                      "test          , VSS                                     ;\n"
                      "endmodule\n",
                      opts);

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

    std::string formatted = format_source("typedef struct {\n"
                                          "logic [7:0] addr;\n"
                                          "logic valid;\n"
                                          "} packet_t;\n",
                                          opts);

    INFO("formatted output:\n" << formatted);
    CHECK(formatted.find("logic [7:0] addr") == std::string::npos);
    CHECK(formatted.find("logic               [7:0]") != std::string::npos);
    CHECK(formatted.find("logic                                   valid") != std::string::npos);
}

TEST_CASE("formatter: outmost indent option applies to module interface and package",
          "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("interface bus;\nlogic valid;\nendinterface\n", opts) == "interface bus;\n"
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
    opts.module.non_ansi_port_per_line_enabled = true;
    opts.module.non_ansi_port_per_line = 1;
    opts.default_indent_level_inside_outmost_block = 0;

    std::string formatted = format_source("module top(a, b, c);\n"
                                          "logic [7:0] testxrp;\n"
                                          "logic d;\n"
                                          "endmodule\n",
                                          opts);

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
                        "endmodule\n",
                        opts) == "module top;\n"
                                 "memory u_mem (\n"
                                 "    .address  (addr      ),\n"
                                 "    .data_in  (data      ),\n"
                                 "    .chip_en  (en        )\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: instance array dimensions are preserved", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    CHECK(format_source("module top;\n"
                        "push_pull_if #(.DeviceDataWidth(WIDTH)) adc_if[CHANNELS](.clk(clk), "
                        ".rst_n(rst_n));\n"
                        "endmodule\n",
                                 opts) == "module top;\n"
                                 "push_pull_if #(.DeviceDataWidth(WIDTH)) adc_if[CHANNELS] (\n"
                                 "    .clk      (clk       ),\n"
                                 "    .rst_n    (rst_n     )\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: autoinst comment does not block instance port expansion", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    CHECK(format_source("module top;\n"
                        "memory #(.MEM_SIZE(3)) u_memory( /*autoinst*/\n"
                        ".address(addr), .data_in(intercontest), .chip_en(tt));\n"
                        "endmodule\n",
                        opts) == "module top;\n"
                                 "memory #(.MEM_SIZE(3)) u_memory ( /*autoinst*/\n"
                                 "    .address  (addr        ),\n"
                                 "    .data_in  (intercontest),\n"
                                 "    .chip_en  (tt          )\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: instance parameter line comments are preserved", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.default_indent_level_inside_outmost_block = 0;

    const std::string src = "module top;\n"
                            "aes_ghash #(\n"
                            "  .SecMasking(1),\n"
                            "  .GFMultCycles(4) // comment A\n"
                            "                   // comment B\n"
                            ") u_aes_ghash (\n"
                            "  .clk_i(clk_i)\n"
                            ");\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));
    CHECK(formatted.find("comment A") != std::string::npos);
    CHECK(formatted.find("comment B") != std::string::npos);

    auto strip_ws = [](const std::string& text) {
        std::string out;
        for (char c : text) {
            if (!std::isspace((unsigned char)c))
                out += c;
        }
        return out;
    };
    CHECK(strip_ws(formatted) == strip_ws(src));
}

TEST_CASE("formatter: line comments do not block instance port expansion", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    CHECK(format_source("module top;\n"
                        "memory u_mem( // test\n"
                        ".i_clk(testxrp), .address(addressss), .data_in(threeshit));\n"
                        "memory u_mem1(.address(), .data_in(zzzry), // test\n"
                        ".dataut(), .read_te());\n"
                                 "endmodule\n",
                        opts) == "module top;\n"
                                 "memory u_mem ( // test\n"
                                 "    .i_clk    (testxrp   ),\n"
                                 "    .address  (addressss ),\n"
                                 "    .data_in  (threeshit )\n"
                                 ");\n"
                                 "memory u_mem1 (\n"
                                 "    .address  (          ),\n"
                                 "    .data_in  (zzzry     ), // test\n"
                                 "    .dataut   (          ),\n"
                                 "    .read_te  (          )\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: standalone line comments inside instance ports are preserved",
          "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source("module top;\n"
                                              "memory u_mem(\n"
                                              ".address(addr),\n"
                                              "// standalone comment\n"
                                              ".data_in(data),\n"
                                              ".chip_en(en));\n"
                                              "endmodule\n",
                                              opts));
    CHECK(formatted.find("// standalone comment") != std::string::npos);
}

TEST_CASE("formatter: block comments do not block instance port expansion", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    CHECK(format_source("module top;\n"
                        "memory u_mem(.address(addr), .data_in(zzzry), /* test */ .dataut());\n"
                                 "endmodule\n",
                        opts) == "module top;\n"
                                 "memory u_mem (\n"
                                 "    .address  (addr      ),\n"
                                 "    .data_in  (zzzry     ), /* test */\n"
                                 "    .dataut   (          )\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: block comment between instance port and paren is safe", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.default_indent_level_inside_outmost_block = 0;

    const std::string src = "module top;\n"
                            "i2c dut (\n"
                            "    .clk_i(clk),\n"
                            "    .racl_error_o /*har*/ ()\n"
                            ");\n"
                            "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(src, opts));

    auto strip_ws = [](const std::string& text) {
        std::string out;
        for (char c : text) {
            if (!std::isspace((unsigned char)c))
                out += c;
        }
        return out;
    };
    CHECK(strip_ws(formatted) == strip_ws(src));
}

TEST_CASE("formatter: expands consecutive single-line instances after multiline instance",
          "[formatter]") {
    FormatOptions opts;
    opts.instance.align = true;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    std::string formatted =
        format_source("module top;\n"
                      "memory #(.MEM_SIZE(3)) u_memory(\n"
                      "                           .address(addr),\n"
                      "                           .data_in(intercontest)\n"
                      "                       );\n"
                      "memory u_mem(.i_clk(testxrp), .address(addressss), .data_in(threeshit));\n"
                      "memory u_mem1(.address(), .data_in(zzzry), .dataut());\n"
                      "endmodule\n",
                      opts);

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
    opts.module.non_ansi_port_per_line_enabled = true;
    opts.module.non_ansi_port_per_line = 3;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.indent_size = 4;

    std::string formatted =
        format_source("module memory_top #(parameter int WIDTH = 4, parameter int DEPTH = 8) (\n"
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
                      "memory u_mem(.i_clk(testxrp), .address(addressss), .data_in(threeshit), "
                      ".chip_en(chip_en), .www333(www333), .zzfuk(zzfuk));\n"
                      "endmodule\n",
                      opts);

    INFO("formatted output:\n" << formatted);
    CHECK(formatted.find("memory u_mem(.i_clk") == std::string::npos);
    CHECK(formatted.find("memory u_mem (\n") != std::string::npos);
}

TEST_CASE("formatter: typedef enum bodies are expanded", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;

    CHECK(format_source("package p;\n"
                        "typedef enum logic[1:0]{IDLE,BUSY=2'd1,DONE} state_t;\n"
                        "endpackage\n",
                        opts) == "package p;\n"
                                 "typedef enum logic [1:0] {\n"
                                 "    IDLE,\n"
                                 "    BUSY = 2'd1,\n"
                                 "    DONE\n"
                                 "} state_t;\n"
                                 "endpackage\n");
}

TEST_CASE("formatter: interface modports are expanded and aligned", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.modport.align = true;
    opts.modport.direction_min_width = 7;

    CHECK(format_source("interface bus_if;\n"
                        "modport master(input clk,output data), slave(output clk,input data);\n"
                        "endinterface\n",
                        opts) == "interface bus_if;\n"
                                 "modport master (\n"
                                 "    input  clk ,\n"
                                 "    output data\n"
                                 "),\n"
                                 "modport slave (\n"
                                 "    output clk ,\n"
                                 "    input  data\n"
                                 ");\n"
                                 "endinterface\n");
}

TEST_CASE("formatter: enum declaration alignment supports strict and adaptive modes",
          "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.enum_declaration.align = true;
    opts.enum_declaration.enum_name_min_width = 8;
    opts.enum_declaration.enum_value_min_width = 6;

    CHECK(format_source("typedef enum {A=1,LONG_NAME=22,C=333333} e_t;\n", opts) ==
          "typedef enum {\n"
          "    A         = 1     ,\n"
          "    LONG_NAME = 22    ,\n"
          "    C         = 333333\n"
          "} e_t;\n");

    opts.enum_declaration.align_adaptive = true;
    CHECK(format_source("typedef enum {A=1,LONG_NAME=22,C=333333} e_t;\n", opts) ==
          "typedef enum {\n"
          "    A       = 1     ,\n"
          "    LONG_NAME = 22    ,\n"
          "    C       = 333333\n"
          "} e_t;\n");
}

TEST_CASE("formatter: modport alignment supports strict and adaptive modes", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.modport.align = true;
    opts.modport.direction_min_width = 7;
    opts.modport.signal_min_width = 6;

    CHECK(format_source("interface i;\n"
                        "modport m(input a,output long_signal,inout c);\n"
                        "endinterface\n",
                        opts) == "interface i;\n"
                                 "modport m (\n"
                                 "    input  a          ,\n"
                                 "    output long_signal,\n"
                                 "    inout  c\n"
                                 ");\n"
                                 "endinterface\n");

    opts.modport.align_adaptive = true;
    CHECK(format_source("interface i;\n"
                        "modport m(input a,output long_signal,inout c);\n"
                        "endinterface\n",
                        opts) == "interface i;\n"
                                 "modport m (\n"
                                 "    input  a     ,\n"
                                 "    output long_signal,\n"
                                 "    inout  c\n"
                                 ");\n"
                                 "endinterface\n");
}

TEST_CASE("formatter: adaptive modport alignment keeps direction column aligned", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.modport.align = true;
    opts.modport.align_adaptive = true;
    opts.modport.direction_min_width = 6;
    opts.modport.signal_min_width = 10;

    CHECK(format_source("interface i;\n"
                        "modport dut(input clk,input validdddddddd,input addr,input wdata,"
                        "input write,output ready,output rdata);\n"
                        "endinterface\n",
                        opts) == "interface i;\n"
                                 "modport dut (\n"
                                 "    input  clk       ,\n"
                                 "    input  validdddddddd,\n"
                                 "    input  addr      ,\n"
                                 "    input  wdata     ,\n"
                                 "    input  write     ,\n"
                                 "    output ready     ,\n"
                                 "    output rdata\n"
                                 ");\n"
                                 "endinterface\n");
}

TEST_CASE("formatter: enum strict alignment does not leave trailing spaces on final item",
          "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.enum_declaration.align = true;
    opts.enum_declaration.align_adaptive = false;
    opts.enum_declaration.enum_name_min_width = 0;
    opts.enum_declaration.enum_value_min_width = 0;

    CHECK(format_source("typedef enum logic [1:0] {IDLE,FETCH,EXECUTE,ERROR} state_t;\n", opts) ==
          "typedef enum logic [1:0] {\n"
          "    IDLE    ,\n"
          "    FETCH   ,\n"
          "    EXECUTE ,\n"
          "    ERROR\n"
          "} state_t;\n");
}

TEST_CASE("formatter: tab_align snaps statement assignment columns", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.statement.align = true;
    opts.tab_align = true;

    CHECK(format_source("module top;\n"
                        "always_comb begin\n"
                        "abcde = 1;\n"
                        "x = 2;\n"
                        "end\n"
                        "endmodule\n",
                        opts) == "module top;\n"
                                 "always_comb begin\n"
                                 "    abcde   = 1;\n"
                                 "    x       = 2;\n"
                                 "end\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: tab_align snaps declaration columns", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.tab_align = true;
    opts.port_declaration.align = true;
    opts.port_declaration.section1_min_width = 1;
    opts.port_declaration.section2_min_width = 1;
    opts.port_declaration.section3_min_width = 1;
    opts.port_declaration.section4_min_width = 1;
    opts.port_declaration.section5_min_width = 1;
    opts.var_declaration.align = true;
    opts.var_declaration.section1_min_width = 1;
    opts.var_declaration.section2_min_width = 1;
    opts.var_declaration.section3_min_width = 1;
    opts.var_declaration.section4_min_width = 1;

    CHECK(format_source("module top(a, bb);\n"
                        "input logic a;\n"
                        "output wire bb;\n"
                        "logic a;\n"
                        "wire bb;\n"
                        "endmodule\n",
                        opts) == "module top(\n"
                                 "    a,\n"
                                 "    bb\n"
                                 ");\n"
                                 "input   logic   a       ;\n"
                                 "output  wire    bb      ;\n"
                                 "logic   a       ;\n"
                                 "wire    bb      ;\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: tab_align snaps enum and modport columns", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.tab_align = true;
    opts.enum_declaration.align = true;
    opts.enum_declaration.enum_name_min_width = 0;
    opts.enum_declaration.enum_value_min_width = 0;
    opts.modport.align = true;
    opts.modport.direction_min_width = 0;
    opts.modport.signal_min_width = 0;

    CHECK(format_source("interface i;\n"
                        "typedef enum {A=1,LONG=22} e_t;\n"
                        "modport m(input clk,output ready);\n"
                        "endinterface\n",
                        opts) == "interface i;\n"
                                 "typedef enum {\n"
                                 "    A       = 1 ,\n"
                                 "    LONG    = 22\n"
                                 "} e_t;\n"
                                 "modport m (\n"
                                 "    input   clk     ,\n"
                                 "    output  ready\n"
                                 ");\n"
                                 "endinterface\n");
}

TEST_CASE("formatter: tab_align snaps fixed instance connection columns", "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.tab_align = true;
    opts.instance.align = true;
    opts.instance.align_adaptive = false;
    opts.instance.port_indent_level = 1;
    opts.instance.instance_port_name_width = 9;
    opts.instance.instance_port_between_paren_width = 3;

    CHECK(format_source("module top;\n"
                        "child u_child(.abcde(sig), .x(s));\n"
                        "endmodule\n",
                        opts) == "module top;\n"
                                 "child u_child (\n"
                                 "    .abcde      (sig),\n"
                                 "    .x          (s  )\n"
                                 ");\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: tab_align does not align equals inside headers or for controls",
          "[formatter]") {
    FormatOptions opts;
    opts.indent_size = 4;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.statement.align = true;
    opts.tab_align = true;

    CHECK(format_source("interface bus_intf #(parameter W_IDTH = 8) (input logic i_clk);\n"
                        "for(int i = 0; i < 32; i++) begin\n"
                        "foo_bar = 1;\n"
                        "x = 2;\n"
                        "end\n"
                        "endinterface\n",
                        opts) == "interface bus_intf #(\n"
                                 "    parameter W_IDTH = 8\n"
                                 ")(\n"
                                 "    input       logic               i_clk\n"
                                 ");\n"
                                 "for (int i = 0; i < 32; i++) begin\n"
                                 "    foo_bar = 1;\n"
                                 "    x = 2;\n"
                                 "end\n"
                                 "endinterface\n");
}

TEST_CASE("formatter: spacing options control control parens and brackets", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.spacing.control_keyword_space = false;
    opts.spacing.space_inside_parens = true;
    opts.spacing.space_inside_dimension_brackets = true;
    opts.spacing.binary_operator_spacing = "none";
    opts.spacing.dimension_binary_operator_spacing = "both";
    opts.spacing.range_colon_spacing = "both";
    opts.spacing.indexed_part_select_spacing = "none";

    CHECK(format_source("module top;\nif(a <= b) x = (c + d);\nwait(done);\nlogic [WIDTH-1:0] data;\nassign y = "
                        "arr[i+1] + arr[base +: width];\nendmodule\n",
                        opts) == "module top;\n"
                                 "if( a<=b )\n"
                                 "  x = ( c+d );\n"
                                 "wait( done );\n"
                                 "logic [ WIDTH - 1 : 0 ] data;\n"
                                 "assign y = arr[ i + 1 ]+arr[ base+:width ];\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: var alignment preserves dimension binary spacing for macro ranges",
          "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.var_declaration.align = true;
    opts.var_declaration.align_adaptive = true;
    opts.var_declaration.section1_min_width = 20;
    opts.var_declaration.section2_min_width = 20;
    opts.var_declaration.section3_min_width = 20;
    opts.var_declaration.section4_min_width = 16;
    opts.spacing.dimension_binary_operator_spacing = "none";

    std::string formatted = format_source("module top;\n"
                                          "logic [`WIDTH - 1123:0] addr;\n"
                                          "logic [`WIDTH - 1:0] data;\n"
                                          "endmodule\n",
                                          opts);

    CHECK(formatted.find("[`WIDTH-1123:0]") != std::string::npos);
    CHECK(formatted.find("[`WIDTH-1:0]") != std::string::npos);
    CHECK(formatted.find("`WIDTH - ") == std::string::npos);
}

TEST_CASE("formatter: wait has no space before paren", "[formatter]") {
    FormatOptions opts;
    opts.spacing.control_keyword_space = true;
    opts.spacing.space_inside_parens = true;

    CHECK(format_source("module top;\nwait(done);\nendmodule\n", opts) == "module top;\n"
                                                                            "  wait( done );\n"
                                                                            "endmodule\n");
}

TEST_CASE("formatter: wait fork does not open a fork block", "[formatter]") {
    FormatOptions opts;

    const std::string input = "class c;\n"
                              "  task a();\n"
                              "    fork begin : isolation_fork\n"
                              "      wait fork;\n"
                              "    end join\n"
                              "  endtask\n"
                              "\n"
                              "  task b();\n"
                              "    x();\n"
                              "  endtask\n"
                              "endclass\n";

    const std::string formatted = format_source(input, opts);
    CHECK(formatted == format_source(formatted, opts));
    CHECK(formatted.find("wait fork;\n") != std::string::npos);
    CHECK(formatted.find("wait fork\n") == std::string::npos);
    CHECK(formatted.find("\n  task b();\n") != std::string::npos);
}

TEST_CASE("formatter: expression macros in nested call arguments are idempotent", "[formatter]") {
    Config cfg = load_config(".");

    const std::string input =
        "class c;\n"
        "  task body();\n"
        "    repeat (20) begin\n"
        "      csr_rd_check(.ptr(a), .compare_value(0),\n"
        "                   .err_msg(called_from(`__FILE__, `__LINE__)));\n"
        "      ral.adc_en_ctl.adc_enable.set(1);\n"
        "      ral.adc_en_ctl.oneshot_mode.set(1);\n"
        "      csr_wr(ral.adc_en_ctl, ral.adc_en_ctl.get());\n"
        "    end\n"
        "  endtask\n"
        "endclass\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(input, cfg.format));
    CHECK(format_source(formatted, cfg.format) == formatted);
    CHECK(formatted.find("ral.adc_en_ctl.oneshot_mode.set(1);") != std::string::npos);
    CHECK(formatted.find("\nral.adc_en_ctl.oneshot_mode.set(1);") == std::string::npos);
}

TEST_CASE("formatter: event control and for semicolon spacing options", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.spacing.procedural_event_control_at_spacing = "both";
    opts.spacing.space_inside_event_control_parens = true;
    opts.spacing.semicolon_spacing = "both";

    CHECK(format_source("module top;\nalways@(posedge clk) q <= d;\n@(posedge clk);\nfor(i = 0;i < "
                        "N;i++) a = b;\nendmodule\n",
                        opts) == "module top;\n"
                                 "always @ ( posedge clk ) q <= d;\n"
                                 "@(posedge clk);\n"
                                 "for (i = 0 ; i < N ; i++)\n"
                                 "  a = b;\n"
                                 "endmodule\n");
}

TEST_CASE("formatter: spacing options survive demo-style alignment passes", "[formatter]") {
    FormatOptions opts;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.var_declaration.align = true;
    opts.port_declaration.align = true;
    opts.spacing.control_keyword_space = true;
    opts.spacing.space_inside_parens = true;
    opts.spacing.space_inside_dimension_brackets = true;
    opts.spacing.range_colon_spacing = "both";
    opts.spacing.indexed_part_select_spacing = "both";

    std::string formatted = format_source("module top;\n"
                                          "input logic [1:0] i_data [7:0];\n"
                                          "logic [7:0] data;\n"
                                          "logic [7+:1] part;\n"
                                          "always_comb begin\n"
                                          "if(a) begin\n"
                                          "end\n"
                                          "case(a)\n"
                                          "1: begin\n"
                                          "end\n"
                                          "endcase\n"
                                          "end\n"
                                          "endmodule\n",
                                          opts);
    INFO("formatted:\n" << formatted);
    CHECK(formatted.find("[ 1 : 0 ]") != std::string::npos);
    CHECK(formatted.find("[ 7 : 0 ]") != std::string::npos);
    CHECK(formatted.find("[ 7 +: 1 ]") != std::string::npos);
    CHECK(formatted.find("if ( a ) begin") != std::string::npos);
    CHECK(formatted.find("case ( a )") != std::string::npos);
}

TEST_CASE("formatter: duplicate instance port comments are preserved", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.instance.align = true;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;

    std::string input = "module top;\n"
                        "child u_child(\n"
                        ".a(sig_a), // first a comment\n"
                        ".a(sig_b), // second a comment\n"
                        ".b(sig_c) // b comment\n"
                        ");\n"
                        "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(input, opts));
    CHECK(formatted.find("// first a comment") != std::string::npos);
    CHECK(formatted.find("// second a comment") != std::string::npos);
    CHECK(formatted.find("// b comment") != std::string::npos);
}

TEST_CASE("formatter: conditional instance headers do not corrupt following instances",
          "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.instance.align = true;
    opts.instance.align_adaptive = true;
    opts.instance.instance_port_name_width = 10;
    opts.instance.instance_port_between_paren_width = 10;

    std::string input = "module tb;\n"
                        "`ifdef FOO\n"
                        "  chip #(\n"
                        "    .P(1)\n"
                        "  ) dut (\n"
                        "`else\n"
                        "  chip dut (\n"
                        "`endif\n"
                        "    .a(a)\n"
                        "  );\n"
                        "\n"
                        "`ifdef BAR\n"
                        "  prim_buf #(\n"
                        "    .Width(W0)\n"
                        "  ) u_req (\n"
                        "    .in_i(x),\n"
                        "    .out_o(y)\n"
                        "  );\n"
                        "  prim_buf #(\n"
                        "    .Width(W1)\n"
                        "  ) u_rsp (\n"
                        "    .in_i(p),\n"
                        "    .out_o(q)\n"
                        "  );\n"
                        "`endif\n"
                        "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(input, opts));

    auto strip_ws = [](const std::string& text) {
        std::string out;
        for (char c : text)
            if (!std::isspace((unsigned char)c))
                out += c;
        return out;
    };
    CHECK(strip_ws(formatted) == strip_ws(input));
    CHECK(formatted.find("u_req") != std::string::npos);
    CHECK(formatted.find(".in_i(x)") != std::string::npos);
    CHECK(formatted.find("u_rsp") != std::string::npos);
    CHECK(formatted.find(".in_i(p)") != std::string::npos);
    CHECK(formatted.find("u_req (\n    .in_i(p)") == std::string::npos);
}

TEST_CASE("formatter: ANSI port directives do not receive commas", "[formatter]") {
    FormatOptions opts;
    opts.safe_mode = true;
    opts.default_indent_level_inside_outmost_block = 0;
    opts.port_declaration.align = true;
    opts.port_declaration.align_adaptive = true;

    std::string input = "module top (\n"
                        "  input clk_i,\n"
                        "  `INOUT_AO(IOA2), // macro port\n"
                        "`ifdef USE_EXTRA\n"
                        "  input extra_i,\n"
                        "`endif // USE_EXTRA\n"
                        "  output done_o\n"
                        ");\n"
                        "endmodule\n";

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(input, opts));
    CHECK(formatted.find("`ifdef USE_EXTRA,") == std::string::npos);
    CHECK(formatted.find("`endif,") == std::string::npos);
    CHECK(formatted.find("`INOUT_AO(IOA2),") != std::string::npos);
    CHECK(formatted.find("extra_i,") != std::string::npos);
}

TEST_CASE("formatter: demo register else body splits to next line", "[formatter]") {
    Config cfg = load_config(".");
    std::ifstream file("demo/register.sv");
    REQUIRE(file.good());
    std::ostringstream ss;
    ss << file.rdbuf();

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(ss.str(), cfg.format));
    CHECK(formatted.find("\n    if (!rst_n)\n        q       <= '0;\n") != std::string::npos);
    CHECK(formatted.find("\n    else\n        q       <= d;\n") != std::string::npos);
    CHECK(format_source(formatted, cfg.format) == formatted);
}

TEST_CASE("formatter: demo memory_top formats with project config", "[formatter]") {
    Config cfg = load_config(".");
    std::ifstream file("demo/memory_top.sv");
    REQUIRE(file.good());
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string input = ss.str();

    std::string formatted;
    REQUIRE_NOTHROW(formatted = format_source(input, cfg.format));
    CHECK(format_source(formatted, cfg.format) == formatted);
}
