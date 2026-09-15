// Incoming LSP positions on lines that carry non-ASCII text.
//
// `Position.character` is a count of UTF-16 code units, while the document is
// stored as UTF-8, so on any line containing a multi-byte character the two
// disagree.  The outgoing half of that boundary was already covered (see
// "foldingRange: character offsets are UTF-16 code units"); the incoming half
// was not, and three handlers indexed the document with the raw column:
//
//   * hover gated on extract_ident(), which landed (bytes - units) too early
//     and returned nothing from a position inside the identifier;
//   * completion read its prefix from the wrong offset and degraded to an
//     unfiltered keyword list;
//   * prepareRename reported a byte column as the rename range.
//
// Each test below pairs an all-ASCII document with one that is identical except
// for a non-ASCII run earlier on the cursor's line.  The answers must match: a
// comment in Korean is not a semantic change.

#include "analyzer.hpp"
#include "features/completion.hpp"
#include "features/hover.hpp"
#include "features/rename.hpp"
#include "string_utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace {

/// 0-based (line, UTF-16 column) of @p needle -- the position a client would
/// send with the cursor on its first character.
std::pair<int, int> lsp_pos_of(std::string_view text, std::string_view needle) {
    const auto off = text.find(needle);
    REQUIRE(off != std::string_view::npos);
    int line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < off; ++i) {
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    return {line, static_cast<int>(utf16_length(text.substr(line_start, off - line_start)))};
}

std::pair<int, int> lsp_pos_after(std::string_view text, std::string_view needle) {
    auto [line, col] = lsp_pos_of(text, needle);
    return {line, col + static_cast<int>(utf16_length(needle))};
}

std::optional<TextDocumentHover::Result> hover_at(const Analyzer& analyzer, const std::string& uri,
                                                  int line, int col) {
    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);
    return provide_hover(analyzer, params);
}

bool has_label(const CompletionList& list, std::string_view label) {
    return std::any_of(list.items.begin(), list.items.end(),
                       [&](const lsCompletionItem& it) { return it.label == label; });
}

// A Korean string literal: two characters, 6 bytes, 2 UTF-16 units.  Chosen
// over a comment so the run sits in code the parser keeps.
constexpr const char* kAsciiPad = R"(  string s = "ab";)";
constexpr const char* kWidePad = R"(  string s = "한글";)";

} // namespace

TEST_CASE("hover: answers on every column of an identifier after non-ASCII text",
          "[position_encoding][hover]") {
    // Byte columns run 4 ahead of UTF-16 columns after the Korean literal, so a
    // handler that treats the incoming column as a byte index looks at a
    // position earlier in the line.  Every column of the identifier must answer,
    // exactly as it does without the literal.
    const auto probe = [](const char* pad, const std::string& uri) {
        Analyzer analyzer;
        const std::string text =
            std::string("module m;\n") + pad + " logic my_signal;\nendmodule\n";
        analyzer.open(uri, text);

        const auto [line, start] = lsp_pos_of(text, "my_signal");
        const int width = static_cast<int>(utf16_length("my_signal"));

        std::vector<int> answered;
        for (int col = start; col < start + width; ++col) {
            if (auto hover = hover_at(analyzer, uri, line, col);
                hover && hover->contents.second.has_value() &&
                hover->contents.second->value.find("**my_signal**") != std::string::npos)
                answered.push_back(col - start);
        }
        return answered;
    };

    const auto ascii = probe(kAsciiPad, "file:///tmp/lvpos_hover_ascii.sv");
    const auto wide = probe(kWidePad, "file:///tmp/lvpos_hover_wide.sv");

    REQUIRE(ascii.size() == static_cast<size_t>(utf16_length("my_signal")));
    CHECK(wide == ascii);
}

