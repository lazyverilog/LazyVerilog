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
