#include "config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
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

TEST_CASE("config: retired compilation knobs are ignored, not rejected", "[config]") {
    // background_compilation_threads and nice_value were removed once the
    // server started deriving both automatically.  Configuration files in the
    // wild still carry them, so they must load without a warning and without
    // disturbing the options that remain.
    auto dir = make_temp_toml(R"(
[compilation]
background_compilation = true
background_compilation_threads = 4
background_compilation_debounce_ms = 750
nice_value = 19
)");

    std::string warning;
    Config cfg = load_config(dir, &warning);

    CHECK(warning.empty());
    CHECK(cfg.compilation.background_compilation == true);
    CHECK(cfg.compilation.background_compilation_debounce_ms == 750);
}

TEST_CASE("config: missing file returns defaults", "[config]") {
    auto dir = fs::temp_directory_path() / "lv_no_such_dir_xyz123";
    fs::remove_all(dir);
    Config cfg = load_config(dir);
    CHECK(cfg.design.vcode.empty());
    CHECK(cfg.design.define.empty());
    CHECK(cfg.compilation.background_compilation == false);
    CHECK(cfg.compilation.background_compilation_debounce_ms == 1500);
    CHECK(cfg.compilation.log_timing == false);
    CHECK(cfg.inlay_hint.enable == true);
    CHECK(cfg.format.indent_size == 2);
    CHECK(cfg.lint.statement.case_missing_default == false);
    CHECK(cfg.lint.style.trailing_whitespace == false);
    CHECK(cfg.lint.naming.register_pattern.empty());
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

[compilation]
background_compilation = true
background_compilation_debounce_ms = 750
log_timing = true

[inlay_hint]
enable = false

[format]
indent_size = 4
blank_lines_between_items = 2
default_indent_level_inside_outmost_block = 0
tab_align = true
format_off_comment_pattern = "// lazyverilog: off"
format_on_comment_pattern = "// lazyverilog: on"

[format.statement]
align = true
align_adaptive = true
lhs_min_width = 8
begin_newline = true
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

[format.function_declaration]
layout = "hanging"
line_length = 70
space_before_paren = true

[format.macros]
object_like_expr = ["RTL_STATUS_WIDTH"]
function_like_expr = ["RTL_MASKED"]
statement_like = ["RTL_CHECK_EQ"]
declaration_like = ["RTL_ALERT_CONNECT"]
control_flow_like = ["RTL_IF_ENABLED"]
block_begin_like = ["RTL_PIPE_STAGE_BEGIN"]
block_end_like = ["RTL_PIPE_STAGE_END"]
whitespace_sensitive = ["RTL_LITERAL_PASTE"]

[format.spacing]
control_keyword_space = false
space_inside_parens = true
space_inside_dimension_brackets = true
binary_operator_spacing = "none"
dimension_binary_operator_spacing = "both"
semicolon_spacing = "both"
range_colon_spacing = "after"
indexed_part_select_spacing = "before"
procedural_event_control_at_spacing = "both"
space_inside_event_control_parens = true

[format.module]
parameter_layout = "hanging"
non_ansi_port_per_line_enabled = true
non_ansi_port_per_line = 4

[format.enum_declaration]
align = true
align_adaptive = true
enum_name_min_width = 8
enum_value_min_width = 12

[format.modport]
align = true
align_adaptive = true
direction_min_width = 8
signal_min_width = 12

[lint]
enable = false

[lint.statement]
enable = true
severity = "hint"
case_missing_default = true
latch_inference_detection = true
explicit_begin = true
no_raw_always = true
blocking_nonblocking_assignments = true

[lint.function]
enable = true
severity = "error"
functions_automatic = true
function_call_style = "named"
explicit_function_lifetime = true
explicit_task_lifetime = true

[lint.module]
enable = true
severity = "warning"
one_module_per_file = true

[lint.instance]
enable = true
severity = "warning"
module_instantiation_style = "named"
stale_instance_diagnostic = true

[lint.naming]
enable = true
severity = "hint"
interface_pattern = ".*_intf$"
struct_pattern = ".*_t$"
union_pattern = ".*_u$"
enum_pattern = ".*_e$"
parameter_pattern = "^P_"
localparam_pattern = "^LP_"
check_module_filename = true
check_package_filename = true
register_pattern = "^r_"

[lint.style]
trailing_whitespace = true

[rtltree]
show_instance_name = false
show_file = false

[autowire]
group_by_instance = true
sort_by_name = true

[autoarg]
autoarg_on_save = true
)");
    Config cfg = load_config(dir);

    CHECK(cfg.design.vcode == "my.f");
    REQUIRE(cfg.design.define.size() == 2);
    CHECK(cfg.design.define[0] == "RTL_SIM");
    CHECK(cfg.design.define[1] == "FAST_MODEL");

    CHECK(cfg.compilation.background_compilation == true);
    CHECK(cfg.compilation.background_compilation_debounce_ms == 750);
    CHECK(cfg.compilation.log_timing == true);

    CHECK(cfg.inlay_hint.enable == false);

    CHECK(cfg.format.indent_size == 4);
    CHECK(cfg.format.blank_lines_between_items == 2);
    CHECK(cfg.format.default_indent_level_inside_outmost_block == 0);
    CHECK(cfg.format.tab_align == true);
    CHECK(cfg.format.format_off_comment_pattern == "// lazyverilog: off");
    CHECK(cfg.format.format_on_comment_pattern == "// lazyverilog: on");

    CHECK(cfg.format.statement.align == true);
    CHECK(cfg.format.statement.align_adaptive == true);
    CHECK(cfg.format.statement.lhs_min_width == 8);
    CHECK(cfg.format.statement.begin_newline == true);
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

    CHECK(cfg.format.function_call.break_policy == "always");
    CHECK(cfg.format.function_call.line_length == 80);
    CHECK(cfg.format.function_call.arg_count == 3);
    CHECK(cfg.format.function_call.layout == "hanging");
    CHECK(cfg.format.function_call.space_before_paren == true);
    CHECK(cfg.format.function_call.space_inside_paren == true);
    CHECK(cfg.format.function_declaration.layout == "hanging");
    CHECK(cfg.format.function_declaration.line_length == 70);
    CHECK(cfg.format.function_declaration.space_before_paren == true);
    auto has_macro = [](const std::vector<std::string>& names, const char* name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    CHECK(has_macro(cfg.format.macros.object_like_expr, "RTL_STATUS_WIDTH"));
    CHECK(has_macro(cfg.format.macros.function_like_expr, "RTL_MASKED"));
    CHECK(has_macro(cfg.format.macros.statement_like, "RTL_CHECK_EQ"));
    CHECK(has_macro(cfg.format.macros.declaration_like, "RTL_ALERT_CONNECT"));
    CHECK(has_macro(cfg.format.macros.control_flow_like, "RTL_IF_ENABLED"));
    CHECK(has_macro(cfg.format.macros.block_begin_like, "RTL_PIPE_STAGE_BEGIN"));
    CHECK(has_macro(cfg.format.macros.block_end_like, "RTL_PIPE_STAGE_END"));
    CHECK(has_macro(cfg.format.macros.whitespace_sensitive, "RTL_LITERAL_PASTE"));

    CHECK(cfg.format.spacing.control_keyword_space == false);
    CHECK(cfg.format.spacing.space_inside_parens == true);
    CHECK(cfg.format.spacing.space_inside_dimension_brackets == true);
    CHECK(cfg.format.spacing.binary_operator_spacing == "none");
    CHECK(cfg.format.spacing.dimension_binary_operator_spacing == "both");
    CHECK(cfg.format.spacing.semicolon_spacing == "both");
    CHECK(cfg.format.spacing.range_colon_spacing == "after");
    CHECK(cfg.format.spacing.indexed_part_select_spacing == "before");
    CHECK(cfg.format.spacing.procedural_event_control_at_spacing == "both");
    CHECK(cfg.format.spacing.space_inside_event_control_parens == true);

    CHECK(cfg.format.module.non_ansi_port_per_line_enabled == true);
    CHECK(cfg.format.module.non_ansi_port_per_line == 4);
    CHECK(cfg.format.module.parameter_layout == "hanging");
    CHECK(cfg.format.enum_declaration.align == true);
    CHECK(cfg.format.enum_declaration.align_adaptive == true);
    CHECK(cfg.format.enum_declaration.enum_name_min_width == 8);
    CHECK(cfg.format.enum_declaration.enum_value_min_width == 12);
    CHECK(cfg.format.modport.align == true);
    CHECK(cfg.format.modport.align_adaptive == true);
    CHECK(cfg.format.modport.direction_min_width == 8);
    CHECK(cfg.format.modport.signal_min_width == 12);

    CHECK(cfg.lint.enable == false);
    CHECK(cfg.lint.statement.enable == true);
    CHECK(cfg.lint.statement.severity == "hint");
    CHECK(cfg.lint.statement.case_missing_default == true);
    CHECK(cfg.lint.function.enable == true);
    CHECK(cfg.lint.function.severity == "error");
    CHECK(cfg.lint.function.functions_automatic == true);
    CHECK(cfg.lint.function.explicit_function_lifetime == true);
    CHECK(cfg.lint.function.explicit_task_lifetime == true);
    CHECK(cfg.lint.function.function_call_style == "named");
    CHECK(cfg.lint.module.enable == true);
    CHECK(cfg.lint.module.one_module_per_file == true);
    CHECK(cfg.lint.instance.module_instantiation_style == "named");
    CHECK(cfg.lint.instance.stale_instance_diagnostic == true);
    CHECK(cfg.lint.statement.latch_inference_detection == true);
    CHECK(cfg.lint.statement.explicit_begin == true);
    CHECK(cfg.lint.statement.no_raw_always == true);
    CHECK(cfg.lint.statement.blocking_nonblocking_assignments == true);
    CHECK(cfg.lint.style.trailing_whitespace == true);
    CHECK(cfg.lint.naming.enable == true);
    CHECK(cfg.lint.naming.interface_pattern == ".*_intf$");
    CHECK(cfg.lint.naming.check_module_filename == true);
    CHECK(cfg.lint.naming.check_package_filename == true);
    CHECK(cfg.lint.naming.register_pattern == "^r_");

    CHECK(cfg.rtltree.show_instance_name == false);
    CHECK(cfg.rtltree.show_file == false);
    CHECK(cfg.autowire.group_by_instance == true);
    CHECK(cfg.autowire.sort_by_name == true);
    CHECK(cfg.autoarg.autoarg_on_save == true);
}

