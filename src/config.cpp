#include "config.hpp"
#include <toml++/toml.hpp>
#include <filesystem>
#include <iostream>

std::filesystem::path find_config_root(const std::filesystem::path& start) {
    auto dir = std::filesystem::is_directory(start) ? start : start.parent_path();
    while (true) {
        if (std::filesystem::exists(dir / "lazyverilog.toml")) return dir;
        auto parent = dir.parent_path();
        if (parent == dir) break; // filesystem root
        dir = parent;
    }
    return {};
}

Config load_config(const std::filesystem::path& root, std::string* warning) {
    Config cfg{};
    auto toml_path = root / "lazyverilog.toml";
    if (!std::filesystem::exists(toml_path)) {
        return cfg;
    }
    try {
        auto tbl = toml::parse_file(toml_path.string());

        // [design]
        if (auto d = tbl["design"].as_table()) {
            if (auto v = (*d)["vcode"].value<std::string>()) cfg.design.vcode = *v;
            if (auto arr = (*d)["define"].as_array()) {
                arr->for_each([&](auto&& el) {
                    if constexpr (toml::is_string<std::remove_cvref_t<decltype(el)>>) {
                        cfg.design.define.push_back(*el);
                    }
                });
            }
        }

        // [perf]
        if (auto p = tbl["perf"].as_table()) {
            if (auto v = (*p)["background_compilation"].value<bool>()) cfg.perf.background_compilation = *v;
            if (auto v = (*p)["nice_value"].value<int64_t>()) cfg.perf.nice_value = static_cast<int>(*v);
            if (auto v = (*p)["log_timing"].value<bool>()) cfg.perf.log_timing = *v;
        }

        // [inlay_hint]
        if (auto ih = tbl["inlay_hint"].as_table()) {
            if (auto v = (*ih)["enable"].value<bool>()) cfg.inlay_hint.enable = *v;
        }

        // [format]
        if (auto f = tbl["format"].as_table()) {
            if (auto v = (*f)["indent_size"].value<int64_t>())   cfg.format.indent_size = static_cast<int>(*v);
            if (auto v = (*f)["compact_indexing_and_selections"].value<bool>()) cfg.format.compact_indexing_and_selections = *v;
            if (auto v = (*f)["keyword_case"].value<std::string>()) cfg.format.keyword_case = *v;
            if (auto v = (*f)["blank_lines_between_items"].value<int64_t>()) cfg.format.blank_lines_between_items = static_cast<int>(*v);
            if (auto v = (*f)["default_indent_level_inside_module_block"].value<int64_t>()) cfg.format.default_indent_level_inside_module_block = static_cast<int>(*v);
            if (auto v = (*f)["enable_format_on_save"].value<bool>()) cfg.format.enable_format_on_save = *v;
            if (auto v = (*f)["safe_mode"].value<bool>())          cfg.format.safe_mode = *v;
            if (auto v = (*f)["trailing_newline"].value<bool>())   cfg.format.trailing_newline = *v;
            if (auto v = (*f)["tab_align"].value<bool>())          cfg.format.tab_align = *v;
            if (auto v = (*f)["align_punctuation"].value<bool>())  cfg.format.align_punctuation = *v;
            // Nested subtables
            if (auto st = (*f)["statement"].as_table()) {
                if (auto v = (*st)["align"].value<bool>())                      cfg.format.statement.align = *v;
                if (auto v = (*st)["align_adaptive"].value<bool>())             cfg.format.statement.align_adaptive = *v;
                if (auto v = (*st)["lhs_min_width"].value<int64_t>())           cfg.format.statement.lhs_min_width = static_cast<int>(*v);
                if (auto v = (*st)["wrap_end_else_clauses"].value<bool>())      cfg.format.statement.wrap_end_else_clauses = *v;
            }
            if (auto pd = (*f)["port_declaration"].as_table()) {
                if (auto v = (*pd)["align"].value<bool>())                      cfg.format.port_declaration.align = *v;
                if (auto v = (*pd)["align_adaptive"].value<bool>())             cfg.format.port_declaration.align_adaptive = *v;
                if (auto v = (*pd)["section1_min_width"].value<int64_t>())      cfg.format.port_declaration.section1_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section2_min_width"].value<int64_t>())      cfg.format.port_declaration.section2_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section3_min_width"].value<int64_t>())      cfg.format.port_declaration.section3_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section4_min_width"].value<int64_t>())      cfg.format.port_declaration.section4_min_width = static_cast<int>(*v);
                if (auto v = (*pd)["section5_min_width"].value<int64_t>())      cfg.format.port_declaration.section5_min_width = static_cast<int>(*v);
            }
            if (auto vd = (*f)["var_declaration"].as_table()) {
                if (auto v = (*vd)["align"].value<bool>())                      cfg.format.var_declaration.align = *v;
                if (auto v = (*vd)["align_adaptive"].value<bool>())             cfg.format.var_declaration.align_adaptive = *v;
                if (auto v = (*vd)["section1_min_width"].value<int64_t>())      cfg.format.var_declaration.section1_min_width = static_cast<int>(*v);
                if (auto v = (*vd)["section2_min_width"].value<int64_t>())      cfg.format.var_declaration.section2_min_width = static_cast<int>(*v);
                if (auto v = (*vd)["section3_min_width"].value<int64_t>())      cfg.format.var_declaration.section3_min_width = static_cast<int>(*v);
                if (auto v = (*vd)["section4_min_width"].value<int64_t>())      cfg.format.var_declaration.section4_min_width = static_cast<int>(*v);
            }
            if (auto inst = (*f)["instance"].as_table()) {
                if (auto v = (*inst)["align"].value<bool>())                            cfg.format.instance.align = *v;
                if (auto v = (*inst)["port_indent_level"].value<int64_t>())             cfg.format.instance.port_indent_level = static_cast<int>(*v);
                if (auto v = (*inst)["instance_port_name_width"].value<int64_t>())      cfg.format.instance.instance_port_name_width = static_cast<int>(*v);
                if (auto v = (*inst)["instance_port_between_paren_width"].value<int64_t>()) cfg.format.instance.instance_port_between_paren_width = static_cast<int>(*v);
                if (auto v = (*inst)["align_adaptive"].value<bool>())                   cfg.format.instance.align_adaptive = *v;
            }
            if (auto fn = (*f)["function_call"].as_table()) {
                if (auto v = (*fn)["break_policy"].value<std::string>())        cfg.format.function.break_policy = *v;
                if (auto v = (*fn)["line_length"].value<int64_t>())             cfg.format.function.line_length = static_cast<int>(*v);
                if (auto v = (*fn)["arg_count"].value<int64_t>())               cfg.format.function.arg_count = static_cast<int>(*v);
                if (auto v = (*fn)["layout"].value<std::string>())              cfg.format.function.layout = *v;
                if (auto v = (*fn)["space_before_paren"].value<bool>())         cfg.format.function.space_before_paren = *v;
                if (auto v = (*fn)["space_inside_paren"].value<bool>())         cfg.format.function.space_inside_paren = *v;
            }
            if (auto po = (*f)["port"].as_table()) {
                if (auto v = (*po)["non_ansi_port_per_line_enabled"].value<bool>())        cfg.format.port.non_ansi_port_per_line_enabled = *v;
                if (auto v = (*po)["non_ansi_port_per_line"].value<int64_t>())             cfg.format.port.non_ansi_port_per_line = static_cast<int>(*v);
                if (auto v = (*po)["non_ansi_port_max_line_length_enabled"].value<bool>()) cfg.format.port.non_ansi_port_max_line_length_enabled = *v;
                if (auto v = (*po)["non_ansi_port_max_line_length"].value<int64_t>())      cfg.format.port.non_ansi_port_max_line_length = static_cast<int>(*v);
            }
        }

        // [lint.*] — flat booleans or nested subtables ([lint.function], etc.)
        if (auto lint = tbl["lint"].as_table()) {
            // Helper: apply a bool key from a table to a field
            auto set_bool = [](const toml::table* t, const char* key, bool& field) {
                if (t) if (auto v = (*t)[key].value<bool>()) field = *v;
            };
            // Flat keys (legacy / simple schema)
            lint->for_each([&](const toml::key& k, auto&& val) {
                if constexpr (toml::is_boolean<std::remove_cvref_t<decltype(val)>>) {
                    bool enabled = *val;
                    auto key = std::string(k.str());
                    if (key == "case_missing_default")        cfg.lint.case_missing_default = enabled;
                    else if (key == "functions_automatic")    cfg.lint.functions_automatic = enabled;
                    else if (key == "explicit_function_lifetime") cfg.lint.explicit_function_lifetime = enabled;
                    else if (key == "explicit_task_lifetime") cfg.lint.explicit_task_lifetime = enabled;
                    else if (key == "module_instantiation_style") cfg.lint.module_instantiation_style = enabled;
                    else if (key == "latch_inference_detection") cfg.lint.latch_inference_detection = enabled;
                    else if (key == "explicit_begin")         cfg.lint.explicit_begin = enabled;
                    else if (key == "register_naming")        cfg.lint.register_naming = enabled;
                }
            });
            // Nested subtable: [lint.function]
            if (auto fn = (*lint)["function"].as_table()) {
                set_bool(fn, "functions_automatic",        cfg.lint.functions_automatic);
                set_bool(fn, "explicit_function_lifetime", cfg.lint.explicit_function_lifetime);
                set_bool(fn, "explicit_task_lifetime",     cfg.lint.explicit_task_lifetime);
            }
            // Nested subtable: [lint.statement]
            if (auto st = (*lint)["statement"].as_table()) {
                set_bool(st, "case_missing_default",    cfg.lint.case_missing_default);
                set_bool(st, "latch_inference_detection", cfg.lint.latch_inference_detection);
                set_bool(st, "explicit_begin",          cfg.lint.explicit_begin);
            }
            // Nested subtable: [lint.style]
            if (auto style = (*lint)["style"].as_table()) {
                set_bool(style, "trailing_whitespace", cfg.lint.trailing_whitespace);
            }
            // Nested subtable: [lint.module]
            if (auto mod = (*lint)["module"].as_table()) {
                // module_instantiation_style can be a string ("", "named", "positional", "both")
                // or a bool. Non-empty non-"positional" string → true.
                if (auto v = (*mod)["module_instantiation_style"].value<std::string>()) {
                    cfg.lint.module_instantiation_style =
                        !v->empty() && *v != "positional";
                } else {
                    set_bool(mod, "module_instantiation_style", cfg.lint.module_instantiation_style);
                }
            }
            // Nested subtable: [lint.naming]
            if (auto nm = (*lint)["naming"].as_table()) {
                set_bool(nm, "enable",                  cfg.lint.naming.enable);
                if (auto v = (*nm)["severity"].value<std::string>())           cfg.lint.naming.severity = *v;
                if (auto v = (*nm)["module_pattern"].value<std::string>())     cfg.lint.naming.module_pattern = *v;
                if (auto v = (*nm)["input_port_pattern"].value<std::string>()) cfg.lint.naming.input_port_pattern = *v;
                if (auto v = (*nm)["output_port_pattern"].value<std::string>()) cfg.lint.naming.output_port_pattern = *v;
                if (auto v = (*nm)["signal_pattern"].value<std::string>())     cfg.lint.naming.signal_pattern = *v;
                if (auto v = (*nm)["interface_pattern"].value<std::string>())  cfg.lint.naming.interface_pattern = *v;
                if (auto v = (*nm)["struct_pattern"].value<std::string>())     cfg.lint.naming.struct_pattern = *v;
                if (auto v = (*nm)["union_pattern"].value<std::string>())      cfg.lint.naming.union_pattern = *v;
                if (auto v = (*nm)["enum_pattern"].value<std::string>())       cfg.lint.naming.enum_pattern = *v;
                if (auto v = (*nm)["parameter_pattern"].value<std::string>())  cfg.lint.naming.parameter_pattern = *v;
                if (auto v = (*nm)["localparam_pattern"].value<std::string>()) cfg.lint.naming.localparam_pattern = *v;
                if (auto v = (*nm)["register_pattern"].value<std::string>())   cfg.lint.naming.register_pattern = *v;
                set_bool(nm, "check_module_filename",   cfg.lint.naming.check_module_filename);
                set_bool(nm, "check_package_filename",  cfg.lint.naming.check_package_filename);
                // Legacy: register_naming bool also set when register_pattern non-empty
                set_bool(nm, "register_naming", cfg.lint.register_naming);
                if (!cfg.lint.naming.register_pattern.empty()) cfg.lint.register_naming = true;
            }
        }

        // [autoinst]
        if (auto ai = tbl["autoinst"].as_table()) {
            if (auto v = (*ai)["align_ports"].value<bool>()) cfg.autoinst.align_ports = *v;
        }

        // [autoarg]
        if (auto aa = tbl["autoarg"].as_table()) {
            if (auto v = (*aa)["indent_size"].value<int64_t>())
                cfg.autoarg.indent_size = static_cast<int>(*v);
            if (auto v = (*aa)["autoarg_on_save"].value<bool>()) cfg.autoarg.autoarg_on_save = *v;
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
        std::string msg = std::string("[lazyverilog] lazyverilog.toml parse error: ") + e.what();
        std::cerr << msg << "\n";
        if (warning) *warning = msg;
    } catch (...) {
        std::cerr << "[lazyverilog] config load error\n";
    }
    return cfg;
}
