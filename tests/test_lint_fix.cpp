// Fixes attached to slang's conversion warnings (src/features/fix_slang.cpp).
// Every fix is checked the same way: the fixed text recompiles without the
// diagnostic and without any diagnostic code the original text did not have.

#include <catch2/catch_test_macros.hpp>

#include "diagnostic_code.hpp"
#include "features/fix_slang.hpp"
#include "fix_apply.hpp"
#include "lsp_position.hpp"

#include <set>
#include <string>
#include <vector>

#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/ParameterSymbols.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>

namespace {

std::vector<ParseDiagInfo> compile_and_collect(std::string_view source) {
    slang::SourceManager sm;
    auto tree = slang::syntax::SyntaxTree::fromText(source, sm);
    slang::ast::Compilation compilation;
    compilation.addSyntaxTree(tree);
    slang::DiagnosticEngine engine(sm);
    SlangFixContext fix_context;
    std::vector<ParseDiagInfo> out;
    for (const auto& diag : compilation.getAllDiagnostics()) {
        ParseDiagInfo info;
        auto loc = sm.getFullyExpandedLoc(diag.location);
        info.line = static_cast<int>(sm.getFileLineNumber(loc)) - 1;
        info.col = lsp_column(sm, loc);
        info.code = slang_diagnostic_code(engine, diag.code);
        info.message = engine.formatMessage(diag);
        info.fixes = slang_fixes(sm, diag, fix_context);
        out.push_back(std::move(info));
    }
    return out;
}

std::string fix_codes(std::string_view source, const std::set<std::string>& codes) {
    auto diags = compile_and_collect(source);
    std::vector<const FixInfo*> fixes;
    for (const auto& d : diags)
        if (codes.count(d.code))
            for (const auto& f : d.fixes)
                fixes.push_back(&f);
    return apply_fixes(source, fixes).text;
}

std::string fix_all(std::string_view source, std::string_view code) {
    return fix_codes(source, {std::string(code)});
}

std::multiset<std::string> codes_of(const std::vector<ParseDiagInfo>& diags) {
    std::multiset<std::string> codes;
    for (const auto& d : diags)
        codes.insert(d.code);
    return codes;
}

// Codes in @p fixed that @p src did not have.
std::set<std::string> new_codes(std::string_view src, std::string_view fixed) {
    auto before = codes_of(compile_and_collect(src));
    std::set<std::string> added;
    for (const auto& c : codes_of(compile_and_collect(fixed)))
        if (!before.count(c))
            added.insert(c);
    return added;
}

// The fixes of the diagnostics with @p code; requires at least one such diagnostic.
std::vector<FixInfo> fixes_of(std::string_view source, std::string_view code) {
    std::vector<FixInfo> fixes;
    bool seen = false;
    for (const auto& d : compile_and_collect(source)) {
        if (d.code != code)
            continue;
        seen = true;
        fixes.insert(fixes.end(), d.fixes.begin(), d.fixes.end());
    }
    REQUIRE(seen);
    return fixes;
}

// Apply the @p code fixes, require @p expected in the result and a clean recompile.
void check_fix(std::string_view src, std::string_view code, std::string_view expected) {
    auto fixed = fix_all(src, code);
    INFO(fixed);
    CHECK(fixed.find(expected) != std::string::npos);
    CHECK(codes_of(compile_and_collect(fixed)).count(std::string(code)) == 0);
    CHECK(new_codes(src, fixed) == std::set<std::string>{});
}

// The values of the top module's localparams, in declaration order.
std::vector<std::string> param_values(std::string_view source) {
    slang::SourceManager sm;
    auto tree = slang::syntax::SyntaxTree::fromText(source, sm);
    slang::ast::Compilation compilation;
    compilation.addSyntaxTree(tree);
    std::vector<std::string> values;
    auto& top = *compilation.getRoot().topInstances[0];
    for (auto& param : top.body.membersOfType<slang::ast::ParameterSymbol>())
        values.push_back(param.getValue().toString());
    return values;
}

// Apply the @p codes fixes @p passes times: @p expected appears, the codes are
// gone, nothing new appears, and every localparam keeps its value.
std::string check_values(std::string_view src, const std::set<std::string>& codes,
                         std::string_view expected, int passes = 1) {
    std::string fixed(src);
    for (int i = 0; i < passes; ++i)
        fixed = fix_codes(fixed, codes);
    INFO(fixed);
    CHECK(fixed.find(expected) != std::string::npos);
    auto after = codes_of(compile_and_collect(fixed));
    for (const auto& code : codes)
        CHECK(after.count(code) == 0);
    CHECK(new_codes(src, fixed) == std::set<std::string>{});
    auto before = param_values(src);
    REQUIRE(before.size() >= 2);
    CHECK(param_values(fixed) == before);
    return fixed;
}

std::string title_of(std::string_view src, std::string_view code) {
    auto fixes = fixes_of(src, code);
    REQUIRE(fixes.size() == 1);
    CHECK(fixes[0].safe);
    return fixes[0].title;
}

} // namespace

