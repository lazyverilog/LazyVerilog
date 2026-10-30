#include "analyzer.hpp"
#include "features/document_highlight.hpp"
#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <vector>

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

/// Fastest of @p runs samples of @p small and of @p large, alternated, as
/// large / small.  Interleaved, a runner that slows part way through hits both
/// sides; the minimum, because everything a shared runner adds only makes a
/// sample slower.  Every sample is printed: a failure is usually about the
/// spread, and one number cannot show it.
double interleaved_ratio(const char* label, int runs, const std::function<double()>& small,
                         const std::function<double()>& large) {
    (void)small(); // warm both before the first measured round
    (void)large();
    std::vector<double> small_samples;
    std::vector<double> large_samples;
    for (int round = 0; round < runs; ++round) {
        small_samples.push_back(small());
        large_samples.push_back(large());
    }
    const double small_ms = *std::min_element(small_samples.begin(), small_samples.end());
    const double large_ms = *std::min_element(large_samples.begin(), large_samples.end());
    const double ratio = large_ms / small_ms;
    std::cout << "\n[" << label << "] " << small_ms << " ms -> " << large_ms
              << " ms  ratio=" << ratio << "\n  small:";
    for (double sample : small_samples)
        std::cout << " " << sample;
    std::cout << "\n  large:";
    for (double sample : large_samples)
        std::cout << " " << sample;
    std::cout << "\n";
    return ratio;
}

/// Milliseconds for @p calls highlights at @p params, each finding @p expected.
double highlight_ms(Analyzer& analyzer, const lsTextDocumentPositionParams& params, int calls,
                    size_t expected) {
    const auto start = std::chrono::steady_clock::now();
    size_t found = 0;
    for (int k = 0; k < calls; ++k)
        found += provide_document_highlight(analyzer, params).size();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(found == expected * calls);
    return ms;
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

/// An open file declaring `leaf`, and @p closed closed project files that
/// each instantiate it many times.
struct ClosedUsers {
    std::filesystem::path dir;
    std::string uri;
    Analyzer analyzer;

    explicit ClosedUsers(int closed)
        : dir(std::filesystem::temp_directory_path() /
              ("lazyverilog_highlight_scaling_" + std::to_string(closed))) {
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

        analyzer.set_extra_files(paths);
        analyzer.wait_for_background_index_idle();
        uri = uri_from_path(leaf_path);
        analyzer.open(uri, leaf);
    }
    ~ClosedUsers() { std::filesystem::remove_all(dir); }
};

} // namespace

TEST_CASE("document highlight: cost does not grow with closed project files",
          "[document-highlight][scaling]") {
    constexpr int kSmall = 25;
    constexpr int kLarge = 400; // 16x the closed files and their uses
    // One highlight is ~0.04 ms, below what one sample should be; 20 per
    // sample (~1 ms, short enough to fit between preemptions on a loaded
    // machine) and 60 alternated rounds, so the minimum is a clean sample on
    // both sides.
    constexpr int kCalls = 20;
    constexpr int kRuns = 60;

    ClosedUsers small(kSmall);
    ClosedUsers large(kLarge);
    const double ratio = interleaved_ratio(
        "highlight scaling", kRuns,
        [&] { return highlight_ms(small.analyzer, at(small.uri, 0, 7), kCalls, 2); },
        [&] { return highlight_ms(large.analyzer, at(large.uri, 0, 7), kCalls, 2); });

    // Highlight answers for one file, so closed shards should not be scanned:
    // ~1.  Scanning them all is ~16.
    CHECK(ratio < 4.0);
}

namespace {

/// @p classes register classes, each embedding a covergroup whose sample() it
/// calls and declaring a sample() of its own -- the shape a generated UVM
/// register model repeats a hundred times in one file.
std::string register_classes(int classes) {
    std::string text;
    for (int i = 0; i < classes; ++i) {
        text += "class reg_" + std::to_string(i) + ";\n";
        text += "  covergroup rd_cg with function sample(int data);\n";
        text += "  endgroup\n";
        text += "  function new();\n";
        text += "    rd_cg = new();\n";
        text += "  endfunction\n";
        text += "  virtual function void sample(int data);\n";
        text += "    rd_cg.sample(data);\n";
        text += "  endfunction\n";
        text += "endclass\n";
    }
    return text;
}

constexpr int kRegisterClassLines = 10;

} // namespace

