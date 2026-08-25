#include <catch2/catch_test_macros.hpp>
#include "analyzer.hpp"
#include "dynamic_file_index.hpp"
#include "features/lint.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

static std::vector<ParseDiagInfo> lint_text(const std::string& text, const LintConfig& cfg) {
    Analyzer analyzer;
    analyzer.open("file:///lint_test.sv", text);
    auto state = analyzer.get_state("file:///lint_test.sv");
    REQUIRE(state != nullptr);
    return run_lint(*state, cfg);
}

TEST_CASE("lint: trailing whitespace disabled emits no diagnostics", "[lint]") {
    LintConfig cfg;
    auto diags = lint_text("module top; \nendmodule\n", cfg);
    CHECK(diags.empty());
}

TEST_CASE("lint: trailing spaces emit warning at first trailing column", "[lint]") {
    LintConfig cfg;
    cfg.style.trailing_whitespace = true;

    auto diags = lint_text("module top;  \nendmodule\n", cfg);

    REQUIRE(diags.size() == 1);
    CHECK(diags[0].line == 0);
    CHECK(diags[0].col == 11);
    CHECK(diags[0].severity == 2);
    CHECK(diags[0].message == "[style] trailing whitespace");
}

TEST_CASE("lint: trailing tabs emit warning", "[lint]") {
    LintConfig cfg;
    cfg.style.trailing_whitespace = true;

    auto diags = lint_text("module top;\n\t\r\nendmodule\n", cfg);

    REQUIRE(diags.size() == 1);
    CHECK(diags[0].line == 1);
    CHECK(diags[0].col == 0);
    CHECK(diags[0].severity == 2);
}

TEST_CASE("lint: clean text has no trailing whitespace diagnostics", "[lint]") {
    LintConfig cfg;
    cfg.style.trailing_whitespace = true;

    auto diags = lint_text("module top;\nendmodule\n", cfg);

    CHECK(diags.empty());
}

static bool has_message_containing(const std::vector<ParseDiagInfo>& diags,
                                   const std::string& text) {
    return std::any_of(diags.begin(), diags.end(), [&](const auto& d) {
        return d.message.find(text) != std::string::npos;
    });
}

TEST_CASE("lint: module rules emit diagnostics", "[lint]") {
    LintConfig cfg;
    cfg.module.one_module_per_file = true;
    cfg.instance.stale_instance_diagnostic = true;

    auto diags = lint_text("module child(input a, output b);\nendmodule\n"
                           "module top;\n"
                           "child u_child(.a(a), .old(b));\n"
                           "endmodule\n",
                           cfg);

    CHECK(has_message_containing(diags, "more than one module"));
    CHECK(has_message_containing(diags, "unknown port 'old'"));
    CHECK(has_message_containing(diags, "missing port 'b'"));
}

TEST_CASE("lint: stale autoinst uses merged extra-file module ports", "[lint]") {
    const auto extra_path = std::filesystem::temp_directory_path() / "lazyverilog_lint_child.sv";
    {
        std::ofstream out(extra_path);
        REQUIRE(out.good());
        out << "module child(input a, output b);\nendmodule\n";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///lint_top.sv";
    analyzer.open(uri, "module top;\nchild u_child(.a(a), .old(b));\nendmodule\n");
    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);

    auto project_index = analyzer.project_index_snapshot();
    REQUIRE(project_index != nullptr);

    LintConfig cfg;
    cfg.instance.stale_instance_diagnostic = true;
    auto diags = run_lint(*state, cfg, project_index.get());

    CHECK(has_message_containing(diags, "unknown port 'old'"));
    CHECK(has_message_containing(diags, "missing port 'b'"));

    std::filesystem::remove(extra_path);
}

TEST_CASE("lint: stale autoinst ignores parameter-only module entries", "[lint]") {
    const auto extra_path = std::filesystem::temp_directory_path() / "lazyverilog_lint_param_child.sv";
    {
        std::ofstream out(extra_path);
        REQUIRE(out.good());
        out << "module child #(parameter int WIDTH = 8)(input a);\nendmodule\n";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///lint_param_top.sv";
    analyzer.open(uri, "module top;\nchild u_child(.a(a));\nendmodule\n");
    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);

    auto project_index = analyzer.project_index_snapshot();
    REQUIRE(project_index != nullptr);

    LintConfig cfg;
    cfg.instance.stale_instance_diagnostic = true;
    auto diags = run_lint(*state, cfg, project_index.get());

    CHECK(!has_message_containing(diags, "missing port 'WIDTH'"));
    CHECK(!has_message_containing(diags, "missing port 'a'"));
    CHECK(!has_message_containing(diags, "unknown port"));

    std::filesystem::remove(extra_path);
}

