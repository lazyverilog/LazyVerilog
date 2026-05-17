#include <catch2/catch_test_macros.hpp>
#include "analyzer.hpp"
#include "features/lint.hpp"
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
    cfg.module.stale_autoinst_diagnostic = true;

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
    const std::string uri = "file:///lint_top.sv";
    analyzer.open(uri, "module top;\nchild u_child(.a(a), .old(b));\nendmodule\n");
    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);

    SyntaxIndex idx = state->index;
    analyzer.merge_extra_file_modules(idx);

    LintConfig cfg;
    cfg.module.stale_autoinst_diagnostic = true;
    auto diags = run_lint(*state, cfg, &idx);

    CHECK(has_message_containing(diags, "unknown port 'old'"));
    CHECK(has_message_containing(diags, "missing port 'b'"));

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
