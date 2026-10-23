#include "analyzer.hpp"
#include "features/document_highlight.hpp"
#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace {

lsTextDocumentPositionParams at(const std::string& uri, int line, int col) {
    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);
    return params;
}

std::string text_of(const std::string& text, const lsRange& range) {
    REQUIRE(range.start.line == range.end.line);
    size_t pos = 0;
    for (int line = 0; line < range.start.line; ++line)
        pos = text.find('\n', pos) + 1;
    return text.substr(pos + range.start.character, range.end.character - range.start.character);
}

} // namespace

TEST_CASE("document highlight: every same-file use of a net", "[document-highlight]") {
    const std::string uri = "file:///tmp/lazyverilog_highlight_net.sv";
    const std::string text = "module m;\n"
                             "  logic a;\n"
                             "  assign a = 1'b0;\n"
                             "  always_comb if (a) ;\n"
                             "endmodule\n";
    Analyzer analyzer;
    analyzer.open(uri, text);

    const auto result = provide_document_highlight(analyzer, at(uri, 2, 9));
    REQUIRE(result.size() == 3);
    for (const auto& highlight : result)
        CHECK(text_of(text, highlight.range) == "a");
}

TEST_CASE("document highlight: uses in another file are excluded", "[document-highlight]") {
    const std::string memory = "module memory(input logic clk);\nendmodule\n";
    const std::string top = "module memory_top;\n"
                            "    memory u_mem0(.clk(clk));\n"
                            "    memory u_mem1(.clk(clk));\n"
                            "endmodule\n";
    const auto dir = std::filesystem::temp_directory_path();
    const auto memory_path = dir / "lazyverilog_highlight_memory.sv";
    const auto top_path = dir / "lazyverilog_highlight_top.sv";
    std::ofstream(memory_path) << memory;
    std::ofstream(top_path) << top;

    Analyzer analyzer;
    analyzer.set_extra_files({memory_path.string(), top_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string memory_uri = uri_from_path(memory_path);
    analyzer.open(memory_uri, memory);
    analyzer.open(uri_from_path(top_path), top);

    // References to the module span both files; highlight keeps only this one.
    REQUIRE(analyzer.find_references(memory_uri, 0, 7, true).size() == 3);
    const auto result = provide_document_highlight(analyzer, at(memory_uri, 0, 7));
    REQUIRE(result.size() == 1);
    CHECK(text_of(memory, result[0].range) == "memory");

    std::filesystem::remove(memory_path);
    std::filesystem::remove(top_path);
}

TEST_CASE("document highlight: keyword returns nothing", "[document-highlight]") {
    const std::string uri = "file:///tmp/lazyverilog_highlight_keyword.sv";
    Analyzer analyzer;
    analyzer.open(uri, "module m;\n  logic a;\nendmodule\n");
    CHECK(provide_document_highlight(analyzer, at(uri, 0, 2)).empty());
}
