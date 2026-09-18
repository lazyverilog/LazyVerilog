#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

// The hand-built `workspace/executeCommand` replies are serialized by this
// helper and handed to the transport as a finished string, so nothing
// downstream re-escapes them: whatever comes out here is what the client's JSON
// parser has to read.

TEST_CASE("json string: the six named escapes keep their short form", "[json]") {
    CHECK(json_quoted("a\"b") == "\"a\\\"b\"");
    CHECK(json_quoted("a\\b") == "\"a\\\\b\"");
    CHECK(json_quoted("a\bb") == "\"a\\bb\"");
    CHECK(json_quoted("a\fb") == "\"a\\fb\"");
    CHECK(json_quoted("a\nb") == "\"a\\nb\"");
    CHECK(json_quoted("a\rb") == "\"a\\rb\"");
    CHECK(json_quoted("a\tb") == "\"a\\tb\"");
}

TEST_CASE("json string: every other C0 control is escaped as \\u00xx", "[json]") {
    // RFC 8259 section 7: no code point below U+0020 may appear raw.  The six
    // above have short forms and the rest have only this one, so a serializer
    // that handles the six and stops emits a document no conforming parser will
    // read.  A vertical tab in a comment was enough to make a whole-document
    // formatting reply undecodable.
    CHECK(json_quoted("\v") == "\"\\u000b\"");
    CHECK(json_quoted(std::string(1, '\0')) == "\"\\u0000\"");
    CHECK(json_quoted("\x1b") == "\"\\u001b\"");
    CHECK(json_quoted("\x1f") == "\"\\u001f\"");

    for (int c = 0; c < 0x20; ++c) {
        const auto quoted = json_quoted(std::string(1, static_cast<char>(c)));
        INFO("control byte 0x" << std::hex << c);
        CHECK(quoted.find(static_cast<char>(c)) == std::string::npos);
    }
}

TEST_CASE("json string: UTF-8 above ASCII is passed through unchanged", "[json]") {
    // JSON strings carry UTF-8 directly, and `char` is signed here -- a
    // continuation byte is negative, so a signed comparison against 0x20 would
    // send it down the escape path and corrupt text that was already valid.
    const std::string cjk = "\xe6\xa8\xa1\xe5\x9d\x97"; // 模块
    CHECK(json_quoted(cjk) == "\"" + cjk + "\"");

    const std::string emoji = "\xf0\x9f\x92\xa1"; // U+1F4A1
    CHECK(json_quoted(emoji) == "\"" + emoji + "\"");
}

TEST_CASE("json string: append_json_string adds to what is already there", "[json]") {
    std::string out = "{\"k\":";
    append_json_string(out, "v");
    out += "}";
    CHECK(out == "{\"k\":\"v\"}");
}
