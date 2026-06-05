#include "config.hpp"
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <unordered_map>
#include <toml++/toml.hpp>

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
    check_enum(cfg.lint.module.module_instantiation_style,
               "[lint.module].module_instantiation_style", {"positional", "named", "both"});
    check_enum(cfg.lint.function.function_call_style, "[lint.function].function_call_style",
               {"positional", "named", "both"});
    check_enum(cfg.format.function.break_policy, "[format.function_call].break_policy",
               {"auto", "always", "never"});
    check_enum(cfg.format.function.layout, "[format.function_call].layout", {"block", "hanging"});
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
    try {
        auto tbl = toml::parse_file(toml_path.string());

        // [design]
        if (auto d = tbl["design"].as_table()) {
            if (auto v = (*d)["vcode"].value<std::string>())
                cfg.design.vcode = *v;
            if (auto arr = (*d)["define"].as_array()) {
                arr->for_each([&](auto&& el) {
                    if constexpr (toml::is_string<std::remove_cvref_t<decltype(el)>>) {
                        cfg.design.define.push_back(*el);
                    }
                });
            }
        }

        // [compilation]
        if (auto p = tbl["compilation"].as_table()) {
            if (auto v = (*p)["background_compilation"].value<bool>())
                cfg.compilation.background_compilation = *v;
            if (auto v = (*p)["background_compilation_threads"].value<int64_t>())
                cfg.compilation.background_compilation_threads = static_cast<int>(*v);
            if (auto v = (*p)["background_compilation_debounce_ms"].value<int64_t>())
                cfg.compilation.background_compilation_debounce_ms = static_cast<int>(*v);
            if (auto v = (*p)["nice_value"].value<int64_t>())
                cfg.compilation.nice_value = static_cast<int>(*v);
            if (auto v = (*p)["log_timing"].value<bool>())
                cfg.compilation.log_timing = *v;
        }

        // [inlay_hint]
        if (auto ih = tbl["inlay_hint"].as_table()) {
            if (auto v = (*ih)["enable"].value<bool>())
                cfg.inlay_hint.enable = *v;
        }

        // [format]
        if (auto f = tbl["format"].as_table()) {
            if (auto v = (*f)["indent_size"].value<int64_t>())
                cfg.format.indent_size = static_cast<int>(*v);
            if (auto v = (*f)["blank_lines_between_items"].value<int64_t>())
                cfg.format.blank_lines_between_items = static_cast<int>(*v);
            if (auto v = (*f)["default_indent_level_inside_outmost_block"].value<int64_t>())
                cfg.format.default_indent_level_inside_outmost_block = static_cast<int>(*v);
            if (auto v = (*f)["enable_format_on_save"].value<bool>())
                cfg.format.enable_format_on_save = *v;
            if (auto v = (*f)["safe_mode"].value<bool>())
                cfg.format.safe_mode = *v;
            if (auto v = (*f)["tab_align"].value<bool>())
                cfg.format.tab_align = *v;
            if (auto v = (*f)["format_off_comment_pattern"].value<std::string>())
                cfg.format.format_off_comment_pattern = *v;
            if (auto v = (*f)["format_on_comment_pattern"].value<std::string>())
                cfg.format.format_on_comment_pattern = *v;
            if (auto v = (*f)["log_path"].value<std::string>())
                cfg.format.log_path = *v;
            // Nested subtables
            if (auto st = (*f)["statement"].as_table()) {
                if (auto v = (*st)["align"].value<bool>())
                    cfg.format.statement.align = *v;
                if (auto v = (*st)["align_adaptive"].value<bool>())
                    cfg.format.statement.align_adaptive = *v;
                if (auto v = (*st)["lhs_min_width"].value<int64_t>())
                    cfg.format.statement.lhs_min_width = static_cast<int>(*v);
                if (auto v = (*st)["begin_newline"].value<bool>())
                    cfg.format.statement.begin_newline = *v;
                if (auto v = (*st)["wrap_end_else_clauses"].value<bool>())
                    cfg.format.statement.wrap_end_else_clauses = *v;
            }
            if (auto pd = (*f)["port_declaration"].as_table()) {
                if (auto v = (*pd)["align"].value<bool>())
                    cfg.format.port_declaration.align = *v;
                if (auto v = (*pd)["align_adaptive"].value<bool>())
                    cfg.format.port_declaration.align_adaptive = *v;
                if (auto v = (*pd)["section1_min_width"].value<int64_t>())
                    cfg.format.port_declaration.section1_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section2_min_width"].value<int64_t>())
                    cfg.format.port_declaration.section2_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section3_min_width"].value<int64_t>())
                    cfg.format.port_declaration.section3_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section4_min_width"].value<int64_t>())
                    cfg.format.port_declaration.section4_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section5_min_width"].value<int64_t>())
                    cfg.format.port_declaration.section5_min_width = static_cast<int>(*v);
            }
            if (auto vd = (*f)["var_declaration"].as_table()) {
                if (auto v = (*vd)["align"].value<bool>())
                    cfg.format.var_declaration.align = *v;
                if (auto v = (*vd)["align_adaptive"].value<bool>())
                    cfg.format.var_declaration.align_adaptive = *v;
                if (auto v = (*vd)["section1_min_width"].value<int64_t>())
                    cfg.format.var_declaration.section1_min_width = static_cast<int>(*v);
                if (auto v = (*vd)["section2_min_width"].value<int64_t>())
                    cfg.format.var_declaration.section2_min_width = static_cast<int>(*v);
                if (auto v = (*vd)["section3_min_width"].value<int64_t>())
                    cfg.format.var_declaration.section3_min_width = static_cast<int>(*v);
                if (auto v = (*vd)["section4_min_width"].value<int64_t>())
                    cfg.format.var_declaration.section4_min_width = static_cast<int>(*v);
            }
            if (auto inst = (*f)["instance"].as_table()) {
                if (auto v = (*inst)["align"].value<bool>())
                    cfg.format.instance.align = *v;
                if (auto v = (*inst)["port_indent_level"].value<int64_t>())
                    cfg.format.instance.port_indent_level = static_cast<int>(*v);
                if (auto v = (*inst)["instance_port_name_width"].value<int64_t>())
                    cfg.format.instance.instance_port_name_width = static_cast<int>(*v);
                if (auto v = (*inst)["instance_port_between_paren_width"].value<int64_t>())
                    cfg.format.instance.instance_port_between_paren_width = static_cast<int>(*v);
                if (auto v = (*inst)["align_adaptive"].value<bool>())
                    cfg.format.instance.align_adaptive = *v;
            }
            if (auto fn = (*f)["function_call"].as_table()) {
                if (auto v = (*fn)["break_policy"].value<std::string>())
                    cfg.format.function.break_policy = *v;
                if (auto v = (*fn)["line_length"].value<int64_t>())
                    cfg.format.function.line_length = static_cast<int>(*v);
                if (auto v = (*fn)["arg_count"].value<int64_t>())
                    cfg.format.function.arg_count = static_cast<int>(*v);
                if (auto v = (*fn)["layout"].value<std::string>())
                    cfg.format.function.layout = *v;
                if (auto v = (*fn)["space_before_paren"].value<bool>())
                    cfg.format.function.space_before_paren = *v;
                if (auto v = (*fn)["space_inside_paren"].value<bool>())
                    cfg.format.function.space_inside_paren = *v;
            }
            if (auto fd = (*f)["function_declaration"].as_table()) {
                if (auto v = (*fd)["layout"].value<std::string>())
                    cfg.format.function_declaration.layout = *v;
                if (auto v = (*fd)["line_length"].value<int64_t>())
                    cfg.format.function_declaration.line_length = static_cast<int>(*v);
            }
            if (auto po = (*f)["module"].as_table()) {
                if (auto v = (*po)["non_ansi_port_per_line_enabled"].value<bool>())
                    cfg.format.module.non_ansi_port_per_line_enabled = *v;
                if (auto v = (*po)["non_ansi_port_per_line"].value<int64_t>())
                    cfg.format.module.non_ansi_port_per_line = static_cast<int>(*v);
                if (auto v = (*po)["non_ansi_port_max_line_length_enabled"].value<bool>())
                    cfg.format.module.non_ansi_port_max_line_length_enabled = *v;
                if (auto v = (*po)["non_ansi_port_max_line_length"].value<int64_t>())
                    cfg.format.module.non_ansi_port_max_line_length = static_cast<int>(*v);
                if (auto v = (*po)["parameter_layout"].value<std::string>())
                    cfg.format.module.parameter_layout = *v;
            }
            if (auto en = (*f)["enum_declaration"].as_table()) {
                if (auto v = (*en)["align"].value<bool>())
                    cfg.format.enum_declaration.align = *v;
                if (auto v = (*en)["align_adaptive"].value<bool>())
                    cfg.format.enum_declaration.align_adaptive = *v;
                if (auto v = (*en)["enum_name_min_width"].value<int64_t>())
                    cfg.format.enum_declaration.enum_name_min_width = static_cast<int>(*v);
                if (auto v = (*en)["enum_value_min_width"].value<int64_t>())
                    cfg.format.enum_declaration.enum_value_min_width = static_cast<int>(*v);
            }
            if (auto mp = (*f)["modport"].as_table()) {
                if (auto v = (*mp)["align"].value<bool>())
                    cfg.format.modport.align = *v;
                if (auto v = (*mp)["align_adaptive"].value<bool>())
                    cfg.format.modport.align_adaptive = *v;
                if (auto v = (*mp)["direction_min_width"].value<int64_t>())
                    cfg.format.modport.direction_min_width = static_cast<int>(*v);
                if (auto v = (*mp)["signal_min_width"].value<int64_t>())
                    cfg.format.modport.signal_min_width = static_cast<int>(*v);
            }
            if (auto sp = (*f)["spacing"].as_table()) {
                if (auto v = (*sp)["control_keyword_space"].value<bool>())
                    cfg.format.spacing.control_keyword_space = *v;
                if (auto v = (*sp)["space_inside_parens"].value<bool>())
                    cfg.format.spacing.space_inside_parens = *v;
                if (auto v = (*sp)["space_inside_dimension_brackets"].value<bool>())
                    cfg.format.spacing.space_inside_dimension_brackets = *v;
                if (auto v = (*sp)["binary_operator_spacing"].value<std::string>())
                    cfg.format.spacing.binary_operator_spacing = *v;
                if (auto v = (*sp)["dimension_binary_operator_spacing"].value<std::string>())
                    cfg.format.spacing.dimension_binary_operator_spacing = *v;
                if (auto v = (*sp)["semicolon_spacing"].value<std::string>())
                    cfg.format.spacing.semicolon_spacing = *v;
                if (auto v = (*sp)["range_colon_spacing"].value<std::string>())
                    cfg.format.spacing.range_colon_spacing = *v;
                if (auto v = (*sp)["indexed_part_select_spacing"].value<std::string>())
                    cfg.format.spacing.indexed_part_select_spacing = *v;
                if (auto v = (*sp)["procedural_event_control_at_spacing"].value<std::string>())
                    cfg.format.spacing.procedural_event_control_at_spacing = *v;
                if (auto v = (*sp)["space_inside_event_control_parens"].value<bool>())
                    cfg.format.spacing.space_inside_event_control_parens = *v;
                if (auto v = (*sp)["assignment_operator_spacing"].value<std::string>())
                    cfg.format.spacing.assignment_operator_spacing = *v;
            }
            if (auto macros = (*f)["macros"].as_table()) {
                auto append_strings = [](const toml::table* t, const char* key,
                                         std::vector<std::string>& dst) {
                    if (auto arr = (*t)[key].as_array()) {
                        arr->for_each([&](auto&& el) {
                            if constexpr (toml::is_string<std::remove_cvref_t<decltype(el)>>)
                                dst.push_back(*el);
                        });
                    }
                };
                append_strings(macros, "object_like_expr", cfg.format.macros.object_like_expr);
                append_strings(macros, "function_like_expr",
                               cfg.format.macros.function_like_expr);
                append_strings(macros, "statement_like", cfg.format.macros.statement_like);
                append_strings(macros, "declaration_like", cfg.format.macros.declaration_like);
                append_strings(macros, "control_flow_like", cfg.format.macros.control_flow_like);
                append_strings(macros, "block_begin_like", cfg.format.macros.block_begin_like);
                append_strings(macros, "block_end_like", cfg.format.macros.block_end_like);
                append_strings(macros, "whitespace_sensitive",
                               cfg.format.macros.whitespace_sensitive);
            }
        }

        // [lint.*]
        if (auto lint = tbl["lint"].as_table()) {
            auto set_bool = [](const toml::table* t, const char* key, bool& field) {
                if (t)
                    if (auto v = (*t)[key].value<bool>())
                        field = *v;
            };
            auto set_rule = [&](const toml::table* t, LintRuleConfig& rule) {
                set_bool(t, "enable", rule.enable);
                if (t)
                    if (auto v = (*t)["severity"].value<std::string>())
                        rule.severity = *v;
            };

            set_bool(lint, "enable", cfg.lint.enable);

            if (auto fn = (*lint)["function"].as_table()) {
                set_rule(fn, cfg.lint.function);
                set_bool(fn, "functions_automatic", cfg.lint.function.functions_automatic);
                if (auto v = (*fn)["function_call_style"].value<std::string>())
                    cfg.lint.function.function_call_style = *v;
                set_bool(fn, "explicit_function_lifetime",
                         cfg.lint.function.explicit_function_lifetime);
                set_bool(fn, "explicit_task_lifetime", cfg.lint.function.explicit_task_lifetime);
            }
            if (auto st = (*lint)["statement"].as_table()) {
                set_rule(st, cfg.lint.statement);
                set_bool(st, "case_missing_default", cfg.lint.statement.case_missing_default);
                set_bool(st, "latch_inference_detection",
                         cfg.lint.statement.latch_inference_detection);
                set_bool(st, "explicit_begin", cfg.lint.statement.explicit_begin);
                set_bool(st, "no_raw_always", cfg.lint.statement.no_raw_always);
                set_bool(st, "blocking_nonblocking_assignments",
                         cfg.lint.statement.blocking_nonblocking_assignments);
            }
            if (auto style = (*lint)["style"].as_table()) {
                set_bool(style, "trailing_whitespace", cfg.lint.style.trailing_whitespace);
            }
            if (auto mod = (*lint)["module"].as_table()) {
                set_rule(mod, cfg.lint.module);
                set_bool(mod, "one_module_per_file", cfg.lint.module.one_module_per_file);
                if (auto v = (*mod)["module_instantiation_style"].value<std::string>())
                    cfg.lint.module.module_instantiation_style = *v;
                set_bool(mod, "stale_autoinst_diagnostic",
                         cfg.lint.module.stale_autoinst_diagnostic);
            }
            if (auto nm = (*lint)["naming"].as_table()) {
                set_rule(nm, cfg.lint.naming);
                if (auto v = (*nm)["module_pattern"].value<std::string>())
                    cfg.lint.naming.module_pattern = *v;
                if (auto v = (*nm)["input_port_pattern"].value<std::string>())
                    cfg.lint.naming.input_port_pattern = *v;
                if (auto v = (*nm)["output_port_pattern"].value<std::string>())
                    cfg.lint.naming.output_port_pattern = *v;
                if (auto v = (*nm)["signal_pattern"].value<std::string>())
                    cfg.lint.naming.signal_pattern = *v;
                if (auto v = (*nm)["interface_pattern"].value<std::string>())
                    cfg.lint.naming.interface_pattern = *v;
                if (auto v = (*nm)["struct_pattern"].value<std::string>())
                    cfg.lint.naming.struct_pattern = *v;
                if (auto v = (*nm)["union_pattern"].value<std::string>())
                    cfg.lint.naming.union_pattern = *v;
                if (auto v = (*nm)["enum_pattern"].value<std::string>())
                    cfg.lint.naming.enum_pattern = *v;
                if (auto v = (*nm)["parameter_pattern"].value<std::string>())
                    cfg.lint.naming.parameter_pattern = *v;
                if (auto v = (*nm)["localparam_pattern"].value<std::string>())
                    cfg.lint.naming.localparam_pattern = *v;
                if (auto v = (*nm)["register_pattern"].value<std::string>())
                    cfg.lint.naming.register_pattern = *v;
                set_bool(nm, "check_module_filename", cfg.lint.naming.check_module_filename);
                set_bool(nm, "check_package_filename", cfg.lint.naming.check_package_filename);
            }
        }

        // [rtltree]
        if (auto rt = tbl["rtltree"].as_table()) {
            if (auto v = (*rt)["show_instance_name"].value<bool>())
                cfg.rtltree.show_instance_name = *v;
            if (auto v = (*rt)["show_file"].value<bool>())
                cfg.rtltree.show_file = *v;
        }

        // [autoarg]
        if (auto aa = tbl["autoarg"].as_table()) {
            if (auto v = (*aa)["autoarg_on_save"].value<bool>())
                cfg.autoarg.autoarg_on_save = *v;
        }

        // [autowire]
        if (auto aw = tbl["autowire"].as_table()) {
            if (auto v = (*aw)["group_by_instance"].value<bool>())
                cfg.autowire.group_by_instance = *v;
            if (auto v = (*aw)["sort_by_name"].value<bool>())
                cfg.autowire.sort_by_name = *v;
        }

        // [autoff]
        if (auto aff = tbl["autoff"].as_table()) {
            if (auto v = (*aff)["register_pattern"].value<std::string>())
                cfg.autoff.register_pattern = *v;
        }

        // [autofunc]
        if (auto af = tbl["autofunc"].as_table()) {
            if (auto v = (*af)["indent_size"].value<int64_t>())
                cfg.autofunc.indent_size = static_cast<int>(*v);
            if (auto v = (*af)["use_named_arguments"].value<bool>())
                cfg.autofunc.use_named_arguments = *v;
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

    // Validate enum/string fields (only when no parse error set a warning)
    if (!warning || warning->empty()) {
        auto val_errors = validate_config(cfg);
        if (!val_errors.empty()) {
            std::string msg = "lazyverilog.toml value error(s):";
            for (const auto& e : val_errors)
                msg += "\n  " + e;
            std::cerr << msg << "\n";
            if (warning)
                *warning = msg;
            if (warning_detail) {
                warning_detail->path = toml_path;
                warning_detail->line = 0;
                warning_detail->column = 0;
                warning_detail->message = msg;
            }
        }
    }

    return cfg;
}
