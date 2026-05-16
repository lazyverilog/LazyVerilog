#include <catch2/catch_test_macros.hpp>
#include "config.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static fs::path make_temp_toml(const std::string& content) {
    auto dir = fs::temp_directory_path() / "lv_test_config";
    fs::create_directories(dir);
    auto p = dir / "lazyverilog.toml";
    std::ofstream f(p);
    f << content;
    return dir;
}

TEST_CASE("config: missing file returns defaults", "[config]") {
    auto dir = fs::temp_directory_path() / "lv_no_such_dir_xyz123";
    fs::remove_all(dir);
    Config cfg = load_config(dir);
    CHECK(cfg.design.vcode.empty());
    CHECK(cfg.design.define.empty());
    CHECK(cfg.perf.background_compilation == false);
    CHECK(cfg.perf.nice_value == 10);
    CHECK(cfg.perf.log_timing == false);
    CHECK(cfg.inlay_hint.enable == true);
    CHECK(cfg.format.indent_size == 2);
    CHECK(cfg.format.trailing_newline == true);
    CHECK(cfg.lint.case_missing_default == false);
    CHECK(cfg.lint.register_naming == false);
    CHECK(cfg.autoinst.align_ports == false);
    CHECK(cfg.autoarg.autoarg_on_save == false);
}

TEST_CASE("config: unknown TOML keys silently ignored", "[config]") {
    auto dir = make_temp_toml(R"(
[unknown_section]
some_key = "value"
another_key = 42

[design]
vcode = "test.f"
unknown_design_key = true
)");
    Config cfg;
    REQUIRE_NOTHROW(cfg = load_config(dir));
    CHECK(cfg.design.vcode == "test.f");
}

TEST_CASE("config: parse all sections correctly", "[config]") {
    auto dir = make_temp_toml(R"(
[design]
vcode = "my.f"
define = ["RTL_SIM", "FAST_MODEL"]

[perf]
background_compilation = true
nice_value = 15
log_timing = true

[inlay_hint]
enable = false

[format]
indent_size = 4
trailing_newline = false
keyword_case = "lower"
blank_lines_between_items = 2
default_indent_level_inside_module_block = 0
compact_indexing_and_selections = false
safe_mode = true
tab_align = false
align_punctuation = true

[format.statement]
align = true
lhs_min_width = 8
wrap_end_else_clauses = true

[format.port_declaration]
align = true
align_adaptive = true
section1_min_width = 6
section2_min_width = 20

[format.var_declaration]
align = true
align_adaptive = true
section1_min_width = 10

[format.instance]
align = true
port_indent_level = 2
instance_port_name_width = 12
align_adaptive = true

[format.function_call]
break_policy = "always"
line_length = 80
arg_count = 3
layout = "hanging"
space_before_paren = true
space_inside_paren = true

[format.port]
non_ansi_port_per_line_enabled = true
non_ansi_port_per_line = 4

[lint]
case_missing_default = true
functions_automatic = true
explicit_function_lifetime = true
explicit_task_lifetime = true
module_instantiation_style = true
latch_inference_detection = true
explicit_begin = true
register_naming = true

[autoinst]
align_ports = true

[autoarg]
autoarg_on_save = true
)");
    Config cfg = load_config(dir);

    CHECK(cfg.design.vcode == "my.f");
    REQUIRE(cfg.design.define.size() == 2);
    CHECK(cfg.design.define[0] == "RTL_SIM");
    CHECK(cfg.design.define[1] == "FAST_MODEL");

    CHECK(cfg.perf.background_compilation == true);
    CHECK(cfg.perf.nice_value == 15);
    CHECK(cfg.perf.log_timing == true);

    CHECK(cfg.inlay_hint.enable == false);

    CHECK(cfg.format.indent_size == 4);
    CHECK(cfg.format.trailing_newline == false);
    CHECK(cfg.format.keyword_case == "lower");
    CHECK(cfg.format.blank_lines_between_items == 2);
    CHECK(cfg.format.default_indent_level_inside_module_block == 0);
    CHECK(cfg.format.compact_indexing_and_selections == false);
    CHECK(cfg.format.safe_mode == true);
    CHECK(cfg.format.tab_align == false);
    CHECK(cfg.format.align_punctuation == true);

    CHECK(cfg.format.statement.align == true);
    CHECK(cfg.format.statement.lhs_min_width == 8);
    CHECK(cfg.format.statement.wrap_end_else_clauses == true);

    CHECK(cfg.format.port_declaration.align == true);
    CHECK(cfg.format.port_declaration.align_adaptive == true);
    CHECK(cfg.format.port_declaration.section1_min_width == 6);
    CHECK(cfg.format.port_declaration.section2_min_width == 20);

    CHECK(cfg.format.var_declaration.align == true);
    CHECK(cfg.format.var_declaration.align_adaptive == true);
    CHECK(cfg.format.var_declaration.section1_min_width == 10);

    CHECK(cfg.format.instance.align == true);
    CHECK(cfg.format.instance.port_indent_level == 2);
    CHECK(cfg.format.instance.instance_port_name_width == 12);
    CHECK(cfg.format.instance.align_adaptive == true);

    CHECK(cfg.format.function.break_policy == "always");
    CHECK(cfg.format.function.line_length == 80);
    CHECK(cfg.format.function.arg_count == 3);
    CHECK(cfg.format.function.layout == "hanging");
    CHECK(cfg.format.function.space_before_paren == true);
    CHECK(cfg.format.function.space_inside_paren == true);

    CHECK(cfg.format.port.non_ansi_port_per_line_enabled == true);
    CHECK(cfg.format.port.non_ansi_port_per_line == 4);

    CHECK(cfg.lint.case_missing_default == true);
    CHECK(cfg.lint.functions_automatic == true);
    CHECK(cfg.lint.explicit_function_lifetime == true);
    CHECK(cfg.lint.explicit_task_lifetime == true);
    CHECK(cfg.lint.module_instantiation_style == true);
    CHECK(cfg.lint.latch_inference_detection == true);
    CHECK(cfg.lint.explicit_begin == true);
    CHECK(cfg.lint.register_naming == true);

    CHECK(cfg.autoinst.align_ports == true);
    CHECK(cfg.autoarg.autoarg_on_save == true);
}

TEST_CASE("config: malformed TOML returns defaults", "[config]") {
    auto dir = make_temp_toml("this is not valid toml @@@ !!!");
    Config cfg;
    REQUIRE_NOTHROW(cfg = load_config(dir));
    CHECK(cfg.perf.background_compilation == false);
    CHECK(cfg.format.indent_size == 2);
}
