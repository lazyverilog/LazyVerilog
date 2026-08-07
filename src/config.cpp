#include "config.hpp"
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <unordered_map>
#include <toml++/toml.hpp>

namespace {

std::string toml_type_name(const toml::node& node) {
    switch (node.type()) {
    case toml::node_type::table: return "table";
    case toml::node_type::array: return "array";
    case toml::node_type::string: return "string";
    case toml::node_type::integer: return "integer";
    case toml::node_type::floating_point: return "floating-point";
    case toml::node_type::boolean: return "boolean";
    case toml::node_type::date: return "date";
    case toml::node_type::time: return "time";
    case toml::node_type::date_time: return "date-time";
    case toml::node_type::none: break;
    }
    return "unknown";
}

void push_type_error(std::vector<std::string>& errors, const std::string& path,
                     const char* expected, const toml::node& got) {
    errors.push_back(path + ": expected " + expected + ", got " + toml_type_name(got));
}

void read_bool(const toml::table* table, const char* key, const std::string& path,
               bool& dst, std::vector<std::string>& errors) {
    if (!table)
        return;
    const toml::node_view node = (*table)[key];
    if (!node)
        return;
    if (const auto* value = node.as_boolean()) {
        dst = value->get();
        return;
    }
    push_type_error(errors, path, "boolean", *node.node());
}

void read_string(const toml::table* table, const char* key, const std::string& path,
                 std::string& dst, std::vector<std::string>& errors) {
    if (!table)
        return;
    const toml::node_view node = (*table)[key];
    if (!node)
        return;
    if (const auto* value = node.as_string()) {
        dst = value->get();
        return;
    }
    push_type_error(errors, path, "string", *node.node());
}

void read_int(const toml::table* table, const char* key, const std::string& path,
              int min_value, int max_value, int& dst, std::vector<std::string>& errors) {
    if (!table)
        return;
    const toml::node_view node = (*table)[key];
    if (!node)
        return;

    const auto* value = node.as_integer();
    if (!value) {
        push_type_error(errors, path, "integer", *node.node());
        return;
    }
    const int64_t raw_value = value->get();
    if (raw_value < min_value || raw_value > max_value) {
        errors.push_back(path + ": integer " + std::to_string(raw_value) +
                         " out of range [" + std::to_string(min_value) + ", " +
                         std::to_string(max_value) + "]");
        return;
    }
    dst = static_cast<int>(raw_value);
}

void append_string_array(const toml::table* table, const char* key, const std::string& path,
                         std::vector<std::string>& dst,
                         std::vector<std::string>& errors) {
    if (!table)
        return;
    const toml::node_view node = (*table)[key];
    if (!node)
        return;

    const toml::array* arr = node.as_array();
    if (!arr) {
        push_type_error(errors, path, "array of strings", *node.node());
        return;
    }

    size_t index = 0;
    arr->for_each([&](auto&& el) {
        using El = std::remove_cvref_t<decltype(el)>;
        if constexpr (toml::is_string<El>) {
            dst.push_back(*el);
        } else {
            errors.push_back(path + "[" + std::to_string(index) +
                             "]: expected string, got " + toml_type_name(el));
        }
        ++index;
    });
}

} // namespace

static std::string normalize_macro_config_name(std::string name) {
    if (!name.empty() && name[0] == '`')
        name.erase(name.begin());
    return name;
}