TEST_CASE("lint-fix: width-trunc casts to the target width", "[lint-fix]") {
    const char* src = "module m(input logic [15:0] b, output logic [7:0] a);\n"
                      "  assign a = b;\nendmodule\n";
    check_fix(src, "width-trunc", "assign a = 8'(b);");
    CHECK(title_of(src, "width-trunc") == "Cast to 8 bits");
}

TEST_CASE("lint-fix: width-expand casts to the target width", "[lint-fix]") {
    const char* src = "module m(input logic [3:0] b, output logic [7:0] a);\n"
                      "  assign a = b;\nendmodule\n";
    check_fix(src, "width-expand", "assign a = 8'(b);");
}

TEST_CASE("lint-fix: a signed target keeps a sign-neutral operand sign-neutral", "[lint-fix]") {
    // `x == y` has no effective sign, so slang reports no sign-conversion; a bare
    // 8'(...) would make it unsigned and earn one.
    const char* src = "module m(input logic x, y, output logic signed [7:0] a);\n"
                      "  assign a = x == y;\nendmodule\n";
    check_fix(src, "width-expand", "assign a = signed'(8'(x == y));");
    CHECK(title_of(src, "width-expand") == "Cast to 8 bits");
}

TEST_CASE("lint-fix: a signed operand into a signed target gets a bare width cast", "[lint-fix]") {
    const char* src = "module m(input logic signed [15:0] b, output logic signed [7:0] a);\n"
                      "  assign a = b;\nendmodule\n";
    check_fix(src, "width-trunc", "assign a = 8'(b);");
}

TEST_CASE("lint-fix: width and sign fixes on one operand compose", "[lint-fix]") {
    const char* src = "module m(input logic [15:0] b, output logic signed [7:0] a);\n"
                      "  assign a = b;\nendmodule\n";
    auto before = codes_of(compile_and_collect(src));
    REQUIRE(before.count("width-trunc") == 1);
    REQUIRE(before.count("sign-conversion") == 1);
    auto fixed = fix_codes(src, {"width-trunc", "sign-conversion"});
    INFO(fixed);
    CHECK((fixed.find("assign a = signed'(8'(b));") != std::string::npos ||
           fixed.find("assign a = 8'(signed'(b));") != std::string::npos));
    auto after = codes_of(compile_and_collect(fixed));
    CHECK(after.count("width-trunc") == 0);
    CHECK(after.count("sign-conversion") == 0);
    CHECK(new_codes(src, fixed) == std::set<std::string>{});
}

TEST_CASE("lint-fix: an input argument is cast", "[lint-fix]") {
    const char* src = "module m(input logic [15:0] b, output logic [7:0] a);\n"
                      "  function automatic logic [7:0] f(input logic [7:0] x); return x; endfunction\n"
                      "  assign a = f(b);\nendmodule\n";
    check_fix(src, "width-trunc", "assign a = f(8'(b));");
}

TEST_CASE("lint-fix: a named argument is cast", "[lint-fix]") {
    const char* src = "module m(input logic [15:0] b, output logic [7:0] a);\n"
                      "  function automatic logic [7:0] f(input logic [7:0] x); return x; endfunction\n"
                      "  assign a = f(.x(b));\nendmodule\n";
    check_fix(src, "width-trunc", "assign a = f(.x(8'(b)));");
}

