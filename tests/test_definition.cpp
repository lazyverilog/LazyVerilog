#include "analyzer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>

static const std::string kDefinitionFixture = R"(
module child (
    input logic clk,
    output logic done
);
endmodule

module top;
    child u_child (
        .clk(clk),
        .done(done)
    );
endmodule
)";

TEST_CASE("definition: instance resolves to module declaration", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_fixture.sv";
    analyzer.open(uri, kDefinitionFixture);

    auto loc = analyzer.definition_of(uri, 8, 11);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 1);
    CHECK(loc->col == 7);
    CHECK(loc->end_col == 12);
}

TEST_CASE("definition: named port resolves to port declaration", "[definition]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/definition_fixture.sv";
    analyzer.open(uri, kDefinitionFixture);

    auto loc = analyzer.definition_of(uri, 9, 10);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == uri);
    CHECK(loc->line == 2);
    CHECK(loc->col == 16);
    CHECK(loc->end_col == 19);
}

static std::string read_text(const std::string& path) {
    std::ifstream input(path);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

TEST_CASE("definition: module lookup uses vcode extra files", "[definition]") {
    Analyzer analyzer;
    const std::string top_uri = "file:///home/hxxdev/dev/LazyVerilog/demo/memory_top.sv";
    analyzer.set_extra_files({"/home/hxxdev/dev/LazyVerilog/demo/memory.sv"});
    analyzer.open(top_uri, read_text("/home/hxxdev/dev/LazyVerilog/demo/memory_top.sv"));

    auto loc = analyzer.definition_of(top_uri, 85, 1);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file:///home/hxxdev/dev/LazyVerilog/demo/memory.sv");
    CHECK(loc->line == 0);
    CHECK(loc->col == 7);
}

TEST_CASE("definition: named port lookup uses vcode extra files", "[definition]") {
    Analyzer analyzer;
    const std::string top_uri = "file:///home/hxxdev/dev/LazyVerilog/demo/memory_top.sv";
    analyzer.set_extra_files({"/home/hxxdev/dev/LazyVerilog/demo/memory.sv"});
    analyzer.open(top_uri, read_text("/home/hxxdev/dev/LazyVerilog/demo/memory_top.sv"));

    auto loc = analyzer.definition_of(top_uri, 99, 6);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file:///home/hxxdev/dev/LazyVerilog/demo/memory.sv");
    CHECK(loc->line == 12);
}

TEST_CASE("definition: macro lookup resolves local define", "[definition]") {
    Analyzer analyzer;
    const std::string top_uri = "file:///home/hxxdev/dev/LazyVerilog/demo/memory_top.sv";
    analyzer.open(top_uri, read_text("/home/hxxdev/dev/LazyVerilog/demo/memory_top.sv"));

    auto loc = analyzer.definition_of(top_uri, 52, 24);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 1);
    CHECK(loc->col == 8);
    CHECK(loc->end_col == 13);
}

TEST_CASE("definition: typedef lookup resolves named type", "[definition]") {
    Analyzer analyzer;
    const std::string top_uri = "file:///home/hxxdev/dev/LazyVerilog/demo/memory_top.sv";
    analyzer.open(top_uri, read_text("/home/hxxdev/dev/LazyVerilog/demo/memory_top.sv"));

    auto loc = analyzer.definition_of(top_uri, 20, 12);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 18);
    CHECK(loc->col == 2);
    CHECK(loc->end_col == 10);
}

TEST_CASE("definition: variable lookup prefers same module scope", "[definition]") {
    Analyzer analyzer;
    const std::string top_uri = "file:///home/hxxdev/dev/LazyVerilog/demo/memory_top.sv";
    analyzer.open(top_uri, read_text("/home/hxxdev/dev/LazyVerilog/demo/memory_top.sv"));

    auto loc = analyzer.definition_of(top_uri, 83, 11);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 54);
    CHECK(loc->col == 40);
}

TEST_CASE("definition: named subroutine argument resolves to formal argument", "[definition]") {
    Analyzer analyzer;
    const std::string top_uri = "file:///home/hxxdev/dev/LazyVerilog/demo/memory_top.sv";
    analyzer.open(top_uri, read_text("/home/hxxdev/dev/LazyVerilog/demo/memory_top.sv"));

    auto loc = analyzer.definition_of(top_uri, 181, 16);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == top_uri);
    CHECK(loc->line == 24);
    CHECK(loc->col == 26);
    CHECK(loc->end_col == 27);
}
