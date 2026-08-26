// Clock extraction from procedural blocks, for `lazyverilog-clk`.
//
// The rule under test is deliberately structural: a block is sequential iff its
// event control carries at least one edge, and when it does, *every*
// edge-triggered signal in the list is reported.  No attempt is made to tell a
// clock from an asynchronous reset, so `@(posedge clk or negedge rst_n)` yields
// both names.  These tests pin that behavior so a future "helpful" heuristic
// cannot be added silently.
#include "features/clock_domain.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/syntax/SyntaxTree.h>
#include <string>
#include <vector>

namespace {

struct BlockCollector : slang::ast::ASTVisitor<BlockCollector, false, false> {
    std::vector<const slang::ast::ProceduralBlockSymbol*> blocks;

    void handle(const slang::ast::ProceduralBlockSymbol& block) { blocks.push_back(&block); }
};

/// Keeps the syntax tree and compilation alive alongside the symbols that point
/// into them.
struct Compiled {
    std::shared_ptr<slang::syntax::SyntaxTree> tree;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::vector<const slang::ast::ProceduralBlockSymbol*> blocks;
};

Compiled compile_source(const std::string& source) {
    Compiled compiled;
    compiled.tree = slang::syntax::SyntaxTree::fromText(source);
    compiled.compilation = std::make_unique<slang::ast::Compilation>();
    compiled.compilation->addSyntaxTree(compiled.tree);

    BlockCollector collector;
    compiled.compilation->getRoot().visit(collector);
    compiled.blocks = std::move(collector.blocks);
    return compiled;
}

std::vector<std::string> clock_names(const clock_domain::BlockClocks& clocks) {
    std::vector<std::string> names;
    for (const auto& clock : clocks.clocks)
        names.push_back(clock.name);
    return names;
}

} // namespace

TEST_CASE("always_ff with one posedge reports one clock", "[clkdomain]") {
    auto compiled = compile_source(R"(
module m(input logic clk, input logic d, output logic q);
    always_ff @(posedge clk) q <= d;
endmodule
)");
    REQUIRE(compiled.blocks.size() == 1);

    const auto result = clock_domain::classify_block(*compiled.blocks[0]);
    REQUIRE(result.kind == clock_domain::BlockKind::Sequential);
    REQUIRE(clock_names(result) == std::vector<std::string>{"clk"});
    REQUIRE(result.clocks[0].edge == slang::ast::EdgeKind::PosEdge);
    REQUIRE(result.clocks[0].signal != nullptr);
}

TEST_CASE("async reset is reported alongside the clock, not filtered", "[clkdomain]") {
    auto compiled = compile_source(R"(
module m(input logic clk, input logic rst_n, input logic d, output logic q);
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            q <= 1'b0;
        else
            q <= d;
    end
endmodule
)");
    REQUIRE(compiled.blocks.size() == 1);

    const auto result = clock_domain::classify_block(*compiled.blocks[0]);
    REQUIRE(result.kind == clock_domain::BlockKind::Sequential);
    REQUIRE(clock_names(result) == std::vector<std::string>{"clk", "rst_n"});
    REQUIRE(result.clocks[0].edge == slang::ast::EdgeKind::PosEdge);
    REQUIRE(result.clocks[1].edge == slang::ast::EdgeKind::NegEdge);
}

TEST_CASE("two clocks in one event list are both reported", "[clkdomain]") {
    auto compiled = compile_source(R"(
module m(input logic clk_a, input logic clk_b, input logic d, output logic q);
    always @(posedge clk_a or posedge clk_b) q <= d;
endmodule
)");
    REQUIRE(compiled.blocks.size() == 1);

    const auto result = clock_domain::classify_block(*compiled.blocks[0]);
    REQUIRE(result.kind == clock_domain::BlockKind::Sequential);
    REQUIRE(clock_names(result) == std::vector<std::string>{"clk_a", "clk_b"});
}

TEST_CASE("plain always with an edge is still sequential", "[clkdomain]") {
    auto compiled = compile_source(R"(
module m(input logic clk, input logic d, output logic q);
    always @(posedge clk) q <= d;
endmodule
)");
    REQUIRE(compiled.blocks.size() == 1);

    const auto result = clock_domain::classify_block(*compiled.blocks[0]);
    REQUIRE(result.kind == clock_domain::BlockKind::Sequential);
    REQUIRE(clock_names(result) == std::vector<std::string>{"clk"});
}

TEST_CASE("level-sensitive always is combinational, not a clock domain", "[clkdomain]") {
    auto compiled = compile_source(R"(
module m(input logic a, input logic b, output logic y);
    logic r;
    always @(a or b) r = a & b;
    assign y = r;
endmodule
)");
    REQUIRE(compiled.blocks.size() == 1);

    const auto result = clock_domain::classify_block(*compiled.blocks[0]);
    REQUIRE(result.kind == clock_domain::BlockKind::Combinational);
    REQUIRE(result.clocks.empty());
}

TEST_CASE("always_comb is combinational", "[clkdomain]") {
    auto compiled = compile_source(R"(
module m(input logic a, input logic b, output logic y);
    logic r;
    always_comb r = a ^ b;
    assign y = r;
endmodule
)");
    REQUIRE(compiled.blocks.size() == 1);

    const auto result = clock_domain::classify_block(*compiled.blocks[0]);
    REQUIRE(result.kind == clock_domain::BlockKind::Combinational);
    REQUIRE(result.clocks.empty());
}

TEST_CASE("always_latch is a state endpoint with no clock to report", "[clkdomain]") {
    auto compiled = compile_source(R"(
module m(input logic en, input logic d, output logic q);
    logic r;
    always_latch if (en) r = d;
    assign q = r;
endmodule
)");
    REQUIRE(compiled.blocks.size() == 1);

    const auto result = clock_domain::classify_block(*compiled.blocks[0]);
    REQUIRE(result.kind == clock_domain::BlockKind::Latch);
    REQUIRE(result.clocks.empty());
}