TEST_CASE("lint-fix: a task argument is cast", "[lint-fix]") {
    const char* src = "module m(input logic [15:0] b);\n"
                      "  task automatic t(input logic [7:0] x); endtask\n"
                      "  initial t(b);\nendmodule\n";
    check_fix(src, "width-trunc", "initial t(8'(b));");
}

TEST_CASE("lint-fix: an input port connection is cast", "[lint-fix]") {
    const char* src = "module sub(input logic [7:0] d); endmodule\n"
                      "module m(input logic [15:0] b);\n"
                      "  sub u(.d(b));\nendmodule\n";
    check_fix(src, "port-width-trunc", "sub u(.d(8'(b)));");
    CHECK(title_of(src, "port-width-trunc") == "Cast to 8 bits");
}

TEST_CASE("lint-fix: an input port connection is widened", "[lint-fix]") {
    const char* src = "module sub(input logic [7:0] d); endmodule\n"
                      "module m(input logic [3:0] b);\n"
                      "  sub u(.d(b));\nendmodule\n";
    check_fix(src, "port-width-expand", "sub u(.d(8'(b)));");
}

TEST_CASE("lint-fix: an output argument gets no fix", "[lint-fix]") {
    const char* src = "module m;\n"
                      "  logic [7:0] a;\n"
                      "  task automatic t(output logic [15:0] x); x = '0; endtask\n"
                      "  initial t(a);\nendmodule\n";
    CHECK(fixes_of(src, "width-trunc").empty());
}

TEST_CASE("lint-fix: an output argument of another type gets no fix", "[lint-fix]") {
    const char* src = "module m;\n"
                      "  real r;\n"
                      "  task automatic t(output logic [7:0] x); x = '0; endtask\n"
                      "  initial t(r);\nendmodule\n";
    CHECK(fixes_of(src, "int-float-conv").empty());
}

TEST_CASE("lint-fix: an output port connection gets no fix", "[lint-fix]") {
    const char* src = "module sub(output logic [15:0] q); assign q = '0; endmodule\n"
                      "module m;\n"
                      "  logic [7:0] a;\n"
                      "  sub u(.q(a));\nendmodule\n";
    CHECK(fixes_of(src, "port-width-trunc").empty());
}

TEST_CASE("lint-fix: a conversion written in a macro body gets no fix", "[lint-fix]") {
    const char* src = "`define ASSIGN assign a = b;\n"
                      "module m(input logic [15:0] b, output logic [7:0] a);\n"
                      "  `ASSIGN\nendmodule\n";
    CHECK(fixes_of(src, "width-trunc").empty());
}

TEST_CASE("lint-fix: a conversion in a macro argument gets no fix", "[lint-fix]") {
    const char* src = "`define ID(x) x\n"
                      "module m(input logic [15:0] b, output logic [7:0] a);\n"
                      "  assign a = `ID(b);\nendmodule\n";
    CHECK(fixes_of(src, "width-trunc").empty());
}

TEST_CASE("lint-fix: implicit-conv casts to the target typedef", "[lint-fix]") {
    const char* src = "typedef enum logic [1:0] {A, B} e_t;\n"
                      "typedef struct packed { logic [1:0] f; } s_t;\n"
                      "module m(input e_t x, output s_t y);\n"
                      "  assign y = x;\nendmodule\n";
    check_fix(src, "implicit-conv", "assign y = s_t'(x);");
    CHECK(title_of(src, "implicit-conv") == "Cast to s_t");
}

TEST_CASE("lint-fix: implicit-conv casts to a typedef imported from a package", "[lint-fix]") {
    const char* src = "package p;\n"
                      "  typedef enum logic [1:0] {A, B} e_t;\n"
                      "  typedef struct packed { logic [1:0] f; } s_t;\n"
                      "endpackage\n"
                      "module m;\n"
                      "  import p::*;\n"
                      "  e_t x; s_t y;\n"
                      "  assign y = x;\nendmodule\n";
    check_fix(src, "implicit-conv", "assign y = s_t'(x);");
}

