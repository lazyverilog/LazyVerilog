// Every request that carries an id must get a response.
//
// JSON-RPC has no way for a client to learn that a request was discarded, so a
// request the server drops stays in the client's pending table for the rest of
// the session -- `vim.lsp.buf_request_sync` blocks until its own timeout, and
// the callback never runs.
//
// The transport converts a request's JSON into a typed message before any of
// our handlers run, and a member that does not fit its field throws out of that
// conversion; the exception is caught one level up, logged, and the message
// dropped.  Measured with `position.character = -5`, five of seven requests
// went unanswered while the two well-formed ones came back normally.
//
// So the cases below are all malformed, in the ways a real client gets wrong:
// a negative position, a null where an object belongs, a string where a number
// belongs, params missing outright.  What is asserted is only that each id
// comes back -- what the server makes of a request that names nothing is its
// business, and an empty result is a fine answer.
//
// This drives the real binary over stdio, because the behaviour under test is
// the transport's, and none of it is reachable from the in-process tests.

#include "cli_process.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using cli_process::run_command;
using cli_process::shell_quote;

namespace {

int checks_run = 0;
int checks_failed = 0;

void expect(bool condition, const std::string& what) {
    ++checks_run;
    if (!condition) {
        ++checks_failed;
        std::cerr << "FAIL: " << what << "\n";
    }
}

std::string frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// A request the client withdraws must come back as an error, not as a result.
///
/// Sent as a burst so that most of them are still queued when the cancels
/// arrive: a cancel that overtakes work already running cannot stop it, which
/// is true of every server (clangd documents the same caveat), so what is
/// pinned here is the case a cancel exists for.
int cancelled_request_errors(const fs::path& server_bin, const fs::path& work) {
    const std::string root_uri = "file://" + (work / "").generic_string();
    const std::string doc_uri  = "file://" + (work / "m.sv").generic_string();

    std::string body = R"(module m;\n)";
    for (int i = 0; i < 400; ++i)
        body += R"(  always_comb begin\n    x = )" + std::to_string(i) + R"(;\n  end\n)";
    body += R"(endmodule\n)";

    const fs::path input = work / "cancel.jsonrpc";
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" + root_uri + R"(","capabilities":{}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
                     R"("uri":")" + doc_uri + R"(","languageId":"systemverilog","version":1,"text":")" +
                     body + R"("}}})");
        // A fresh snapshot per request, so none of them is a cache hit.
        for (int i = 0; i < 8; ++i) {
            out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
                         R"("textDocument":{"uri":")" + doc_uri + R"(","version":)" +
                         std::to_string(i + 2) + R"(},"contentChanges":[{"range":{)"
                         R"("start":{"line":0,"character":0},"end":{"line":0,"character":0}},)"
                         R"("text":"// x\n"}]}})");
            out << frame(R"({"jsonrpc":"2.0","id":)" + std::to_string(300 + i) +
                         R"(,"method":"textDocument/foldingRange","params":{"textDocument":{"uri":")" +
                         doc_uri + R"("}}})");
        }
        for (int i = 2; i < 8; ++i)
            out << frame(R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":)" +
                         std::to_string(300 + i) + "}}");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }

    const auto result = run_command(server_bin, "< " + shell_quote(input));
    fs::remove(input);
    expect(result.exit_code == 0, "the server exits cleanly after a burst of cancels");

    int cancelled = 0;
    for (int i = 0; i < 8; ++i) {
        const std::string id = R"("id":)" + std::to_string(300 + i);
        auto at = result.stdout_text.find(id);
        if (at == std::string::npos)
            continue;
        // -32800 is RequestCancelled.  Look only inside this reply.
        const auto next = result.stdout_text.find("Content-Length", at);
        if (result.stdout_text.substr(at, (next == std::string::npos ? result.stdout_text.size()
                                                                     : next) - at)
                .find("-32800") != std::string::npos)
            ++cancelled;
    }
    return cancelled;
}


