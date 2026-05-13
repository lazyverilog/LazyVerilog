#include "analyzer.hpp"
#include "features/rename.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("rename: prepares identifier range and edits all resolved references", "[rename]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rename_fixture.sv";
    analyzer.open(uri, R"(
module top;
    logic a;
    assign a = a;
endmodule
)");

    lsTextDocumentPositionParams prepare_params;
    prepare_params.textDocument.uri.raw_uri_ = uri;
    prepare_params.position = lsPosition(3, 11);
    auto prepared = prepare_rename(analyzer, prepare_params);
    REQUIRE(prepared.has_value());
    CHECK(prepared->placeholder == "a");
    CHECK(prepared->range.start.line == 3);
    CHECK(prepared->range.start.character == 11);

    TextDocumentRename::Params rename_params;
    rename_params.textDocument.uri.raw_uri_ = uri;
    rename_params.position = lsPosition(3, 11);
    rename_params.newName = "next_a";

    auto edit = provide_rename(analyzer, rename_params);
    REQUIRE(edit.changes.has_value());
    REQUIRE(edit.changes->contains(uri));
    const auto& edits = edit.changes->at(uri);
    REQUIRE(edits.size() == 3);
    CHECK(edits[0].range.start.line == 2);
    CHECK(edits[0].newText == "next_a");
    CHECK(edits[1].range.start.line == 3);
    CHECK(edits[2].range.start.line == 3);
}