TEST_CASE("lint: statement rules emit diagnostics", "[lint]") {
    LintConfig cfg;
    cfg.statement.no_raw_always = true;
    cfg.statement.blocking_nonblocking_assignments = true;
    cfg.statement.explicit_begin = true;

    auto diags = lint_text("module top;\n"
                           "always @(*) a = b;\n"
                           "always_ff @(posedge clk) q = d;\n"
                           "always_comb begin\n"
                           "  y <= a;\n"
                           "  if (a) y = b;\n"
                           "end\n"
                           "endmodule\n",
                           cfg);

    CHECK(has_message_containing(diags, "raw always"));
    CHECK(has_message_containing(diags, "always_ff should use nonblocking"));
    CHECK(has_message_containing(diags, "always_comb should use blocking"));
    CHECK(has_message_containing(diags, "if statement body should use begin/end"));
}

TEST_CASE("lint: naming rules emit diagnostics", "[lint]") {
    LintConfig cfg;
    cfg.naming.enable = true;
    cfg.naming.interface_pattern = ".*_if$";
    cfg.naming.struct_pattern = ".*_t$";
    cfg.naming.union_pattern = ".*_u$";
    cfg.naming.enum_pattern = ".*_e$";
    cfg.naming.parameter_pattern = "P_.*";
    cfg.naming.localparam_pattern = "LP_.*";
    cfg.naming.check_module_filename = true;
    cfg.naming.check_package_filename = true;

    auto diags = lint_text("interface bus;\nendinterface\n"
                           "typedef struct { logic a; } packet;\n"
                           "typedef union { logic a; } word;\n"
                           "typedef enum { IDLE } state;\n"
                           "parameter DEPTH = 1;\n"
                           "localparam WIDTH = 1;\n"
                           "package wrong_pkg;\nendpackage\n"
                           "module wrong_mod;\nendmodule\n",
                           cfg);

    CHECK(has_message_containing(diags, "interface 'bus'"));
    CHECK(has_message_containing(diags, "struct 'packet'"));
    CHECK(has_message_containing(diags, "union 'word'"));
    CHECK(has_message_containing(diags, "enum 'state'"));
    CHECK(has_message_containing(diags, "parameter 'DEPTH'"));
    CHECK(has_message_containing(diags, "localparam 'WIDTH'"));
    CHECK(has_message_containing(diags, "module 'wrong_mod' does not match filename"));
    CHECK(has_message_containing(diags, "package 'wrong_pkg' does not match filename"));
}

TEST_CASE("lint: included files are skipped", "[lint]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto include_path = dir / "lazyverilog_lint_include.svh";
    {
        std::ofstream out(include_path);
        REQUIRE(out.good());
        out << "parameter BAD_NAME = 1;\n";
    }

    const auto top_path = dir / "lazyverilog_lint_top.sv";
    const auto top_uri = uri_from_path(top_path);
    Analyzer analyzer;
    analyzer.open(top_uri, "`include \"lazyverilog_lint_include.svh\"\nmodule top;\nendmodule\n");
    auto state = analyzer.get_state(top_uri);
    REQUIRE(state != nullptr);

    LintConfig cfg;
    cfg.naming.enable = true;
    cfg.naming.parameter_pattern = "^P_.*$";

    auto diags = run_lint(*state, cfg);
    CHECK_FALSE(has_message_containing(diags, "parameter 'BAD_NAME'"));

    std::filesystem::remove(include_path);
}

TEST_CASE("lint: a macro body is not linted at the invocation line", "[lint][macro]") {
    // Code written inside a macro body belongs to whoever wrote the macro.
    // Reporting it against the line that *invokes* the macro shows the user a
    // warning about source they do not own and cannot edit -- on a UVM file
    // every `uvm_*_utils line otherwise contributes several of these.
    LintConfig cfg;
    cfg.statement.explicit_begin = true;

    auto diags = lint_text("`define BARE_IF(C) if (C) $display(\"x\");\n"
                           "module top;\n"
                           "  initial begin\n"
                           "    `BARE_IF(1)\n"
                           "    if (1) $display(\"z\");\n"
                           "  end\n"
                           "endmodule\n",
                           cfg);

    // The user-written `if` on line 5 is still reported ...
    REQUIRE(has_message_containing(diags, "if statement body should use begin/end"));
    // ... and it is the only one: the macro body's `if` is not.
    const auto count = std::count_if(diags.begin(), diags.end(), [](const auto& d) {
        return d.message.find("if statement body should use begin/end") != std::string::npos;
    });
    CHECK(count == 1);
    for (const auto& d : diags) {
        if (d.message.find("if statement body should use begin/end") != std::string::npos)
            CHECK(d.line == 4); // 0-based: line 5, the hand-written if
    }
}

