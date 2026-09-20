// Diagnostic codes for the lint rules -- the `--nowarn` namespace.
//
// The table in lint_codes.cpp is the single definition of both the code text
// and the `--help` description, and a static_assert already pins its row count
// to the enumerator count.  What that assert cannot see is a row carrying the
// wrong id: two rows for one rule and none for another keeps the count right
// while giving one rule another rule's code.  That is what the first case here
// is for.
//
// The rest pin the matching rule, because `--nowarn lint-naming` silencing
// every naming rule and `--nowarn lint-nam` silencing nothing are the same line
// of code seen from two sides.

#include <catch2/catch_test_macros.hpp>

#include "analyzer.hpp"
#include "features/lint.hpp"
#include "features/lint_codes.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<ParseDiagInfo> lint_text(const std::string& text, const LintConfig& cfg) {
    Analyzer analyzer;
    analyzer.open("file:///lint_codes_test.sv", text);
    auto state = analyzer.get_state("file:///lint_codes_test.sv");
    REQUIRE(state != nullptr);
    return run_lint(*state, cfg);
}

bool has_code(const std::vector<ParseDiagInfo>& diags, std::string_view code) {
    return std::any_of(diags.begin(), diags.end(),
                       [&](const ParseDiagInfo& d) { return d.code == code; });
}

} // namespace

TEST_CASE("lint codes are one-to-one with the table", "[lint][lint-codes]") {
    const auto table = all_lint_codes();

    std::set<std::string> codes;
    for (const auto& entry : table) {
        INFO("code: " << entry.code);

        // Every lookup must find its own row.  A duplicated id makes some other
        // enumerator fall through to the front() fallback, and this is what
        // notices.
        CHECK(lint_code_info(entry.id).id == entry.id);
        CHECK(lint_code_info(entry.id).code == entry.code);

        // The `lint-` prefix is what keeps this namespace from ever colliding
        // with slang's option names, and it is what --nowarn dispatches on.
        CHECK(entry.code.rfind("lint-", 0) == 0);
        CHECK(!entry.description.empty());

        CHECK(codes.insert(std::string(entry.code)).second);
    }

    CHECK(codes.size() == table.size());
    CHECK(codes.size() == static_cast<size_t>(LintCode::Count));
}

TEST_CASE("a lint code matches itself and its hierarchy", "[lint][lint-codes]") {
    CHECK(lint_code_matches("lint-naming-module", "lint-naming-module"));
    CHECK(lint_code_matches("lint-naming", "lint-naming-module"));
    CHECK(lint_code_matches("lint", "lint-naming-module"));

    // Only on separator boundaries: a truncated code is a typo, not a prefix.
    CHECK_FALSE(lint_code_matches("lint-nam", "lint-naming-module"));
    CHECK_FALSE(lint_code_matches("lint-naming-mod", "lint-naming-module"));

    // And never the other way round -- a broader code is not silenced by a
    // narrower one.
    CHECK_FALSE(lint_code_matches("lint-naming-module", "lint-naming"));
    CHECK_FALSE(lint_code_matches("lint-style", "lint-naming-module"));
}

TEST_CASE("a --nowarn value is known only when it silences a rule", "[lint][lint-codes]") {
    CHECK(is_known_lint_nowarn("lint"));
    CHECK(is_known_lint_nowarn("lint-naming"));
    CHECK(is_known_lint_nowarn("lint-style-trailing-whitespace"));

    CHECK_FALSE(is_known_lint_nowarn("lint-nam"));
    CHECK_FALSE(is_known_lint_nowarn("lint-bogus"));
    CHECK_FALSE(is_known_lint_nowarn("width-trunc"));
    CHECK_FALSE(is_known_lint_nowarn(""));
}

TEST_CASE("a lint diagnostic carries the code of the rule that made it",
          "[lint][lint-codes]") {
    // Asserted against the table rather than against a literal, so renaming a
    // code in one place cannot leave this test agreeing with the old spelling.
    SECTION("style") {
        LintConfig cfg;
        cfg.style.trailing_whitespace = true;
        auto diags = lint_text("module top;  \nendmodule\n", cfg);
        REQUIRE(diags.size() == 1);
        CHECK(diags[0].code == lint_code_info(LintCode::StyleTrailingWhitespace).code);
    }

    SECTION("naming, per rule and not per category") {
        LintConfig cfg;
        cfg.naming.enable = true;
        cfg.naming.module_pattern = "^m_.*$";
        cfg.naming.input_port_pattern = "^i_.*$";

        auto diags = lint_text("module top (input logic clk);\nendmodule\n", cfg);

        CHECK(has_code(diags, lint_code_info(LintCode::NamingModule).code));
        CHECK(has_code(diags, lint_code_info(LintCode::NamingInputPort).code));
        // Two rules in one category must not collapse onto one code, or
        // --nowarn could not tell them apart.
        CHECK(lint_code_info(LintCode::NamingModule).code !=
              lint_code_info(LintCode::NamingInputPort).code);
    }

    SECTION("statement") {
        LintConfig cfg;
        cfg.statement.enable = true;
        cfg.statement.case_missing_default = true;

        auto diags = lint_text("module top;\n"
                               "  always_comb begin\n"
                               "    case (1'b0)\n"
                               "      1'b0: ;\n"
                               "    endcase\n"
                               "  end\n"
                               "endmodule\n",
                               cfg);

        CHECK(has_code(diags, lint_code_info(LintCode::StatementCaseMissingDefault).code));
    }
}

TEST_CASE("every lint diagnostic carries a code", "[lint][lint-codes]") {
    // A rule added without one reports an empty code, which --nowarn can never
    // match and which prints as a bare message -- silently un-suppressible.
    LintConfig cfg;
    cfg.naming.enable = true;
    cfg.naming.module_pattern = "^m_.*$";
    cfg.naming.input_port_pattern = "^i_.*$";
    cfg.naming.signal_pattern = "^s_.*$";
    cfg.style.trailing_whitespace = true;
    cfg.module.one_module_per_file = true;
    cfg.statement.enable = true;
    cfg.statement.case_missing_default = true;
    cfg.statement.explicit_begin = true;
    cfg.statement.no_raw_always = true;

    auto diags = lint_text("module top (input logic clk);  \n"
                           "  wire w;\n"
                           "  always @(*) if (clk) ;\n"
                           "endmodule\n"
                           "module other;\n"
                           "endmodule\n",
                           cfg);

    REQUIRE(!diags.empty());
    for (const auto& d : diags) {
        INFO("message: " << d.message);
        CHECK(!d.code.empty());
        CHECK(is_known_lint_nowarn(d.code));
    }
}
