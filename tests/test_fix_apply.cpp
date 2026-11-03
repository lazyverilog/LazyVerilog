#include "fix_apply.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
FixEdit ins(int line, int col, const char* t) { return {line, col, line, col, t}; }
FixInfo wrap(int line, int s, int e, const char* open, const char* close) {
    return {"wrap", true, {ins(line, s, open), ins(line, e, close)}};
}
FixInfo repl(int line, int s, int e, const char* t) { return {"r", true, {{line, s, line, e, t}}}; }
} // namespace

TEST_CASE("apply_fixes: a wrap is two insertions", "[fix-apply]") {
    FixInfo f = wrap(0, 11, 12, "8'(", ")");
    auto r = apply_fixes("assign a = b;", {&f});
    CHECK(r.text == "assign a = 8'(b);");
    CHECK(r.applied == 1);
    CHECK(r.skipped == 0);
}

TEST_CASE("apply_fixes: two wraps on one expression nest", "[fix-apply]") {
    FixInfo a = wrap(0, 11, 12, "8'(", ")");
    FixInfo b = wrap(0, 11, 12, "signed'(", ")");
    CHECK(apply_fixes("assign a = b;", {&a, &b}).text == "assign a = signed'(8'(b));");
}

TEST_CASE("apply_fixes: an overlapping replacement is skipped", "[fix-apply]") {
    FixInfo a = repl(0, 11, 12, "x");
    FixInfo b = repl(0, 10, 13, "y");
    auto r = apply_fixes("assign a = b;", {&a, &b});
    CHECK(r.text == "assign a = x;");
    CHECK(r.applied == 1);
    CHECK(r.skipped == 1);
    CHECK(r.outcomes == std::vector<FixOutcome>{FixOutcome::Applied, FixOutcome::Skipped});
}

TEST_CASE("apply_fixes: CRLF and a multibyte column", "[fix-apply]") {
    FixInfo f = wrap(1, 11, 12, "8'(", ")");
    CHECK(apply_fixes("// \xC3\xA9\r\nassign a = b;\r\n", {&f}).text ==
          "// \xC3\xA9\r\nassign a = 8'(b);\r\n");
    FixInfo g = wrap(0, 12, 13, "8'(", ")");
    CHECK(apply_fixes("/* \xC3\xA9 */ x = b;", {&g}).text == "/* \xC3\xA9 */ x = 8'(b);");
}

TEST_CASE("apply_fixes: an identical fix is applied once", "[fix-apply]") {
    FixInfo a = wrap(0, 11, 12, "8'(", ")");
    FixInfo b = a;
    auto r = apply_fixes("assign a = b;", {&a, &b});
    CHECK(r.text == "assign a = 8'(b);");
    CHECK(r.applied == 1);
    CHECK(r.skipped == 0);
    CHECK(r.outcomes == std::vector<FixOutcome>{FixOutcome::Applied, FixOutcome::Duplicate});
}