/// AutoArg-on-save must not depend on whether the parse has landed.
///
/// Format-on-save arrives from BufWritePre, right behind the didChange that
/// carried the last keystroke, so the snapshot the handler finds is routinely
/// the text-only placeholder `enqueue_parse()` installed.  Testing `state->tree`
/// there made AutoArg run or not run depending on that race, which a user
/// experiences as the feature working intermittently.
///
/// Driven over stdio with the whole session buffered, so the formatting request
/// is dispatched immediately behind the didChange -- the window itself.  The
/// document is padded so the parse cannot finish inside the microseconds the
/// dispatch thread needs to reach the next message; without the padding this
/// would pass on a fast machine whatever the server did.
///
/// Returns whether the reply carries the generated port list.
bool autoarg_on_save_after_edit(const fs::path& server_bin, const fs::path& work) {
    const fs::path project = work / "autoarg";
    fs::create_directories(project);
    {
        std::ofstream toml(project / "lazyverilog.toml");
        toml << "[design]\nvcode = \"lv.f\"\n"
             << "[autoarg]\nautoarg_on_save = true\n"
             << "[format]\nenable_format_on_save = true\n";
        std::ofstream flist(project / "lv.f");
    }

    // A non-ANSI header with an empty port list is what AutoArg fills in.
    std::string body = R"(module aa ();\n  input logic i_clk;\n  output logic o_q;\n)";
    for (int i = 0; i < 4000; ++i)
        body += R"(  always_comb begin\n    x = )" + std::to_string(i) + R"(;\n  end\n)";
    body += R"(endmodule\n)";

    const std::string root_uri = "file://" + (project / "").generic_string();
    const std::string doc_uri  = "file://" + (project / "aa.sv").generic_string();
    {
        std::ofstream sv(project / "aa.sv");
        sv << "module aa ();\n";
    }

    const fs::path input = project / "autoarg.jsonrpc";
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" + root_uri + R"(","capabilities":{}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
                     R"("uri":")" + doc_uri + R"(","languageId":"systemverilog","version":1,"text":")" +
                     body + R"("}}})");
        // The keystroke, then the save.  Nothing between them.
        out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
                     R"("textDocument":{"uri":")" + doc_uri + R"(","version":2},)"
                     R"("contentChanges":[{"range":{"start":{"line":0,"character":0},)"
                     R"("end":{"line":0,"character":0}},"text":"// edit\n"}]}})");
        out << frame(R"({"jsonrpc":"2.0","id":40,"method":"textDocument/formatting","params":{)"
                     R"("textDocument":{"uri":")" + doc_uri + R"("},)"
                     R"("options":{"tabSize":4,"insertSpaces":true}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }

    const auto result = run_command(server_bin, "< " + shell_quote(input));
    expect(result.exit_code == 0, "the server exits cleanly after a format-on-save");

    const auto at = result.stdout_text.find(R"("id":40)");
    if (at == std::string::npos)
        return false;
    // The port *list*, not merely the names: both appear in the body's own
    // `input`/`output` declarations whether AutoArg ran or not, so checking for
    // them alone passes on a reply that only reformatted the file.  What only
    // AutoArg produces is a filled header -- `module aa(` followed by the names
    // -- in place of the empty `module aa();` the source has.
    const auto reply = result.stdout_text.substr(at, 400);
    return !contains(reply, "module aa();") && contains(reply, "i_clk,");
}

struct Case {
    int         id;
    std::string method;
    std::string params;
    std::string what;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <lazyverilog-lsp-binary>\n";
        return 2;
    }

    const fs::path server_bin = argv[1];
    if (!fs::exists(server_bin)) {
        std::cerr << "server binary does not exist: " << server_bin << "\n";
        return 2;
    }

    const fs::path work = fs::temp_directory_path() /
                          ("lazyverilog-request-reply-" +
                           std::to_string(cli_process::current_process_id()));
    fs::create_directories(work);
    {
        std::ofstream toml(work / "lazyverilog.toml");
        toml << "[design]\nvcode = \"lv.f\"\n";
        std::ofstream flist(work / "lv.f");
    }
    const std::string source = "module m;\n  always_comb begin\n    x = 1;\n  end\nendmodule\n";
    {
        std::ofstream sv(work / "m.sv");
        sv << source;
    }

    const std::string root_uri = "file://" + (work / "").generic_string();
    const std::string doc_uri  = "file://" + (work / "m.sv").generic_string();
    const std::string doc      = R"("textDocument":{"uri":")" + doc_uri + R"("})";

    const std::vector<Case> cases = {
        {10, "textDocument/definition", "{" + doc + R"(,"position":{"line":0,"character":-5}})",
         "negative character"},
        {11, "textDocument/hover", "{" + doc + R"(,"position":{"line":-1,"character":-1}})",
         "negative line and character"},
        {12, "textDocument/completion", "{" + doc + R"(,"position":{"line":"x","character":0}})",
         "a string where a number belongs"},
        {13, "textDocument/references",
         "{" + doc + R"(,"position":null,"context":{"includeDeclaration":true}})",
         "a null position"},
        {14, "textDocument/definition", R"({"textDocument":null,"position":{"line":0,"character":0}})",
         "a null textDocument"},
        {15, "textDocument/definition", "{}", "params missing every member"},
        {16, "textDocument/inlayHint",
         "{" + doc +
             R"(,"range":{"start":{"line":-3,"character":-3},"end":{"line":-1,"character":0}}})",
         "a negative range"},
        // Well-formed, and last: a server that answered nothing at all would
        // otherwise pass every case above.
        {17, "textDocument/foldingRange", "{" + doc + "}", "a well-formed request"},
    };

    const fs::path input = work / "session.jsonrpc";
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" +
                     root_uri + R"(","capabilities":{}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
                     R"("uri":")" +
                     doc_uri + R"(","languageId":"systemverilog","version":1,"text":)"
                     R"("module m;\n  always_comb begin\n    x = 1;\n  end\nendmodule\n"}}})");
        for (const auto& c : cases)
            out << frame(R"({"jsonrpc":"2.0","id":)" + std::to_string(c.id) + R"(,"method":")" +
                         c.method + R"(","params":)" + c.params + "}");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }

    const auto result = run_command(server_bin, "< " + shell_quote(input));

    // Checked before the replies.  foldingRange and inlayHint are answered off
    // the dispatch thread and `exit` is the next message behind them, so a
    // shutdown that does not wait for those workers loses a reply on some runs
    // and takes the process down on others -- and the per-reply checks below
    // cannot tell the second case from a server that simply said nothing.
    expect(result.exit_code == 0, "the server exits cleanly after answering every request");

    for (const auto& c : cases)
        expect(contains(result.stdout_text, R"("id":)" + std::to_string(c.id)),
               c.method + " answered " + c.what);

    // A withdrawn request is answered with RequestCancelled rather than run.
    expect(cancelled_request_errors(server_bin, work) > 0,
           "a cancelled request comes back as RequestCancelled");

    // A save that lands while the last keystroke is still parsing still runs
    // AutoArg.
    expect(autoarg_on_save_after_edit(server_bin, work),
           "autoarg_on_save fills the port list for a save behind an edit");

    fs::remove_all(work);

    std::cerr << "request-reply-cli-smoke: " << (checks_run - checks_failed) << "/" << checks_run
              << " checks passed\n";
    return checks_failed == 0 ? 0 : 1;
}