TEST_CASE("lint-fix: implicit-conv to a typedef not visible by its plain name gets no fix",
          "[lint-fix]") {
    const char* src = "package p;\n"
                      "  typedef enum logic [1:0] {A, B} e_t;\n"
                      "  typedef struct packed { logic [1:0] f; } s_t;\n"
                      "endpackage\n"
                      "module m;\n"
                      "  p::e_t x; p::s_t y;\n"
                      "  assign y = x;\nendmodule\n";
    CHECK(fixes_of(src, "implicit-conv").empty());
}

TEST_CASE("lint-fix: implicit-conv to an anonymous type gets no fix", "[lint-fix]") {
    const char* src = "typedef enum logic [1:0] {A, B} e_t;\n"
                      "module m(input e_t x);\n"
                      "  struct packed { logic [1:0] f; } y;\n"
                      "  assign y = x;\nendmodule\n";
    CHECK(fixes_of(src, "implicit-conv").empty());
}

TEST_CASE("lint-fix: packed-array-conv casts to the target typedef", "[lint-fix]") {
    const char* src = "typedef logic [1:0][3:0] arr_t;\n"
                      "module m(input logic [7:0] b, output arr_t a);\n"
                      "  assign a = b;\nendmodule\n";
    check_fix(src, "packed-array-conv", "assign a = arr_t'(b);");
}

TEST_CASE("lint-fix: packed-array-conv to an anonymous type gets no fix", "[lint-fix]") {
    const char* src = "module m(input logic [7:0] b, output logic [1:0][3:0] a);\n"
                      "  assign a = b;\nendmodule\n";
    CHECK(fixes_of(src, "packed-array-conv").empty());
}

TEST_CASE("lint-fix: sign-conversion casts to signed", "[lint-fix]") {
    const char* src = "module m(input logic [7:0] b, output logic signed [7:0] a);\n"
                      "  assign a = b;\nendmodule\n";
    check_fix(src, "sign-conversion", "assign a = signed'(b);");
    CHECK(title_of(src, "sign-conversion") == "Cast to signed");
}

TEST_CASE("lint-fix: sign-conversion casts to unsigned", "[lint-fix]") {
    const char* src = "module m(input logic signed [7:0] b, output logic [7:0] a);\n"
                      "  assign a = b;\nendmodule\n";
    check_fix(src, "sign-conversion", "assign a = unsigned'(b);");
    CHECK(title_of(src, "sign-conversion") == "Cast to unsigned");
}

TEST_CASE("lint-fix: sign-compare casts the signed operand to unsigned", "[lint-fix]") {
    const char* src = "module m(input logic signed [7:0] s, input logic [7:0] u, output logic o);\n"
                      "  assign o = u < s;\nendmodule\n";
    check_fix(src, "sign-compare", "assign o = u < unsigned'(s);");
    CHECK(title_of(src, "sign-compare") == "Cast to unsigned");
}

TEST_CASE("lint-fix: arith-op-mismatch widens the narrower operand", "[lint-fix]") {
    const char* src = "module m(input logic [7:0] a, input logic [3:0] b, output logic [7:0] c);\n"
                      "  assign c = a + b;\nendmodule\n";
    check_fix(src, "arith-op-mismatch", "assign c = a + 8'(b);");
    CHECK(title_of(src, "arith-op-mismatch") == "Cast b to 8 bits");
}

TEST_CASE("lint-fix: bitwise-op-mismatch widens the narrower operand", "[lint-fix]") {
    const char* src = "module m(input logic [3:0] a, input logic [7:0] b, output logic [7:0] c);\n"
                      "  assign c = a & b;\nendmodule\n";
    check_fix(src, "bitwise-op-mismatch", "assign c = 8'(a) & b;");
    CHECK(title_of(src, "bitwise-op-mismatch") == "Cast a to 8 bits");
}

TEST_CASE("lint-fix: comparison-mismatch gets no fix", "[lint-fix]") {
    // slang raises it only for operands that are not both plain bit vectors
    // (bit vectors of one sign never warn, of two signs warn sign-compare), and
    // a width cast leaves such a pair mismatched.
    const char* src = "typedef struct packed { logic [7:0] f; } s_t;\n"
                      "module m(input s_t s, input logic [3:0] x, output logic o);\n"
                      "  assign o = s == x;\nendmodule\n";
    CHECK(fixes_of(src, "comparison-mismatch").empty());
}

