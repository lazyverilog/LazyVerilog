#include "edit_watermark.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {

std::string did_change(const std::string& uri, int version) {
    return R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
           uri + R"(","version":)" + std::to_string(version) +
           R"(},"contentChanges":[{"text":"x"}]}})";
}

} // namespace

TEST_CASE("edit watermark: an unread edit marks the document superseded", "[watermark]") {
    const std::string uri = "file:///w.sv";
    EditWatermark watermark;

    // Nothing seen at all.
    CHECK(!watermark.superseded(uri));

    // The transport has read a keystroke the request worker has not run yet:
    // anything computed now describes a document the user has moved past.
    watermark.observe(did_change(uri, 2));
    CHECK(watermark.superseded(uri));

    // Once the notification's own handler runs, the counts agree again.
    watermark.on_dispatch(uri);
    CHECK(!watermark.superseded(uri));
}

TEST_CASE("edit watermark: a burst stays superseded until the last edit lands",
          "[watermark]") {
    const std::string uri = "file:///burst.sv";
    EditWatermark watermark;

    for (int v = 2; v <= 6; ++v)
        watermark.observe(did_change(uri, v));

    // Four of the five keystrokes are still in flight after the first is applied.
    for (int applied = 1; applied <= 4; ++applied) {
        watermark.on_dispatch(uri);
        CHECK(watermark.superseded(uri));
    }
    watermark.on_dispatch(uri);
    CHECK(!watermark.superseded(uri));
}

TEST_CASE("edit watermark: each document is counted on its own", "[watermark]") {
    const std::string a = "file:///a.sv";
    const std::string b = "file:///b.sv";
    EditWatermark watermark;

    watermark.observe(did_change(a, 2));
    CHECK(watermark.superseded(a));
    // Editing one buffer must not make another buffer's folds look obsolete.
    CHECK(!watermark.superseded(b));
}

TEST_CASE("edit watermark: only didChange counts", "[watermark]") {
    const std::string uri = "file:///other.sv";
    EditWatermark watermark;

    watermark.observe(
        R"({"jsonrpc":"2.0","id":1,"method":"textDocument/foldingRange","params":{"textDocument":{"uri":")" +
        uri + R"("}}})");
    watermark.observe(
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
        uri + R"(","languageId":"systemverilog","version":1,"text":"module m; endmodule"}}})");
    watermark.observe(
        R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":")" +
        uri + R"("}}})");
    CHECK(!watermark.superseded(uri));
}

TEST_CASE("edit watermark: a document containing the method name is not an edit",
          "[watermark]") {
    const std::string uri = "file:///selfreferential.sv";
    EditWatermark watermark;

    // The substring test in front of the parse is a filter, not the decision:
    // a buffer whose own text mentions the method — editing this server's source,
    // for instance — must not be read as a keystroke.
    watermark.observe(
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
        uri +
        R"(","languageId":"systemverilog","version":1,"text":"// textDocument/didChange"}}})");
    CHECK(!watermark.superseded(uri));
}

TEST_CASE("edit watermark: malformed and unexpected messages are ignored", "[watermark]") {
    const std::string uri = "file:///malformed.sv";
    EditWatermark watermark;

    watermark.observe("");
    watermark.observe("not json at all");
    watermark.observe(R"({"method":"textDocument/didChange")");                    // truncated
    watermark.observe(R"({"method":"textDocument/didChange","params":{}})");       // no textDocument
    watermark.observe(R"({"method":"textDocument/didChange","params":{"textDocument":{}}})");
    watermark.observe(R"({"method":"textDocument/didChange","params":{"textDocument":{"uri":7}}})");
    watermark.observe(R"([1,2,"textDocument/didChange"])");                        // not an object
    CHECK(!watermark.superseded(uri));
}

TEST_CASE("edit watermark: a closed document is forgotten", "[watermark]") {
    const std::string uri = "file:///closed.sv";
    EditWatermark watermark;

    watermark.observe(did_change(uri, 2));
    REQUIRE(watermark.superseded(uri));

    // Closing drops the counters, so reopening the file does not start out
    // looking like it has an edit in flight.
    watermark.forget(uri);
    CHECK(!watermark.superseded(uri));
}

TEST_CASE("edit watermark: a URI is matched exactly as the client spelled it",
          "[watermark]") {
    EditWatermark watermark;

    // LspCpp stores an incoming URI verbatim, so the watermark keys on the same
    // bytes the handlers see.  Percent-encoding differences are different files
    // as far as both are concerned.
    watermark.observe(did_change("file:///dir/a%20b.sv", 2));
    CHECK(watermark.superseded("file:///dir/a%20b.sv"));
    CHECK(!watermark.superseded("file:///dir/a b.sv"));
}