static std::vector<std::string> validate_config(const Config& cfg) {
    std::vector<std::string> errors;

    auto check_severity = [&](const std::string& val, const char* ctx) {
        if (!val.empty() && val != "warning" && val != "error" && val != "hint")
            errors.push_back(std::string(ctx) + ": invalid severity \"" + val +
                             "\" (must be \"warning\", \"error\", or \"hint\")");
    };
    check_severity(cfg.lint.function.severity, "[lint.function].severity");
    check_severity(cfg.lint.statement.severity, "[lint.statement].severity");
    check_severity(cfg.lint.module.severity, "[lint.module].severity");
    check_severity(cfg.lint.naming.severity, "[lint.naming].severity");

    auto check_enum = [&](const std::string& val, const char* ctx,
                          std::initializer_list<const char*> valid) {
        if (val.empty())
            return;
        for (auto v : valid)
            if (val == v)
                return;
        std::string msg = std::string(ctx) + ": invalid value \"" + val + "\" (must be one of:";
        for (auto v : valid)
            msg += std::string(" \"") + v + "\"";
        msg += ")";
        errors.push_back(msg);
    };
    check_enum(cfg.lint.instance.module_instantiation_style,
               "[lint.instance].module_instantiation_style", {"positional", "named", "both"});
    check_enum(cfg.lint.function.function_call_style, "[lint.function].function_call_style",
               {"positional", "named", "both"});
    check_enum(cfg.format.function_call.break_policy, "[format.function_call].break_policy",
               {"auto", "always", "never"});
    check_enum(cfg.format.function_call.layout, "[format.function_call].layout", {"block", "hanging"});
    check_enum(cfg.format.function_declaration.layout, "[format.function_declaration].layout",
               {"block", "hanging"});
    check_enum(cfg.format.module.parameter_layout, "[format.module].parameter_layout",
               {"block", "hanging"});
    check_enum(cfg.format.spacing.binary_operator_spacing,
               "[format.spacing].binary_operator_spacing", {"none", "before", "after", "both"});
    check_enum(cfg.format.spacing.dimension_binary_operator_spacing,
               "[format.spacing].dimension_binary_operator_spacing",
               {"none", "before", "after", "both"});
    check_enum(cfg.format.spacing.semicolon_spacing, "[format.spacing].semicolon_spacing",
               {"none", "before", "after", "both"});
    check_enum(cfg.format.spacing.range_colon_spacing, "[format.spacing].range_colon_spacing",
               {"none", "before", "after", "both"});
    check_enum(cfg.format.spacing.indexed_part_select_spacing,
               "[format.spacing].indexed_part_select_spacing", {"none", "before", "after", "both"});
    check_enum(cfg.format.spacing.procedural_event_control_at_spacing,
               "[format.spacing].procedural_event_control_at_spacing",
               {"none", "before", "after", "both"});
    check_enum(cfg.format.spacing.assignment_operator_spacing,
               "[format.spacing].assignment_operator_spacing",
               {"none", "before", "after", "both"});

    std::unordered_map<std::string, std::vector<std::string>> macro_roles;
    auto add_macro_role = [&](const std::vector<std::string>& names, const char* role) {
        for (const auto& name : names)
            macro_roles[normalize_macro_config_name(name)].push_back(role);
    };
    add_macro_role(cfg.format.macros.object_like_expr, "object_like_expr");
    add_macro_role(cfg.format.macros.function_like_expr, "function_like_expr");
    add_macro_role(cfg.format.macros.statement_like, "statement_like");
    add_macro_role(cfg.format.macros.declaration_like, "declaration_like");
    add_macro_role(cfg.format.macros.control_flow_like, "control_flow_like");
    add_macro_role(cfg.format.macros.block_begin_like, "block_begin_like");
    add_macro_role(cfg.format.macros.block_end_like, "block_end_like");
    for (const auto& [name, roles] : macro_roles) {
        if (roles.size() <= 1)
            continue;
        std::string msg = "[format.macros]: macro \"" + name +
                          "\" appears in multiple role lists:";
        for (const auto& role : roles)
            msg += " " + role;
        errors.push_back(msg);
    }

    return errors;
}

std::filesystem::path find_config_root(const std::filesystem::path& start) {
    auto dir = std::filesystem::is_directory(start) ? start : start.parent_path();
    while (true) {
        if (std::filesystem::exists(dir / "lazyverilog.toml"))
            return dir;
        auto parent = dir.parent_path();
        if (parent == dir)
            break; // filesystem root
        dir = parent;
    }
    return {};
}