TEST_CASE("config: malformed TOML returns defaults", "[config]") {
    auto dir = make_temp_toml("[design]\nvcode = \"ok.f\"\ndefine = [\"A\",] @@@\n");
    Config cfg;
    std::string warning;
    ConfigWarning warning_detail;
    REQUIRE_NOTHROW(cfg = load_config(dir, &warning, &warning_detail));
    CHECK(cfg.compilation.background_compilation == false);
    CHECK(cfg.format.indent_size == 2);
    CHECK_FALSE(warning.empty());
    CHECK(warning.find("line 3, column") != std::string::npos);
    CHECK(warning_detail.path == dir / "lazyverilog.toml");
    CHECK(warning_detail.line == 3);
    CHECK(warning_detail.column > 0);
}

TEST_CASE("config: macro role conflicts are reported", "[config]") {
    auto dir = make_temp_toml(R"(
[format.macros]
statement_like = ["MY_MACRO"]
declaration_like = ["MY_MACRO"]
)");
    std::string warning;
    ConfigWarning warning_detail;
    Config cfg = load_config(dir, &warning, &warning_detail);
    (void)cfg;
    CHECK_FALSE(warning.empty());
    CHECK(warning.find("MY_MACRO") != std::string::npos);
    CHECK(warning.find("multiple role lists") != std::string::npos);
}