TEST_CASE("lint-fix: float-int-conv converts to a named integer type", "[lint-fix]") {
    const char* src = "module m(input real r);\n"
                      "  int i;\n"
                      "  always_comb i = r;\nendmodule\n";
    check_fix(src, "float-int-conv", "always_comb i = int'(r);");
    CHECK(title_of(src, "float-int-conv") == "Convert to int");
}

TEST_CASE("lint-fix: float-int-conv converts to a typedef", "[lint-fix]") {
    const char* src = "typedef logic [7:0] u8_t;\n"
                      "module m(input real r, output u8_t a);\n"
                      "  always_comb a = r;\nendmodule\n";
    check_fix(src, "float-int-conv", "always_comb a = u8_t'(r);");
}

TEST_CASE("lint-fix: float-int-conv to an unnamed width gets no fix", "[lint-fix]") {
    // 8'(r) would be BadIntegerCast: slang rejects a size cast of a real.
    const char* src = "module m(input real r, output logic signed [7:0] a);\n"
                      "  always_comb a = r;\nendmodule\n";
    CHECK(fixes_of(src, "float-int-conv").empty());
}

TEST_CASE("lint-fix: int-float-conv converts to real", "[lint-fix]") {
    const char* src = "module m(input logic [7:0] b);\n"
                      "  real r;\n"
                      "  always_comb r = b;\nendmodule\n";
    check_fix(src, "int-float-conv", "always_comb r = real'(b);");
    CHECK(title_of(src, "int-float-conv") == "Convert to real");
}

TEST_CASE("lint-fix: int-float-conv converts to shortreal", "[lint-fix]") {
    const char* src = "module m(input logic [7:0] b);\n"
                      "  shortreal r;\n"
                      "  always_comb r = b;\nendmodule\n";
    check_fix(src, "int-float-conv", "always_comb r = shortreal'(b);");
    CHECK(title_of(src, "int-float-conv") == "Convert to shortreal");
}

TEST_CASE("lint-fix: float-widen converts to real", "[lint-fix]") {
    const char* src = "module m(input shortreal s);\n"
                      "  real r;\n"
                      "  always_comb r = s;\nendmodule\n";
    check_fix(src, "float-widen", "always_comb r = real'(s);");
    CHECK(title_of(src, "float-widen") == "Convert to real");
}

TEST_CASE("lint-fix: float-narrow converts to shortreal", "[lint-fix]") {
    const char* src = "module m(input real r);\n"
                      "  shortreal s;\n"
                      "  always_comb s = r;\nendmodule\n";
    check_fix(src, "float-narrow", "always_comb s = shortreal'(r);");
    CHECK(title_of(src, "float-narrow") == "Convert to shortreal");
}

TEST_CASE("lint-fix: int-bool-conv compares with '0", "[lint-fix]") {
    const char* src = "module m(input logic [7:0] a, output logic o);\n"
                      "  always_comb if (a + 1) o = 1; else o = 0;\nendmodule\n";
    check_fix(src, "int-bool-conv", "if ((a + 1) != '0)");
    CHECK(title_of(src, "int-bool-conv") == "Compare with '0");
}

TEST_CASE("lint-fix: float-bool-conv compares with 0.0", "[lint-fix]") {
    const char* src = "module m(input real r, output logic o);\n"
                      "  always_comb if (r) o = 1; else o = 0;\nendmodule\n";
    check_fix(src, "float-bool-conv", "if ((r) != 0.0)");
    CHECK(title_of(src, "float-bool-conv") == "Compare with 0.0");
}

TEST_CASE("lint-fix: sign-conversion on a widening gets no fix", "[lint-fix]") {
    // The operand is extended by its own sign; a sign cast would extend by the
    // target's (b = 4'h8: a = 8 implicitly, -8 through signed'(b)).
    CHECK(fixes_of("module m(input logic [3:0] b, output logic signed [7:0] a);\n"
                   "  assign a = b;\nendmodule\n",
                   "sign-conversion")
              .empty());
    CHECK(fixes_of("module m(input logic signed [3:0] b, output logic [7:0] a);\n"
                   "  assign a = b;\nendmodule\n",
                   "sign-conversion")
              .empty());
}