TEST_CASE("completion: filters by prefix after non-ASCII text on the same line",
          "[position_encoding][completion]") {
    // Reading the prefix from a byte offset returned the wrong word, so the
    // context fell back to "no prefix" and every keyword came back.
    const auto probe = [](const char* pad, const std::string& uri) {
        Analyzer analyzer;
        CompletionEngine engine;
        const std::string text = std::string("module m;\n  logic my_signal;\n") + pad +
                                 " assign x = my_si\nendmodule\n";
        analyzer.open(uri, text);

        const auto [line, col] = lsp_pos_after(text, "my_si");
        lsTextDocumentPositionParams params;
        params.textDocument.uri.raw_uri_ = uri;
        params.position = lsPosition(line, col);
        CancellationToken tok;
        auto state = analyzer.get_state(uri);
        REQUIRE(state != nullptr);
        return engine.complete(params, *state, analyzer, tok);
    };

    const auto ascii = probe(kAsciiPad, "file:///tmp/lvpos_comp_ascii.sv");
    const auto wide = probe(kWidePad, "file:///tmp/lvpos_comp_wide.sv");

    REQUIRE(has_label(ascii, "my_signal"));
    CHECK(has_label(wide, "my_signal"));
    // The real symptom was the unfiltered fallback: a keyword dump many times
    // the size of the prefix-filtered list.
    CHECK(wide.items.size() == ascii.items.size());
}

TEST_CASE("prepareRename: reports the rename range in UTF-16 columns",
          "[position_encoding][rename]") {
    // The range goes straight back to the client, which resolves it against its
    // own UTF-16 view of the line.  A byte column here overwrites the wrong text.
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lvpos_rename_wide.sv";
    const std::string text =
        std::string("module m;\n") + kWidePad + " logic my_signal;\nendmodule\n";
    analyzer.open(uri, text);

    const auto [line, start] = lsp_pos_of(text, "my_signal");

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, start + 2);

    auto result = prepare_rename(analyzer, params);
    REQUIRE(result.has_value());
    CHECK(result->placeholder == "my_signal");
    CHECK(result->range.start.line == line);
    CHECK(result->range.start.character == start);
    CHECK(result->range.end.character == start + static_cast<int>(utf16_length("my_signal")));
}

TEST_CASE("a non-BMP character counts as two columns on the way in",
          "[position_encoding][hover]") {
    // A surrogate pair is where a UTF-16 count parts company with a character
    // count as well as with a byte count: U+1F680 is 4 bytes and 2 units.
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lvpos_astral.sv";
    const std::string text = "module m;\n  string s = \"\xF0\x9F\x9A\x80\"; logic my_signal;\nendmodule\n";
    analyzer.open(uri, text);

    const auto [line, start] = lsp_pos_of(text, "my_signal");

    // Spelled out rather than derived, so the test pins the convention itself:
    // `  string s = "` is 14 units, the rocket is 2, then `"; logic ` is 9.
    CHECK(start == 25);

    auto hover = hover_at(analyzer, uri, line, start);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value.find("**my_signal**") != std::string::npos);
}

TEST_CASE("lsp_position_to_byte_offset converts, and clamps past the end",
          "[position_encoding]") {
    // Split after the escapes: a hex escape is greedy, so "\x80cd" would be read
    // as one out-of-range value rather than a byte followed by "cd".
    const std::string text = "ab\n" "\xED\x95\x9C\xEA\xB8\x80" "cd\nef\n";

    // Line 1 is "한글cd": 2 UTF-16 units of Korean, then ASCII.
    CHECK(lsp_position_to_byte_offset(text, 1, 0) == 3);
    CHECK(lsp_position_to_byte_offset(text, 1, 2) == 9);  // past both Korean chars
    CHECK(lsp_position_to_byte_offset(text, 1, 3) == 10); // past 'c'

    // A column past the end of the line stops at the newline rather than
    // running into the next line.
    CHECK(lsp_position_to_byte_offset(text, 1, 99) == 11);

    // A line past the end of the document clamps to the end.
    CHECK(lsp_position_to_byte_offset(text, 99, 0) == text.size());

    // Round trip: the column conversion is the inverse of the length count.
    CHECK(lsp_column_from_byte_offset(text, 3, 9) == 2);
    CHECK(lsp_column_from_byte_offset(text, 3, 3) == 0);
}