TEST_CASE("document highlight: an embedded covergroup's sample() stays in its class",
          "[document-highlight][covergroup]") {
    const std::string uri = "file:///tmp/lazyverilog_highlight_covergroup.sv";
    const std::string text = register_classes(3);
    Analyzer analyzer;
    analyzer.open(uri, text);

    // `rd_cg.sample` in the second class: its covergroup's sample() and the call.
    const int call_line = kRegisterClassLines + 7;
    const auto result = provide_document_highlight(analyzer, at(uri, call_line, 10));
    std::vector<int> lines;
    for (const auto& highlight : result) {
        CHECK(text_of(text, highlight.range) == "sample");
        lines.push_back(highlight.range.start.line);
    }
    std::sort(lines.begin(), lines.end());
    CHECK(lines == std::vector<int>{kRegisterClassLines + 1, call_line});
}

namespace {

/// A file of @p classes register classes.  Every class adds same-named
/// candidates a highlight of the first class's `rd_cg.sample` has to rule out.
struct RegisterFile {
    const std::string uri = "file:///tmp/lazyverilog_highlight_member_scaling.sv";
    Analyzer analyzer;

    explicit RegisterFile(int classes) { analyzer.open(uri, register_classes(classes)); }
};

} // namespace

TEST_CASE("document highlight: cost grows linearly with same-named candidates",
          "[document-highlight][scaling]") {
    // 4x the classes, the file and the candidates.  One small sample is four
    // highlights against one on the large file, so both take about as long
    // and linear is ~1: timed one against one (linear ~4), the longer large
    // sample was the likelier one to catch a stall.
    constexpr int kSmall = 25;
    constexpr int kFactor = 4;
    constexpr int kRuns = 15;

    RegisterFile small(kSmall);
    RegisterFile large(kSmall * kFactor);
    const double ratio = interleaved_ratio(
        "highlight member scaling", kRuns,
        [&] { return highlight_ms(small.analyzer, at(small.uri, 7, 10), kFactor, 2); },
        [&] { return highlight_ms(large.analyzer, at(large.uri, 7, 10), 1, 2); });

    // Resolving each candidate with its own walk of the file is ~4 (16 before
    // the per-sample factor).  2 is the 8.0 this limit was over one-to-one
    // samples, divided by that factor.
    CHECK(ratio < 2.0);
}


namespace {

/// An open file declaring `base_c::m`, and @p closed closed project files
/// that each declare a subclass overriding it -- the shape of uvm_component's
/// subtree under build_phase.
struct ClosedOverrides {
    std::filesystem::path dir;
    std::string uri;
    Analyzer analyzer;

    explicit ClosedOverrides(int closed)
        : dir(std::filesystem::temp_directory_path() /
              ("lazyverilog_highlight_override_" + std::to_string(closed))) {
        std::filesystem::create_directories(dir);
        std::vector<std::string> paths;
        for (int i = 0; i < closed; ++i) {
            const auto path = dir / ("sub_" + std::to_string(i) + ".sv");
            std::ofstream(path) << "class sub_" + std::to_string(i) +
                                       " extends base_c;\n"
                                       "  virtual function void m();\n"
                                       "    super.m();\n"
                                       "  endfunction\n"
                                       "endclass\n";
            paths.push_back(path.string());
        }
        const auto base_path = dir / "base.sv";
        const std::string base = "class base_c;\n"
                                 "  virtual function void m();\n"
                                 "  endfunction\n"
                                 "endclass\n"
                                 "class near_c extends base_c;\n"
                                 "  virtual function void m();\n"
                                 "    super.m();\n"
                                 "  endfunction\n"
                                 "endclass\n";
        std::ofstream(base_path) << base;
        paths.push_back(base_path.string());

        analyzer.set_extra_files(paths);
        analyzer.wait_for_background_index_idle();
        uri = uri_from_path(base_path);
        analyzer.open(uri, base);
    }
    ~ClosedOverrides() { std::filesystem::remove_all(dir); }
};

} // namespace

TEST_CASE("document highlight: a method's cost does not grow with its override subtree",
          "[document-highlight][scaling]") {
    constexpr int kSmall = 25;
    constexpr int kLarge = 400; // 16x the closed overrides
    // One highlight is ~0.08 ms; 20 per sample (~1.6 ms), 30 alternated rounds.
    constexpr int kCalls = 20;
    constexpr int kRuns = 30;

    ClosedOverrides small(kSmall);
    ClosedOverrides large(kLarge);
    const double ratio = interleaved_ratio(
        "highlight override scaling", kRuns,
        [&] { return highlight_ms(small.analyzer, at(small.uri, 1, 24), kCalls, 3); },
        [&] { return highlight_ms(large.analyzer, at(large.uri, 1, 24), kCalls, 3); });

    // Highlight walks up from this file's occurrences: ~1.  Walking the root's
    // whole subtree on every cursor move is ~16.
    CHECK(ratio < 4.0);
}