TEST_CASE("lint-fix: sign-compare on an operator operand gets no fix", "[lint-fix]") {
    // In the comparison s is extended unsigned before the add; inside
    // unsigned'(s + 1) the add runs signed.
    const char* src = "module m(input logic signed [7:0] s, input logic [7:0] u, output logic o);\n"
                      "  assign o = u < s + 1;\nendmodule\n";
    CHECK(fixes_of(src, "sign-compare").empty());
}

TEST_CASE("lint-fix: many widenings in one module each get their own fix", "[lint-fix]") {
    std::string src = "module m(input logic x, y);\n";
    for (int i = 0; i < 40; ++i) {
        auto n = std::to_string(i);
        src += "  logic [3:0] b" + n + "; logic [7:0] a" + n + "; logic signed [7:0] s" + n +
               ";\n  assign a" + n + " = b" + n + ";\n  assign s" + n + " = x == y;\n";
    }
    src += "endmodule\n";
    auto fixed = fix_all(src, "width-expand");
    for (int i = 0; i < 40; ++i) {
        auto n = std::to_string(i);
        CHECK(fixed.find("assign a" + n + " = 8'(b" + n + ");") != std::string::npos);
        CHECK(fixed.find("assign s" + n + " = signed'(8'(x == y));") != std::string::npos);
    }
    CHECK(codes_of(compile_and_collect(fixed)).count("width-expand") == 0);
    CHECK(new_codes(src, fixed) == std::set<std::string>{});
}

TEST_CASE("lint-fix: an operand title is one line of at most 40 chars", "[lint-fix]") {
    const char* src =
        "module m(input logic [7:0] a, input logic [3:0] in0, in1, in2, in3, in4, in5, in6,\n"
        "         in7, output logic [7:0] c);\n"
        "  assign c = a + (in0 +\n      in1 + in2 + in3 + in4 + in5 + in6 + in7);\nendmodule\n";
    auto title = title_of(src, "arith-op-mismatch");
    INFO(title);
    CHECK(title.find('\n') == std::string::npos);
    CHECK(title.find("  ") == std::string::npos);
    CHECK(title.rfind("Cast in0 + in1 + in2", 0) == 0);
    CHECK(title.size() == std::string("Cast ").size() + 40 + std::string(" to 8 bits").size());
    CHECK(title.find("... to 8 bits") != std::string::npos);
}

TEST_CASE("lint-fix: value - width-trunc", "[lint-fix]") {
    check_values("module m;\n"
                 "  function automatic logic [7:0] f(logic [15:0] b);\n"
                 "    logic [7:0] a; a = b; return a;\n  endfunction\n"
                 "  localparam logic [7:0] R0 = f(16'hABCD);\n"
                 "  localparam logic [7:0] R1 = f(16'hFF80);\nendmodule\n",
                 {"width-trunc"}, "a = 8'(b);");
}

TEST_CASE("lint-fix: value - width-expand into a signed target", "[lint-fix]") {
    check_values("module m;\n"
                 "  function automatic logic signed [7:0] f(logic x, logic y);\n"
                 "    logic signed [7:0] a; a = x == y; return a;\n  endfunction\n"
                 "  localparam logic signed [7:0] R0 = f(1, 1);\n"
                 "  localparam logic signed [7:0] R1 = f(0, 1);\nendmodule\n",
                 {"width-expand"}, "a = signed'(8'(x == y));");
    check_values("module m;\n"
                 "  function automatic logic [7:0] f(logic [3:0] x);\n"
                 "    logic [7:0] a; a = x; return a;\n  endfunction\n"
                 "  localparam logic [7:0] R0 = f(4'hF);\n"
                 "  localparam logic [7:0] R1 = f(4'h8);\nendmodule\n",
                 {"width-expand"}, "a = 8'(x);");
}

TEST_CASE("lint-fix: value - sign-conversion at equal width", "[lint-fix]") {
    check_values("module m;\n"
                 "  function automatic logic signed [7:0] f(logic [7:0] b);\n"
                 "    logic signed [7:0] a; a = b; return a;\n  endfunction\n"
                 "  localparam logic signed [7:0] R0 = f(8'hFF);\n"
                 "  localparam logic signed [7:0] R1 = f(8'h01);\nendmodule\n",
                 {"sign-conversion"}, "a = signed'(b);");
}