TEST_CASE("lint: a macro argument is still linted, at the call site", "[lint][macro]") {
    // The other half of the rule.  A macro *argument* is text the user typed at
    // the invocation, so it stays lintable -- and must be reported where they
    // typed it, not at a position inside the expansion buffer.  Without this a
    // fix that simply drops every macro-expanded token would also pass the
    // suppression test above.
    LintConfig cfg;
    cfg.naming.enable = true;
    cfg.naming.signal_pattern = "^s_.*$";

    auto diags = lint_text("`define DECL_WIRE(N) logic N;\n"
                           "module top;\n"
                           "  `DECL_WIRE(bad_name)\n"
                           "endmodule\n",
                           cfg);

    REQUIRE(has_message_containing(diags, "bad_name"));
    for (const auto& d : diags) {
        if (d.message.find("bad_name") != std::string::npos)
            CHECK(d.line == 2); // 0-based: line 3, where the user typed it
    }
}

// ── [lint.function] section gating and severity ──────────────────────────────
//
// The lifetime rules used to read neither `[lint.function].enable` nor
// `[lint.function].severity`: they fired whenever their own sub-flag was set
// and always reported at "warning".  These tests pin both to the same contract
// the naming/module/statement sections already follow.

static const char* kLifetimeSource =
    "class c;\n"
    "  function void f();\n"
    "  endfunction\n"
    "  task t();\n"
    "  endtask\n"
    "endclass\n";

TEST_CASE("lint: [lint.function] enable=false disables the lifetime rules", "[lint][function]") {
    LintConfig cfg;
    cfg.function.enable = false;
    cfg.function.explicit_function_lifetime = true;
    cfg.function.explicit_task_lifetime = true;

    auto diags = lint_text(kLifetimeSource, cfg);

    CHECK_FALSE(has_message_containing(diags, "missing explicit lifetime"));
}

TEST_CASE("lint: [lint.function] enable=false disables functions_automatic", "[lint][function]") {
    LintConfig cfg;
    cfg.function.enable = false;
    cfg.function.functions_automatic = true;

    auto diags = lint_text(kLifetimeSource, cfg);

    CHECK_FALSE(has_message_containing(diags, "should use 'automatic' lifetime"));
}

TEST_CASE("lint: [lint.function] enable=false disables function_call_style", "[lint][function]") {
    LintConfig cfg;
    cfg.function.enable = false;
    cfg.function.function_call_style = "named";

    auto diags = lint_text("module top;\n"
                           "  initial f(1);\n"
                           "endmodule\n",
                           cfg);

    CHECK_FALSE(has_message_containing(diags, "named arguments required"));
}

TEST_CASE("lint: [lint.function] severity applies to the lifetime rules", "[lint][function]") {
    LintConfig cfg;
    cfg.function.enable = true;
    cfg.function.severity = "hint";
    cfg.function.explicit_function_lifetime = true;
    cfg.function.explicit_task_lifetime = true;

    auto diags = lint_text(kLifetimeSource, cfg);

    int lifetime_diags = 0;
    for (const auto& d : diags) {
        if (d.message.find("missing explicit lifetime") != std::string::npos) {
            ++lifetime_diags;
            CHECK(d.severity == 3);
        }
    }
    CHECK(lifetime_diags == 2);
}

TEST_CASE("lint: [lint.function] severity applies to functions_automatic", "[lint][function]") {
    LintConfig cfg;
    cfg.function.enable = true;
    cfg.function.severity = "error";
    cfg.function.functions_automatic = true;

    auto diags = lint_text(kLifetimeSource, cfg);

    REQUIRE(has_message_containing(diags, "should use 'automatic' lifetime"));
    for (const auto& d : diags) {
        if (d.message.find("should use 'automatic' lifetime") != std::string::npos)
            CHECK(d.severity == 1);
    }
}

TEST_CASE("lint: the task lifetime message is tagged [task], not [function]",
          "[lint][function]") {
    LintConfig cfg;
    cfg.function.enable = true;
    cfg.function.explicit_task_lifetime = true;

    auto diags = lint_text(kLifetimeSource, cfg);

    REQUIRE(has_message_containing(diags, "task declaration missing explicit lifetime"));
    for (const auto& d : diags) {
        if (d.message.find("task declaration missing explicit lifetime") != std::string::npos)
            CHECK(d.message.rfind("[task] ", 0) == 0);
    }
}
