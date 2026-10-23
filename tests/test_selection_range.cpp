#include "analyzer.hpp"
#include "features/folding_range.hpp"
#include "features/selection_range.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace {

bool before_or_equal(const lsPosition& a, const lsPosition& b) {
    return a.line < b.line || (a.line == b.line && a.character <= b.character);
}

bool contains(const lsRange& outer, const lsRange& inner) {
    return before_or_equal(outer.start, inner.start) && before_or_equal(inner.end, outer.end);
}

bool same(const lsRange& a, const lsRange& b) {
    return a.start.line == b.start.line && a.start.character == b.start.character &&
           a.end.line == b.end.line && a.end.character == b.end.character;
}

std::vector<lsRange> chain(const SelectionRange& range) {
    std::vector<lsRange> out;
    for (const SelectionRange* r = &range; r; r = r->parent ? *r->parent : nullptr)
        out.push_back(r->range);
    return out;
}

void check_strictly_growing(const std::vector<lsRange>& ranges) {
    for (size_t i = 1; i < ranges.size(); ++i) {
        CHECK(contains(ranges[i], ranges[i - 1]));
        CHECK(!same(ranges[i], ranges[i - 1]));
    }
}

// ASCII-only slice; enough for these fixtures.
std::string text_of(const std::string& text, const lsRange& range) {
    size_t start = 0;
    for (int line = 0; line < range.start.line; ++line)
        start = text.find('\n', start) + 1;
    size_t end = start;
    for (int line = range.start.line; line < range.end.line; ++line)
        end = text.find('\n', end) + 1;
    return text.substr(start + range.start.character,
                       end + range.end.character - start - range.start.character);
}

SelectionRangeParams request(const std::string& uri, std::vector<lsPosition> positions) {
    SelectionRangeParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.positions = std::move(positions);
    return params;
}

lsRange range(int sl, int sc, int el, int ec) {
    lsRange r;
    r.start = lsPosition(sl, sc);
    r.end = lsPosition(el, ec);
    return r;
}

} // namespace

TEST_CASE("selection range: chain grows strictly", "[selection-range]") {
    const std::string uri = "file:///tmp/lazyverilog_selection_chain.sv";
    const std::string text = "module m;\n"
                             "  logic a, b, c;\n"
                             "  assign c = a + b;\n"
                             "endmodule\n";
    Analyzer analyzer;
    analyzer.open(uri, text);

    const auto answer = provide_selection_range(analyzer, request(uri, {lsPosition(2, 13)}));
    REQUIRE(answer.result.size() == 1);
    const auto ranges = chain(answer.result[0]);
    REQUIRE(ranges.size() >= 3);
    CHECK(text_of(text, ranges.front()) == "a");
    CHECK(std::any_of(ranges.begin(), ranges.end(),
                      [&](const lsRange& r) { return text_of(text, r) == "a + b"; }));
    CHECK(contains(ranges.back(), range(0, 0, 3, 9)));
    check_strictly_growing(ranges);
}

TEST_CASE("selection range: one result per position", "[selection-range]") {
    const std::string uri = "file:///tmp/lazyverilog_selection_many.sv";
    Analyzer analyzer;
    analyzer.open(uri, "module m;\n  logic a, b;\nendmodule\n");
    const auto answer = provide_selection_range(
        analyzer, request(uri, {lsPosition(0, 7), lsPosition(1, 8), lsPosition(1, 11)}));
    CHECK(answer.result.size() == 3);
}

TEST_CASE("selection range: position past end of file", "[selection-range]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_selection_eof.sv";
    analyzer.open(uri, "module m;\nendmodule\n");
    auto answer = provide_selection_range(analyzer, request(uri, {lsPosition(999, 4)}));
    REQUIRE(answer.result.size() == 1);
    CHECK(same(answer.result[0].range, range(2, 0, 2, 0)));

    const std::string empty_uri = "file:///tmp/lazyverilog_selection_empty.sv";
    analyzer.open(empty_uri, "");
    answer = provide_selection_range(analyzer, request(empty_uri, {lsPosition(0, 0)}));
    REQUIRE(answer.result.size() == 1);
    CHECK(same(answer.result[0].range, range(0, 0, 0, 0)));
}

TEST_CASE("selection range: without tree uses word then folds", "[selection-range]") {
    const std::string uri = "file:///tmp/lazyverilog_selection_folds.sv";
    const std::string text = "module m;\n"
                             "  always_comb begin\n"
                             "    if (x) begin\n"
                             "      y = 1;\n"
                             "    end\n"
                             "  end\n"
                             "endmodule\n";
    Analyzer analyzer;
    analyzer.open(uri, text);
    FoldingRangeRequestParams fold_params;
    fold_params.textDocument.uri.raw_uri_ = uri;
    const auto folds = provide_folding_range(analyzer, fold_params);

    const auto answer = selection_range_without_tree(text, folds, lsPosition(3, 6));
    REQUIRE(answer.result.size() == 1);
    const auto ranges = chain(answer.result[0]);
    REQUIRE(ranges.size() >= 4);
    CHECK(text_of(text, ranges.front()) == "y");
    CHECK(same(ranges.back(), range(0, 0, 7, 0)));
    check_strictly_growing(ranges);
}

TEST_CASE("selection range: utf-16 columns", "[selection-range]") {
    const std::string uri = "file:///tmp/lazyverilog_selection_utf16.sv";
    // The emoji is 4 bytes but 2 UTF-16 units, so `a` sits at UTF-16 column 15.
    const std::string text = "module m;\n"
                             "  logic b;\n"
                             "  logic /*\xF0\x9F\x98\x80*/ a;\n"
                             "endmodule\n";
    Analyzer analyzer;
    analyzer.open(uri, text);
    const auto answer = provide_selection_range(analyzer, request(uri, {lsPosition(2, 15)}));
    REQUIRE(answer.result.size() == 1);
    CHECK(same(answer.result[0].range, range(2, 15, 2, 16)));
}