TEST_CASE("lint-fix: value - sign-compare with a primary operand", "[lint-fix]") {
    // s = -1 is 8'hFF zero-extended to 16'h00FF in the comparison, not 16'hFFFF.
    check_values("module m;\n"
                 "  function automatic logic f(logic [15:0] u, logic signed [7:0] s);\n"
                 "    return u < s;\n  endfunction\n"
                 "  localparam logic R0 = f(16'h00FF, -8'sd1);\n"
                 "  localparam logic R1 = f(16'h00FE, -8'sd1);\n"
                 "  localparam logic R2 = f(16'h0001, 8'sd16);\nendmodule\n",
                 {"sign-compare"}, "return u < unsigned'(s);");
}

TEST_CASE("lint-fix: value - width and sign on a widening, in two runs", "[lint-fix]") {
    // Run one casts the width only (no sign fix on a widening); run two meets
    // an equal-width sign-conversion.
    const char* src = "module m;\n"
                      "  function automatic logic signed [7:0] f(logic [3:0] b);\n"
                      "    logic signed [7:0] a; a = b; return a;\n  endfunction\n"
                      "  localparam logic signed [7:0] R0 = f(4'h8);\n"
                      "  localparam logic signed [7:0] R1 = f(4'h7);\nendmodule\n";
    CHECK(fix_codes(src, {"width-expand", "sign-conversion"}).find("a = 8'(b);") !=
          std::string::npos);
    check_values(src, {"width-expand", "sign-conversion"}, "a = signed'(8'(b));", 2);
}

TEST_CASE("lint-fix: value - width and sign on a truncation, in one run", "[lint-fix]") {
    auto fixed = check_values("module m;\n"
                              "  function automatic logic signed [7:0] f(logic [15:0] b);\n"
                              "    logic signed [7:0] a; a = b; return a;\n  endfunction\n"
                              "  localparam logic signed [7:0] R0 = f(16'h00F0);\n"
                              "  localparam logic signed [7:0] R1 = f(16'h8170);\nendmodule\n",
                              {"width-trunc", "sign-conversion"}, "a = ");
    CHECK((fixed.find("a = signed'(8'(b));") != std::string::npos ||
           fixed.find("a = 8'(signed'(b));") != std::string::npos));
}

TEST_CASE("lint-fix: sign-conversion on an operator operand gets no fix", "[lint-fix]") {
    // b + c is evaluated 8 bits wide in the assignment (4'hF + 4'hF = 30);
    // inside signed'(b + c) it would be 4 bits wide.
    const char* src = "module m;\n"
                      "  function automatic logic signed [7:0] f(logic [3:0] b, logic [3:0] c);\n"
                      "    logic signed [7:0] a; a = b + c; return a;\n  endfunction\n"
                      "  localparam logic signed [7:0] R0 = f(4'hF, 4'hF);\n"
                      "  localparam logic signed [7:0] R1 = f(4'h1, 4'h2);\nendmodule\n";
    CHECK(fixes_of(src, "sign-conversion").empty());
    // slang raises no width-expand here (the add is context-determined), so
    // nothing at all is rewritten and R0 stays 30.
    CHECK(fix_codes(src, {"width-expand", "sign-conversion"}) == src);
    CHECK(param_values(src).front() == "8'sd30");
}

TEST_CASE("lint-fix: a conversion inside a block scope still gets its fix", "[lint-fix]") {
    check_fix("module m(input logic [15:0] w, output logic [7:0] q);\n"
              "  always_comb begin : blk q = w; end\nendmodule\n",
              "width-trunc", "begin : blk q = 8'(w); end");
    check_fix("module m(input logic [15:0] w, output logic [7:0] z);\n"
              "  always_comb for (int i = 0; i < 2; i++) z = w;\nendmodule\n",
              "width-trunc", "z = 8'(w);");
    check_fix("module m(input logic [15:0] w, output logic [7:0] q);\n"
              "  always_comb begin logic [7:0] t; t = w; q = t; end\nendmodule\n",
              "width-trunc", "t = 8'(w);");
}