Config load_config(const std::filesystem::path& root, std::string* warning,
                   ConfigWarning* warning_detail) {
    Config cfg{};
    if (warning)
        warning->clear();
    if (warning_detail)
        *warning_detail = {};
    auto toml_path = root / "lazyverilog.toml";
    if (!std::filesystem::exists(toml_path)) {
        return cfg;
    }
    std::vector<std::string> value_errors;
    try {
        auto tbl = toml::parse_file(toml_path.string());

        auto expect_table = [&](const toml::table* parent, const char* key,
                                const std::string& path) {
            if (!parent)
                return;
            const auto node = (*parent)[key];
            if (node && !node.as_table())
                push_type_error(value_errors, path, "table", *node.node());
        };

        for (const auto* section : {"design", "compilation", "inlay_hint", "format",
                                    "lint", "rtltree", "autoarg", "autowire", "autoff",
                                    "autofunc"}) {
            expect_table(&tbl, section, std::string("[") + section + "]");
        }
        if (auto f = tbl["format"].as_table()) {
            for (const auto* section : {"statement", "port_declaration", "var_declaration",
                                        "instance", "function_call", "function_declaration",
                                        "module", "enum_declaration", "modport", "spacing",
                                        "macros"}) {
                expect_table(f, section, std::string("[format.") + section + "]");
            }
        }
        if (auto lint = tbl["lint"].as_table()) {
            for (const auto* section : {"function", "statement", "style", "module",
                                        "instance", "naming"}) {
                expect_table(lint, section, std::string("[lint.") + section + "]");
            }
        }

        // [design]
        if (auto d = tbl["design"].as_table()) {
            read_string(d, "vcode", "[design].vcode", cfg.design.vcode, value_errors);
            append_string_array(d, "define", "[design].define", cfg.design.define,
                                value_errors);
        }

        // [compilation]
        if (auto p = tbl["compilation"].as_table()) {
            read_bool(p, "background_compilation",
                      "[compilation].background_compilation",
                      cfg.compilation.background_compilation, value_errors);
            read_int(p, "background_compilation_debounce_ms",
                     "[compilation].background_compilation_debounce_ms", 0, 600000,
                     cfg.compilation.background_compilation_debounce_ms, value_errors);
            read_bool(p, "log_timing", "[compilation].log_timing",
                      cfg.compilation.log_timing, value_errors);
        }

        // [inlay_hint]
        if (auto ih = tbl["inlay_hint"].as_table()) {
            read_bool(ih, "enable", "[inlay_hint].enable", cfg.inlay_hint.enable,
                      value_errors);
        }

        // [format]
        if (auto f = tbl["format"].as_table()) {
            read_int(f, "indent_size", "[format].indent_size", 1, 64,
                     cfg.format.indent_size, value_errors);
            read_int(f, "blank_lines_between_items", "[format].blank_lines_between_items",
                     0, 100, cfg.format.blank_lines_between_items, value_errors);
            read_int(f, "default_indent_level_inside_outmost_block",
                     "[format].default_indent_level_inside_outmost_block", 0, 1,
                     cfg.format.default_indent_level_inside_outmost_block, value_errors);
            read_bool(f, "enable_format_on_save", "[format].enable_format_on_save",
                      cfg.format.enable_format_on_save, value_errors);
            read_bool(f, "tab_align", "[format].tab_align", cfg.format.tab_align,
                      value_errors);
            read_string(f, "format_off_comment_pattern",
                        "[format].format_off_comment_pattern",
                        cfg.format.format_off_comment_pattern, value_errors);
            read_string(f, "format_on_comment_pattern",
                        "[format].format_on_comment_pattern",
                        cfg.format.format_on_comment_pattern, value_errors);
            read_string(f, "log_path", "[format].log_path", cfg.format.log_path,
                        value_errors);
            // Nested subtables
            if (auto st = (*f)["statement"].as_table()) {
                read_bool(st, "align", "[format.statement].align",
                          cfg.format.statement.align, value_errors);
                read_bool(st, "align_adaptive", "[format.statement].align_adaptive",
                          cfg.format.statement.align_adaptive, value_errors);
                read_int(st, "lhs_min_width", "[format.statement].lhs_min_width", 0, 10000,
                         cfg.format.statement.lhs_min_width, value_errors);
                read_bool(st, "begin_newline", "[format.statement].begin_newline",
                          cfg.format.statement.begin_newline, value_errors);
                read_bool(st, "wrap_end_else_clauses",
                          "[format.statement].wrap_end_else_clauses",
                          cfg.format.statement.wrap_end_else_clauses, value_errors);
            }
            if (auto pd = (*f)["port_declaration"].as_table()) {
                read_bool(pd, "align", "[format.port_declaration].align",
                          cfg.format.port_declaration.align, value_errors);
                read_bool(pd, "align_adaptive", "[format.port_declaration].align_adaptive",
                          cfg.format.port_declaration.align_adaptive, value_errors);
                read_int(pd, "section1_min_width",
                         "[format.port_declaration].section1_min_width", 0, 10000,
                         cfg.format.port_declaration.section1_min_width, value_errors);
                read_int(pd, "section2_min_width",
                         "[format.port_declaration].section2_min_width", 0, 10000,
                         cfg.format.port_declaration.section2_min_width, value_errors);
                read_int(pd, "section3_min_width",
                         "[format.port_declaration].section3_min_width", 0, 10000,
                         cfg.format.port_declaration.section3_min_width, value_errors);
                read_int(pd, "section4_min_width",
                         "[format.port_declaration].section4_min_width", 0, 10000,
                         cfg.format.port_declaration.section4_min_width, value_errors);
                read_int(pd, "section5_min_width",
                         "[format.port_declaration].section5_min_width", 0, 10000,
                         cfg.format.port_declaration.section5_min_width, value_errors);
            }
            if (auto vd = (*f)["var_declaration"].as_table()) {
                read_bool(vd, "align", "[format.var_declaration].align",
                          cfg.format.var_declaration.align, value_errors);
                read_bool(vd, "align_adaptive", "[format.var_declaration].align_adaptive",
                          cfg.format.var_declaration.align_adaptive, value_errors);
                read_int(vd, "section1_min_width",
                         "[format.var_declaration].section1_min_width", 0, 10000,
                         cfg.format.var_declaration.section1_min_width, value_errors);
                read_int(vd, "section2_min_width",
                         "[format.var_declaration].section2_min_width", 0, 10000,
                         cfg.format.var_declaration.section2_min_width, value_errors);
                read_int(vd, "section3_min_width",
                         "[format.var_declaration].section3_min_width", 0, 10000,
                         cfg.format.var_declaration.section3_min_width, value_errors);
                read_int(vd, "section4_min_width",
                         "[format.var_declaration].section4_min_width", 0, 10000,
                         cfg.format.var_declaration.section4_min_width, value_errors);
            }
            if (auto inst = (*f)["instance"].as_table()) {
                read_bool(inst, "align", "[format.instance].align",
                          cfg.format.instance.align, value_errors);
                read_int(inst, "port_indent_level", "[format.instance].port_indent_level",
                         0, 1000, cfg.format.instance.port_indent_level, value_errors);
                read_int(inst, "instance_port_name_width",
                         "[format.instance].instance_port_name_width", 0, 10000,
                         cfg.format.instance.instance_port_name_width, value_errors);
                read_int(inst, "instance_port_between_paren_width",
                         "[format.instance].instance_port_between_paren_width", 0, 10000,
                         cfg.format.instance.instance_port_between_paren_width, value_errors);
                read_bool(inst, "align_adaptive", "[format.instance].align_adaptive",
                          cfg.format.instance.align_adaptive, value_errors);
            }
            if (auto fn = (*f)["function_call"].as_table()) {
                read_string(fn, "break_policy", "[format.function_call].break_policy",
                            cfg.format.function_call.break_policy, value_errors);
                read_int(fn, "line_length", "[format.function_call].line_length", 1, 10000,
                         cfg.format.function_call.line_length, value_errors);
                read_int(fn, "arg_count", "[format.function_call].arg_count", -1, 10000,
                         cfg.format.function_call.arg_count, value_errors);
                read_string(fn, "layout", "[format.function_call].layout",
                            cfg.format.function_call.layout, value_errors);
                read_bool(fn, "space_before_paren", "[format.function_call].space_before_paren",
                          cfg.format.function_call.space_before_paren, value_errors);
                read_bool(fn, "space_inside_paren", "[format.function_call].space_inside_paren",
                          cfg.format.function_call.space_inside_paren, value_errors);
            }
            if (auto fd = (*f)["function_declaration"].as_table()) {
                read_string(fd, "layout", "[format.function_declaration].layout",
                            cfg.format.function_declaration.layout, value_errors);
                read_int(fd, "line_length", "[format.function_declaration].line_length", 1,
                         10000, cfg.format.function_declaration.line_length, value_errors);
                read_bool(fd, "space_before_paren",
                          "[format.function_declaration].space_before_paren",
                          cfg.format.function_declaration.space_before_paren, value_errors);
            }
            if (auto po = (*f)["module"].as_table()) {
                read_bool(po, "non_ansi_port_per_line_enabled",
                          "[format.module].non_ansi_port_per_line_enabled",
                          cfg.format.module.non_ansi_port_per_line_enabled, value_errors);
                read_int(po, "non_ansi_port_per_line",
                         "[format.module].non_ansi_port_per_line", 1, 1000,
                         cfg.format.module.non_ansi_port_per_line, value_errors);
                read_bool(po, "non_ansi_port_max_line_length_enabled",
                          "[format.module].non_ansi_port_max_line_length_enabled",
                          cfg.format.module.non_ansi_port_max_line_length_enabled, value_errors);
                read_int(po, "non_ansi_port_max_line_length",
                         "[format.module].non_ansi_port_max_line_length", 1, 10000,
                         cfg.format.module.non_ansi_port_max_line_length, value_errors);
                read_string(po, "parameter_layout", "[format.module].parameter_layout",
                            cfg.format.module.parameter_layout, value_errors);
            }
            if (auto en = (*f)["enum_declaration"].as_table()) {
                read_bool(en, "align", "[format.enum_declaration].align",
                          cfg.format.enum_declaration.align, value_errors);
                read_bool(en, "align_adaptive", "[format.enum_declaration].align_adaptive",
                          cfg.format.enum_declaration.align_adaptive, value_errors);
                read_int(en, "enum_name_min_width",
                         "[format.enum_declaration].enum_name_min_width", 0, 10000,
                         cfg.format.enum_declaration.enum_name_min_width, value_errors);
                read_int(en, "enum_value_min_width",
                         "[format.enum_declaration].enum_value_min_width", 0, 10000,
                         cfg.format.enum_declaration.enum_value_min_width, value_errors);
            }
            if (auto mp = (*f)["modport"].as_table()) {
                read_bool(mp, "align", "[format.modport].align",
                          cfg.format.modport.align, value_errors);
                read_bool(mp, "align_adaptive", "[format.modport].align_adaptive",
                          cfg.format.modport.align_adaptive, value_errors);
                read_int(mp, "direction_min_width", "[format.modport].direction_min_width",
                         0, 10000, cfg.format.modport.direction_min_width, value_errors);
                read_int(mp, "signal_min_width", "[format.modport].signal_min_width", 0,
                         10000, cfg.format.modport.signal_min_width, value_errors);
            }
            if (auto sp = (*f)["spacing"].as_table()) {
                read_bool(sp, "control_keyword_space", "[format.spacing].control_keyword_space",
                          cfg.format.spacing.control_keyword_space, value_errors);
                read_bool(sp, "space_inside_parens", "[format.spacing].space_inside_parens",
                          cfg.format.spacing.space_inside_parens, value_errors);
                read_bool(sp, "space_inside_dimension_brackets",
                          "[format.spacing].space_inside_dimension_brackets",
                          cfg.format.spacing.space_inside_dimension_brackets, value_errors);
                read_string(sp, "binary_operator_spacing",
                            "[format.spacing].binary_operator_spacing",
                            cfg.format.spacing.binary_operator_spacing, value_errors);
                read_string(sp, "dimension_binary_operator_spacing",
                            "[format.spacing].dimension_binary_operator_spacing",
                            cfg.format.spacing.dimension_binary_operator_spacing, value_errors);
                read_string(sp, "semicolon_spacing", "[format.spacing].semicolon_spacing",
                            cfg.format.spacing.semicolon_spacing, value_errors);
                read_string(sp, "range_colon_spacing", "[format.spacing].range_colon_spacing",
                            cfg.format.spacing.range_colon_spacing, value_errors);
                read_string(sp, "indexed_part_select_spacing",
                            "[format.spacing].indexed_part_select_spacing",
                            cfg.format.spacing.indexed_part_select_spacing, value_errors);
                read_string(sp, "procedural_event_control_at_spacing",
                            "[format.spacing].procedural_event_control_at_spacing",
                            cfg.format.spacing.procedural_event_control_at_spacing, value_errors);
                read_bool(sp, "space_inside_event_control_parens",
                          "[format.spacing].space_inside_event_control_parens",
                          cfg.format.spacing.space_inside_event_control_parens, value_errors);
                read_string(sp, "assignment_operator_spacing",
                            "[format.spacing].assignment_operator_spacing",
                            cfg.format.spacing.assignment_operator_spacing, value_errors);
            }
            if (auto macros = (*f)["macros"].as_table()) {
                append_string_array(macros, "object_like_expr",
                                    "[format.macros].object_like_expr",
                                    cfg.format.macros.object_like_expr, value_errors);
                append_string_array(macros, "function_like_expr",
                                    "[format.macros].function_like_expr",
                                    cfg.format.macros.function_like_expr, value_errors);
                append_string_array(macros, "statement_like", "[format.macros].statement_like",
                                    cfg.format.macros.statement_like, value_errors);
                append_string_array(macros, "declaration_like",
                                    "[format.macros].declaration_like",
                                    cfg.format.macros.declaration_like, value_errors);
                append_string_array(macros, "control_flow_like",
                                    "[format.macros].control_flow_like",
                                    cfg.format.macros.control_flow_like, value_errors);
                append_string_array(macros, "block_begin_like",
                                    "[format.macros].block_begin_like",
                                    cfg.format.macros.block_begin_like, value_errors);
                append_string_array(macros, "block_end_like", "[format.macros].block_end_like",
                                    cfg.format.macros.block_end_like, value_errors);
                append_string_array(macros, "whitespace_sensitive",
                                    "[format.macros].whitespace_sensitive",
                                    cfg.format.macros.whitespace_sensitive, value_errors);
            }
        }

        // [lint.*]
        if (auto lint = tbl["lint"].as_table()) {
            auto set_rule = [&](const toml::table* t, const std::string& path,
                                LintRuleConfig& rule) {
                read_bool(t, "enable", path + ".enable", rule.enable, value_errors);
                read_string(t, "severity", path + ".severity", rule.severity, value_errors);
            };

            read_bool(lint, "enable", "[lint].enable", cfg.lint.enable, value_errors);

            if (auto fn = (*lint)["function"].as_table()) {
                set_rule(fn, "[lint.function]", cfg.lint.function);
                read_bool(fn, "functions_automatic", "[lint.function].functions_automatic",
                          cfg.lint.function.functions_automatic, value_errors);
                read_string(fn, "function_call_style", "[lint.function].function_call_style",
                            cfg.lint.function.function_call_style, value_errors);
                read_bool(fn, "explicit_function_lifetime",
                          "[lint.function].explicit_function_lifetime",
                          cfg.lint.function.explicit_function_lifetime, value_errors);
                read_bool(fn, "explicit_task_lifetime",
                          "[lint.function].explicit_task_lifetime",
                          cfg.lint.function.explicit_task_lifetime, value_errors);
            }
            if (auto st = (*lint)["statement"].as_table()) {
                set_rule(st, "[lint.statement]", cfg.lint.statement);
                read_bool(st, "case_missing_default",
                          "[lint.statement].case_missing_default",
                          cfg.lint.statement.case_missing_default, value_errors);
                read_bool(st, "latch_inference_detection",
                          "[lint.statement].latch_inference_detection",
                          cfg.lint.statement.latch_inference_detection, value_errors);
                read_bool(st, "explicit_begin", "[lint.statement].explicit_begin",
                          cfg.lint.statement.explicit_begin, value_errors);
                read_bool(st, "no_raw_always", "[lint.statement].no_raw_always",
                          cfg.lint.statement.no_raw_always, value_errors);
                read_bool(st, "blocking_nonblocking_assignments",
                          "[lint.statement].blocking_nonblocking_assignments",
                          cfg.lint.statement.blocking_nonblocking_assignments, value_errors);
            }
            if (auto style = (*lint)["style"].as_table()) {
                read_bool(style, "trailing_whitespace", "[lint.style].trailing_whitespace",
                          cfg.lint.style.trailing_whitespace, value_errors);
            }
            if (auto mod = (*lint)["module"].as_table()) {
                set_rule(mod, "[lint.module]", cfg.lint.module);
                read_bool(mod, "one_module_per_file", "[lint.module].one_module_per_file",
                          cfg.lint.module.one_module_per_file, value_errors);
            }
            if (auto inst = (*lint)["instance"].as_table()) {
                set_rule(inst, "[lint.instance]", cfg.lint.instance);
                read_string(inst, "module_instantiation_style",
                            "[lint.instance].module_instantiation_style",
                            cfg.lint.instance.module_instantiation_style, value_errors);
                read_bool(inst, "stale_instance_diagnostic",
                          "[lint.instance].stale_instance_diagnostic",
                          cfg.lint.instance.stale_instance_diagnostic, value_errors);
            }
            if (auto nm = (*lint)["naming"].as_table()) {
                set_rule(nm, "[lint.naming]", cfg.lint.naming);
                read_string(nm, "module_pattern", "[lint.naming].module_pattern",
                            cfg.lint.naming.module_pattern, value_errors);
                read_string(nm, "input_port_pattern", "[lint.naming].input_port_pattern",
                            cfg.lint.naming.input_port_pattern, value_errors);
                read_string(nm, "output_port_pattern", "[lint.naming].output_port_pattern",
                            cfg.lint.naming.output_port_pattern, value_errors);
                read_string(nm, "signal_pattern", "[lint.naming].signal_pattern",
                            cfg.lint.naming.signal_pattern, value_errors);
                read_string(nm, "interface_pattern", "[lint.naming].interface_pattern",
                            cfg.lint.naming.interface_pattern, value_errors);
                read_string(nm, "struct_pattern", "[lint.naming].struct_pattern",
                            cfg.lint.naming.struct_pattern, value_errors);
                read_string(nm, "union_pattern", "[lint.naming].union_pattern",
                            cfg.lint.naming.union_pattern, value_errors);
                read_string(nm, "enum_pattern", "[lint.naming].enum_pattern",
                            cfg.lint.naming.enum_pattern, value_errors);
                read_string(nm, "parameter_pattern", "[lint.naming].parameter_pattern",
                            cfg.lint.naming.parameter_pattern, value_errors);
                read_string(nm, "localparam_pattern", "[lint.naming].localparam_pattern",
                            cfg.lint.naming.localparam_pattern, value_errors);
                read_string(nm, "register_pattern", "[lint.naming].register_pattern",
                            cfg.lint.naming.register_pattern, value_errors);
                read_bool(nm, "check_module_filename", "[lint.naming].check_module_filename",
                          cfg.lint.naming.check_module_filename, value_errors);
                read_bool(nm, "check_package_filename",
                          "[lint.naming].check_package_filename",
                          cfg.lint.naming.check_package_filename, value_errors);
            }
        }

        // [rtltree]
        if (auto rt = tbl["rtltree"].as_table()) {
            read_bool(rt, "show_instance_name", "[rtltree].show_instance_name",
                      cfg.rtltree.show_instance_name, value_errors);
            read_bool(rt, "show_file", "[rtltree].show_file", cfg.rtltree.show_file,
                      value_errors);
        }

        // [autoarg]
        if (auto aa = tbl["autoarg"].as_table()) {
            read_bool(aa, "autoarg_on_save", "[autoarg].autoarg_on_save",
                      cfg.autoarg.autoarg_on_save, value_errors);
        }

        // [autowire]
        if (auto aw = tbl["autowire"].as_table()) {
            read_bool(aw, "group_by_instance", "[autowire].group_by_instance",
                      cfg.autowire.group_by_instance, value_errors);
            read_bool(aw, "sort_by_name", "[autowire].sort_by_name",
                      cfg.autowire.sort_by_name, value_errors);
        }

        // [autoff]
        if (auto aff = tbl["autoff"].as_table()) {
            read_string(aff, "register_pattern", "[autoff].register_pattern",
                        cfg.autoff.register_pattern, value_errors);
        }

        // [autofunc]
        if (auto af = tbl["autofunc"].as_table()) {
            read_int(af, "indent_size", "[autofunc].indent_size", 1, 64,
                     cfg.autofunc.indent_size, value_errors);
            read_bool(af, "use_named_arguments", "[autofunc].use_named_arguments",
                      cfg.autofunc.use_named_arguments, value_errors);
        }

        // Unknown top-level keys silently ignored (toml++ doesn't error on them)

    } catch (const toml::parse_error& e) {
        const auto& pos = e.source().begin;
        std::string msg = "lazyverilog.toml parse error";
        if (pos)
            msg +=
                " at line " + std::to_string(pos.line) + ", column " + std::to_string(pos.column);
        msg += ": ";
        msg += std::string(e.description());
        std::cerr << msg << "\n";
        if (warning)
            *warning = msg;
        if (warning_detail) {
            warning_detail->path = toml_path;
            warning_detail->line = pos ? pos.line : 0;
            warning_detail->column = pos ? pos.column : 0;
            warning_detail->message = msg;
        }
    } catch (const std::exception& e) {
        std::string msg = std::string("config load error: ") + e.what();
        std::cerr << msg << "\n";
        if (warning)
            *warning = msg;
        if (warning_detail) {
            warning_detail->path = toml_path;
            warning_detail->message = msg;
        }
    } catch (...) {
        std::string msg = "config load error";
        std::cerr << msg << "\n";
        if (warning)
            *warning = msg;
        if (warning_detail) {
            warning_detail->path = toml_path;
            warning_detail->message = msg;
        }
    }

    // Validate typed/ranged scalar fields, string enums, and cross-option
    // relationships (only when no parse error set a warning).  Invalid typed
    // fields are not assigned while parsing, so callers keep safe defaults for
    // those options and receive all accumulated diagnostics in one warning.
    if (!warning || warning->empty()) {
        auto semantic_errors = validate_config(cfg);
        value_errors.insert(value_errors.end(), semantic_errors.begin(), semantic_errors.end());
        if (!value_errors.empty()) {
            std::string msg = "lazyverilog.toml value error(s):";
            for (const auto& e : value_errors)
                msg += "\n  " + e;
            std::cerr << msg << "\n";
            if (warning)
                *warning = msg;
            if (warning_detail) {
                warning_detail->path = toml_path;
                warning_detail->line = 0;
                warning_detail->column = 0;
                warning_detail->message = msg;
                warning_detail->validation_errors = value_errors;
            }
        }
    }

    return cfg;
}