TEST_CASE("config: invalid value types and integer ranges are reported", "[config]") {
    auto dir = make_temp_toml(R"(
[design]
vcode = true
define = ["RTL_SIM", 123, false]

[compilation]
background_compilation = "yes"
background_compilation_debounce_ms = -1
log_timing = 1

[format]
indent_size = 0
blank_lines_between_items = -1
default_indent_level_inside_outmost_block = 2
enable_format_on_save = "true"
tab_align = 1
format_off_comment_pattern = false
format_on_comment_pattern = 7
log_path = 99

[format.statement]
align = "true"
lhs_min_width = -1

[format.function_call]
break_policy = false
line_length = 0
arg_count = -2
space_before_paren = 1

[format.macros]
statement_like = "MY_MACRO"
declaration_like = ["OK_DECL", 42]

[lint]
enable = "true"

[lint.naming]
severity = true
module_pattern = 1
check_module_filename = "yes"

[rtltree]
show_file = 1

[autowire]
group_by_instance = "yes"

[autoff]
register_pattern = false

[autofunc]
indent_size = 0
use_named_arguments = "yes"
)");

    std::string warning;
    ConfigWarning warning_detail;
    Config cfg = load_config(dir, &warning, &warning_detail);

    auto has_error = [&](const std::string& needle) {
        return std::any_of(warning_detail.validation_errors.begin(),
                           warning_detail.validation_errors.end(),
                           [&](const std::string& err) {
                               return err.find(needle) != std::string::npos;
                           });
    };

    CHECK_FALSE(warning.empty());
    CHECK(warning.find("lazyverilog.toml value error(s)") != std::string::npos);
    CHECK(warning_detail.path == dir / "lazyverilog.toml");
    CHECK(warning_detail.line == 0);
    CHECK(warning_detail.column == 0);
    CHECK(warning_detail.validation_errors.size() >= 25);

    CHECK(has_error("[design].vcode: expected string, got boolean"));
    CHECK(has_error("[design].define[1]: expected string, got integer"));
    CHECK(has_error("[design].define[2]: expected string, got boolean"));
    CHECK(has_error("[compilation].background_compilation: expected boolean, got string"));
    CHECK(has_error("[format].indent_size: integer 0 out of range [1, 64]"));
    CHECK(has_error("[format].default_indent_level_inside_outmost_block: integer 2 out of range [0, 1]"));
    CHECK(has_error("[format].format_off_comment_pattern: expected string, got boolean"));
    CHECK(has_error("[format.function_call].break_policy: expected string, got boolean"));
    CHECK(has_error("[format.function_call].arg_count: integer -2 out of range [-1, 10000]"));
    CHECK(has_error("[format.macros].statement_like: expected array of strings, got string"));
    CHECK(has_error("[format.macros].declaration_like[1]: expected string, got integer"));
    CHECK(has_error("[lint.naming].severity: expected string, got boolean"));
    CHECK(has_error("[rtltree].show_file: expected boolean, got integer"));
    CHECK(has_error("[autoff].register_pattern: expected string, got boolean"));
    CHECK(has_error("[autofunc].indent_size: integer 0 out of range [1, 64]"));

    // Invalid scalar/ranged options keep their safe defaults instead of
    // propagating bad values into formatter, linter, or background worker code.
    CHECK(cfg.design.vcode.empty());
    REQUIRE(cfg.design.define.size() == 1);
    CHECK(cfg.design.define[0] == "RTL_SIM");
    CHECK(cfg.compilation.background_compilation == false);
    CHECK(cfg.format.indent_size == 2);
    CHECK(cfg.format.default_indent_level_inside_outmost_block == 1);
    CHECK(cfg.format.function_call.arg_count == -1);
    CHECK(cfg.format.macros.declaration_like.back() == "OK_DECL");
    CHECK(cfg.lint.enable == true);
    CHECK(cfg.lint.naming.severity == "warning");
    CHECK(cfg.rtltree.show_file == true);
    CHECK(cfg.autoff.register_pattern.empty());
    CHECK(cfg.autofunc.indent_size == 4);
    CHECK(cfg.autofunc.use_named_arguments == true);
}
