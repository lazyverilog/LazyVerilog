#include "analyzer.hpp"
#include "features/references.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("references: verifies tokens against the same syntax-tree definition", "[references]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/references_fixture.sv";
    analyzer.open(uri, R"(
module top;
    logic a;
    logic b;
    assign a = a;
    assign b = a;
endmodule
)");

    TextDocumentReferences::Params params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(4, 11);
    params.context.includeDeclaration = true;

    auto refs = provide_references(analyzer, params);
    REQUIRE(refs.size() == 4);
    CHECK(refs[0].range.start.line == 2);
    CHECK(refs[0].range.start.character == 10);
    CHECK(refs[1].range.start.line == 4);
    CHECK(refs[2].range.start.line == 4);
    CHECK(refs[3].range.start.line == 5);

    params.context.includeDeclaration = false;
    refs = provide_references(analyzer, params);
    REQUIRE(refs.size() == 3);
    CHECK(refs[0].range.start.line == 4);
}
