#include <catch2/catch_test_macros.hpp>
#include "analyzer.hpp"
#include "features/lint.hpp"

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
    cfg.trailing_whitespace = true;

    auto diags = lint_text("module top;  \nendmodule\n", cfg);

    REQUIRE(diags.size() == 1);
    CHECK(diags[0].line == 0);
    CHECK(diags[0].col == 11);
    CHECK(diags[0].severity == 2);
    CHECK(diags[0].message == "[style] trailing whitespace");
}

TEST_CASE("lint: trailing tabs emit warning", "[lint]") {
    LintConfig cfg;
    cfg.trailing_whitespace = true;

    auto diags = lint_text("module top;\n\t\r\nendmodule\n", cfg);

    REQUIRE(diags.size() == 1);
    CHECK(diags[0].line == 1);
    CHECK(diags[0].col == 0);
    CHECK(diags[0].severity == 2);
}

TEST_CASE("lint: clean text has no trailing whitespace diagnostics", "[lint]") {
    LintConfig cfg;
    cfg.trailing_whitespace = true;

    auto diags = lint_text("module top;\nendmodule\n", cfg);

    CHECK(diags.empty());
}
