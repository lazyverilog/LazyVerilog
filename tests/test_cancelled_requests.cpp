// A cancel must come from a `$/cancelRequest`, not from a message that merely
// contains those characters.
//
// `observe()` runs on the thread that reads messages, so it rejects with a
// substring test before parsing anything.  The substring matches anywhere,
// though -- including inside the document text a didChange carries -- and the
// scan that follows looked for an `"id"` after `"params"` without ever
// checking the method.  A SystemVerilog buffer holding both that string and a
// quoted `"id"` therefore had an unrelated request's id recorded as cancelled,
// and that request answered with RequestCancelled instead of a result.
#include "cancelled_requests.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("cancelled requests: a real cancel is recorded", "[cancel]") {
    CancelledRequests cancelled;
    cancelled.observe(R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":7}})");
    CHECK(cancelled.take(CancelledRequests::key(7)));
    // Consumed: a request is answered once.
    CHECK(!cancelled.take(CancelledRequests::key(7)));
}

TEST_CASE("cancelled requests: a string id is recorded as a string", "[cancel]") {
    CancelledRequests cancelled;
    cancelled.observe(R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":"7"}})");
    // `1` and `"1"` are different requests in JSON-RPC.
    CHECK(!cancelled.take(CancelledRequests::key(7)));
    CHECK(cancelled.take(CancelledRequests::key(std::string_view("7"))));
}

TEST_CASE("cancelled requests: a document that mentions the method is not a cancel",
          "[cancel]") {
    CancelledRequests cancelled;
    // The text the user is editing happens to contain both the method name and
    // a quoted "id" -- a comment about the protocol, or a UVM string field.
    cancelled.observe(
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":)"
        R"({"uri":"file:///m.sv","version":2},"contentChanges":[{"text":)"
        R"("// see $/cancelRequest\n  string \"id\" = 9;\n"}]}})");
    CHECK(!cancelled.take(CancelledRequests::key(9)));
}

TEST_CASE("cancelled requests: a request whose params mention the method is not a cancel",
          "[cancel]") {
    CancelledRequests cancelled;
    cancelled.observe(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":)"
                      R"({"query":"$/cancelRequest","id":3}})");
    CHECK(!cancelled.take(CancelledRequests::key(3)));
}
