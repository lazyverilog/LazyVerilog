#include "analyzer.hpp"
#include "features/document_highlight.hpp"
#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

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

namespace {

/// Fastest of @p runs highlighting `leaf` in an open file, with @p closed
/// closed project files that each instantiate it many times.  The minimum, for
/// the reason the connect guard takes it.
double fastest_highlight_ms(int closed, int runs) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lazyverilog_highlight_scaling_" + std::to_string(closed));
    std::filesystem::create_directories(dir);
    std::vector<std::string> paths;
    for (int i = 0; i < closed; ++i) {
        const auto path = dir / ("user_" + std::to_string(i) + ".sv");
        std::string text = "module user_" + std::to_string(i) + ";\n";
        for (int k = 0; k < 40; ++k)
            text += "    leaf u_" + std::to_string(k) + "();\n";
        std::ofstream(path) << text + "endmodule\n";
        paths.push_back(path.string());
    }
    const auto leaf_path = dir / "leaf.sv";
    const std::string leaf = "module leaf;\nendmodule\nmodule wrap;\n  leaf u_a();\nendmodule\n";
    std::ofstream(leaf_path) << leaf;
    paths.push_back(leaf_path.string());

    Analyzer analyzer;
    analyzer.set_extra_files(paths);
    analyzer.wait_for_background_index_idle();
    const std::string uri = uri_from_path(leaf_path);
    analyzer.open(uri, leaf);

    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = provide_document_highlight(analyzer, at(uri, 0, 7));
        samples.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
        REQUIRE(result.size() == 2);
    }
    std::filesystem::remove_all(dir);
    return *std::min_element(samples.begin(), samples.end());
}

} // namespace

TEST_CASE("document highlight: cost does not grow with closed project files",
          "[document-highlight][scaling]") {
    constexpr int kSmall = 25;
    constexpr int kLarge = 400; // 16x the closed files and their uses
    constexpr int kRuns = 7;

    const double small_ms = fastest_highlight_ms(kSmall, kRuns);
    const double large_ms = fastest_highlight_ms(kLarge, kRuns);
    const double ratio = large_ms / small_ms;
    std::cout << "\n[highlight scaling] closed files " << kSmall << " -> " << kLarge << ": "
              << small_ms << " ms -> " << large_ms << " ms  ratio=" << ratio << "\n";

    // Highlight answers for one file, so closed shards should not be scanned:
    // ~1.  Scanning them all is ~16.
    CHECK(ratio < 4.0);
}
