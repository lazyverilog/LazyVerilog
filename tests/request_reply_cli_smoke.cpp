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
#include <iterator>
#include <regex>
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
/// Format-on-save arrives as willSaveWaitUntil, right behind the didChange that
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
        out << frame(R"({"jsonrpc":"2.0","id":40,"method":"textDocument/willSaveWaitUntil",)"
                     R"("params":{"textDocument":{"uri":")" + doc_uri + R"("},"reason":1}})");
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

/// The text of the reply to request `id`, up to the next message.
std::string reply_for(const std::string& out, int id) {
    const auto at = out.find(R"("id":)" + std::to_string(id));
    if (at == std::string::npos)
        return {};
    const auto next = out.find("Content-Length", at);
    return out.substr(at, next == std::string::npos ? std::string::npos : next - at);
}

struct SaveReplies {
    bool advertised       = false; // initialize says willSaveWaitUntil
    bool formatting_edits = false; // textDocument/formatting returned an edit
    bool save_edits       = false; // textDocument/willSaveWaitUntil returned an edit
};

/// Format Document and save are separate paths: textDocument/formatting always
/// formats, and willSaveWaitUntil formats only when enable_format_on_save is
/// set.  Formatting used to double as the save hook, so with the option off a
/// manual "Format Document" silently did nothing.
SaveReplies save_and_format(const fs::path& server_bin, const fs::path& work, bool on_save) {
    const fs::path project = work / (on_save ? "save_on" : "save_off");
    fs::create_directories(project);
    {
        std::ofstream toml(project / "lazyverilog.toml");
        toml << "[design]\nvcode = \"lv.f\"\n"
             << "[format]\nenable_format_on_save = " << (on_save ? "true" : "false") << "\n";
        std::ofstream flist(project / "lv.f");
    }
    const std::string root_uri = "file://" + (project / "").generic_string();
    const std::string doc_uri  = "file://" + (project / "f.sv").generic_string();
    const fs::path input = project / "save.jsonrpc";
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" + root_uri + R"(","capabilities":{}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        // Unindented body: any formatting pass rewrites it.
        out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
                     R"("uri":")" + doc_uri + R"(","languageId":"systemverilog","version":1,)"
                     R"("text":"module f;\nlogic x;\nendmodule\n"}}})");
        out << frame(R"({"jsonrpc":"2.0","id":60,"method":"textDocument/formatting","params":{)"
                     R"("textDocument":{"uri":")" + doc_uri + R"("},)"
                     R"("options":{"tabSize":4,"insertSpaces":true}}})");
        out << frame(R"({"jsonrpc":"2.0","id":61,"method":"textDocument/willSaveWaitUntil",)"
                     R"("params":{"textDocument":{"uri":")" + doc_uri + R"("},"reason":1}})");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }
    const auto result = run_command(server_bin, "< " + shell_quote(input));
    expect(result.exit_code == 0, "the server exits cleanly after formatting and a save");
    const auto fmt  = reply_for(result.stdout_text, 60);
    const auto save = reply_for(result.stdout_text, 61);
    expect(!save.empty(), "willSaveWaitUntil is answered");
    return {contains(result.stdout_text, R"("willSaveWaitUntil":true)"), contains(fmt, "newText"),
            contains(save, "newText")};
}

