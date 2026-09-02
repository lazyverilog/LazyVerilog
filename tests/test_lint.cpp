#include <catch2/catch_test_macros.hpp>
#include "analyzer.hpp"
#include "dynamic_file_index.hpp"
#include "features/lint.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
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
    cfg.statement.enable = true;
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
    cfg.statement.enable = true;
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

// ── [lint.statement] section gating and severity ─────────────────────────────
//
// Same contract as [lint.function] above: the sub-flags select which rules run,
// `enable` turns the whole section off, and `severity` decides how every one of
// its diagnostics is reported.  `case_missing_default` and
// `latch_inference_detection` read neither, so they fired with enable=false and
// always reported at "warning".

static const char* kStatementSource =
    "module top;\n"
    "  logic a, b, c, d;\n"
    "  always @(*) begin\n"
    "    case (a)\n"
    "      1'b0: b = 1'b0;\n"
    "    endcase\n"
    "  end\n"
    "  always_comb begin\n"
    "    if (a) c = 1'b1;\n"
    "  end\n"
    "  always_ff @(posedge a) if (b) d <= 1'b1;\n"
    "endmodule\n";

static LintConfig statement_config(bool enable, const std::string& severity) {
    LintConfig cfg;
    cfg.statement.enable = enable;
    cfg.statement.severity = severity;
    cfg.statement.case_missing_default = true;
    cfg.statement.latch_inference_detection = true;
    cfg.statement.explicit_begin = true;
    cfg.statement.no_raw_always = true;
    cfg.statement.blocking_nonblocking_assignments = true;
    return cfg;
}

TEST_CASE("lint: [lint.statement] enable=false disables every statement rule",
          "[lint][statement]") {
    auto diags = lint_text(kStatementSource, statement_config(false, "warning"));

    CHECK_FALSE(has_message_containing(diags, "[statement]"));
}

TEST_CASE("lint: [lint.statement] severity applies to every statement rule",
          "[lint][statement]") {
    auto diags = lint_text(kStatementSource, statement_config(true, "hint"));

    int statement_diags = 0;
    for (const auto& d : diags) {
        if (d.message.find("[statement]") != std::string::npos) {
            ++statement_diags;
            CHECK(d.severity == 3);
        }
    }
    // case_missing_default, latch_inference_detection, no_raw_always, and
    // explicit_begin all have something to say about the fixture.
    CHECK(statement_diags >= 4);
}

TEST_CASE("lint: [lint.statement] rules still fire with enable=true", "[lint][statement]") {
    auto diags = lint_text(kStatementSource, statement_config(true, "warning"));

    CHECK(has_message_containing(diags, "case statement missing default item"));
    CHECK(has_message_containing(diags, "may infer a latch"));
    CHECK(has_message_containing(diags, "raw always block"));
    CHECK(has_message_containing(diags, "should use begin/end"));
}

// A header shared by every file in a design is the largest thing lint sees, and
// lint has nothing to say about it: run_lint_impl() drops every diagnostic whose
// URI is not the document's own.  Dropping them at the end still pays for the
// regex match, the URI derivation and the line/column resolution on each one, so
// a 30k-line params.svh taxed every keystroke in every module that includes it.
//
// Guarded the way docs/dev/startup-perf.md prescribes: a ratio against a
// structurally identical input rather than a millisecond budget, reporting the
// fastest of several runs, so a loaded CI runner cannot fail it.  Both halves
// use the same module text and differ only in how many declarations the header
// they include contributes.  Parsing happens outside the timed region, so this
// measures lint and nothing else.
namespace {

constexpr int kHeaderScalingDecls = 4000;
constexpr int kHeaderScalingRuns = 7;

std::string scaling_header(int count) {
    std::string text;
    for (int i = 0; i < count; ++i)
        text += "localparam int HDR_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    return text;
}

LintConfig header_scaling_config() {
    LintConfig cfg;
    // Rules that fire per declaration: without them the header costs the walk
    // only and the ratio would not show the work being removed.
    cfg.naming.enable = true;
    cfg.naming.parameter_pattern = "^W_.*$";
    cfg.naming.localparam_pattern = "^LP_.*$";
    cfg.naming.signal_pattern = "^s_.*$";
    return cfg;
}

struct HeaderScalingResult {
    double best_ms{};
    size_t diag_count{};
};

/// Fastest run_lint() over a module preceded by kHeaderScalingDecls
/// declarations.  Those declarations sit in an `include`d header when
/// @p in_header, and in the file itself otherwise.  Everything else -- the
/// declaration text, their count, the module, the config, the machine -- is
/// identical between the two, so the difference is only where they live.
HeaderScalingResult lint_ms(bool in_header) {
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("lazyverilog-lint-header-scaling-") +
                      (in_header ? "included" : "own"));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::string decls = scaling_header(kHeaderScalingDecls);
    std::string module_text;
    if (in_header) {
        std::ofstream out(dir / "defs.svh", std::ios::binary);
        out << decls;
        module_text = "`include \"defs.svh\"\n";
    } else {
        module_text = decls;
    }
    module_text += "module blk (input logic clk_i);\n"
                   "    logic data_q;\n"
                   "    always_ff @(posedge clk_i) data_q <= 1'b1;\n"
                   "endmodule\n";
    const auto module_path = dir / "blk.sv";
    {
        std::ofstream out(module_path, std::ios::binary);
        out << module_text;
    }

    Analyzer analyzer;
    analyzer.set_project_config({}, {dir.string()}, {});
    const auto uri = uri_from_path(module_path);
    analyzer.open(uri, module_text);
    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);
    REQUIRE(state->tree != nullptr);
    // The header half really is expanding a second file; otherwise both halves
    // would be the same input and the comparison would prove nothing.
    REQUIRE(state->include_dependencies.size() == (in_header ? 1u : 0u));

    const auto cfg = header_scaling_config();
    HeaderScalingResult result;
    result.best_ms = std::numeric_limits<double>::max();
    for (int run = 0; run < kHeaderScalingRuns; ++run) {
        const auto start = std::chrono::steady_clock::now();
        auto diags = run_lint(*state, cfg);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        result.diag_count = diags.size();
        result.best_ms =
            std::min(result.best_ms, std::chrono::duration<double, std::milli>(elapsed).count());
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return result;
}

} // namespace

TEST_CASE("lint: an included declaration costs far less than an owned one", "[lint][scaling]") {
    const auto own = lint_ms(/*in_header=*/false);
    const auto included = lint_ms(/*in_header=*/true);

    // Same declarations either way, and lint reports none of them when they are
    // included -- run_lint_impl() drops every diagnostic that is not the
    // document's own.  This is the behaviour the timing check protects.
    CHECK(included.diag_count < own.diag_count);

    INFO("lint ms for " << kHeaderScalingDecls << " decls: own = " << own.best_ms
                        << ", included = " << included.best_ms);
    // Both halves walk the same number of top-level members, so this cannot
    // reach zero -- what it pins is that an included declaration is never
    // analyzed.  Before the subtree skip the two were within noise of each
    // other; a 4x gap cannot be reached by analyzing included text.
    CHECK(included.best_ms < own.best_ms / 4.0);
}