/// A didChange for a document that was never opened must not open it.
///
/// The handler applied the change to "" and parsed the result, which installed
/// a buffer of whatever the change happened to carry: it shadowed the file's
/// shard until the file was really opened.  documentSymbol answers from that
/// buffer, and the file is not on disk, so a module named in the reply is the ghost.
///
/// Returns whether the documentSymbol reply is free of the ghost module.
bool change_without_open_is_ignored(const fs::path& server_bin, const fs::path& work) {
    const std::string root_uri  = "file://" + (work / "").generic_string();
    const std::string ghost_uri = "file://" + (work / "ghost.sv").generic_string();

    const fs::path input = work / "ghost.jsonrpc";
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" + root_uri + R"(","capabilities":{}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
                     R"("textDocument":{"uri":")" + ghost_uri + R"(","version":2},)"
                     R"("contentChanges":[{"text":"module   ghost_buffer;endmodule\n"}]}})");
        // Formatting first: it waits for the parse in flight, so the
        // documentSymbol behind it sees a ghost if one was made.
        out << frame(R"({"jsonrpc":"2.0","id":49,"method":"textDocument/formatting","params":{)"
                     R"("textDocument":{"uri":")" + ghost_uri + R"("},)"
                     R"("options":{"tabSize":4,"insertSpaces":true}}})");
        out << frame(R"({"jsonrpc":"2.0","id":50,"method":"textDocument/documentSymbol","params":{)"
                     R"("textDocument":{"uri":")" + ghost_uri + R"("}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }

    const auto result = run_command(server_bin, "< " + shell_quote(input));
    fs::remove(input);
    expect(result.exit_code == 0, "the server exits cleanly after a change to an unopened file");
    const auto at = result.stdout_text.find(R"("id":50)");
    expect(at != std::string::npos, "documentSymbol for an unopened file is answered");
    if (at == std::string::npos)
        return false;
    const auto next = result.stdout_text.find("Content-Length", at);
    return !contains(result.stdout_text.substr(at, next == std::string::npos ? std::string::npos
                                                                             : next - at),
                     "ghost_buffer");
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
        // Methods the server has no handler for: MethodNotFound, checked below.
        {20, "textDocument/declaration", "{" + doc + R"(,"position":{"line":0,"character":0}})",
         "an unsupported method"},
        {21, "textDocument/semanticTokens/full", "{" + doc + "}", "an unsupported method"},
        {22, "lazyverilog/noSuchMethod", "{}", "an unknown method"},
        {23, "$/noSuchRequest", "{}", "an unknown $/ request"},
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
        // An unknown `$/` notification is ignored, and must not get a reply.
        out << frame(R"({"jsonrpc":"2.0","method":"$/noSuchNotification","params":{}})");
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
    // -32601 is MethodNotFound.
    for (int id : {20, 21, 22, 23})
        expect(contains(reply_for(result.stdout_text, id), "-32601"),
               "request " + std::to_string(id) + " for an unhandled method is MethodNotFound");
    // A reply to the unknown notification would carry no method name, only
    // "id":null, so count responses instead: one per request id sent, and none
    // with a null id.  Server-to-client requests carry "method", not
    // "result"/"error", after the id.
    {
        const std::regex response(R"re("id":[^,{}]*,"(result|error)")re");
        const auto replies = std::distance(
            std::sregex_iterator(result.stdout_text.begin(), result.stdout_text.end(), response),
            std::sregex_iterator());
        expect(replies == static_cast<long>(cases.size()) + 1,
               "exactly one response per request (initialize + cases), got " +
                   std::to_string(replies));
        expect(!contains(result.stdout_text, R"("id":null)"),
               "an unknown $/ notification gets no reply");
    }

    // A withdrawn request is answered with RequestCancelled rather than run.
    expect(cancelled_request_errors(server_bin, work) > 0,
           "a cancelled request comes back as RequestCancelled");

    // A save that lands while the last keystroke is still parsing still runs
    // AutoArg.
    expect(autoarg_on_save_after_edit(server_bin, work),
           "autoarg_on_save fills the port list for a save behind an edit");

    const auto off = save_and_format(server_bin, work, false);
    expect(off.advertised, "initialize advertises willSaveWaitUntil");
    expect(off.formatting_edits,
           "textDocument/formatting formats with enable_format_on_save off");
    expect(!off.save_edits, "willSaveWaitUntil leaves the file alone with the option off");
    const auto on = save_and_format(server_bin, work, true);
    expect(on.formatting_edits, "textDocument/formatting formats with enable_format_on_save on");
    expect(on.save_edits, "willSaveWaitUntil formats with enable_format_on_save on");

    expect(change_without_open_is_ignored(server_bin, work),
           "a didChange for an unopened file does not create a buffer");

    fs::remove_all(work);

    std::cerr << "request-reply-cli-smoke: " << (checks_run - checks_failed) << "/" << checks_run
              << " checks passed\n";
    return checks_failed == 0 ? 0 : 1;
}
