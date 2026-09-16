// The server's `initialize` reply must be built from the lazyverilog.toml in
// the workspace root -- and from no other one.
//
// Capabilities are exchanged once and never revised, so a config found after
// the reply cannot take a feature back off: the client goes on requesting it
// for the whole session and the server answers every request with nothing.
// `[inlay_hint].enable` is the case that actually reaches the wire --
// `caps.inlayHintProvider` is built from it -- so it is what this test reads.
//
// `<root>/lazyverilog.toml` is the whole contract: the server does not search
// for the file, at initialize or at didOpen.  A project that puts it elsewhere
// is configured wrong and gets built-in defaults, which is what the third case
// below pins -- an upward walk would make it read the config two directories up
// and answer `false`.
//
// This drives the real binary over stdio rather than calling into the server,
// because the behaviour under test is the content of an LSP reply.

#include "cli_process.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

/// Send `initialize` for @p root_uri, then `exit`, and return the server's
/// stdout.  Writing both messages up front and letting the server read to EOF
/// keeps this to one blocking call with no timing assumptions.
std::string initialize_with_root(const fs::path& server_bin, const std::string& root_uri,
                                 bool dynamic_registration = false) {
    static int counter = 0;
    const fs::path input = fs::temp_directory_path() /
                           ("lazyverilog-config-root-" +
                            std::to_string(cli_process::current_process_id()) + "-" +
                            std::to_string(counter++) + ".jsonrpc");
    const std::string caps =
        dynamic_registration
            ? R"({"textDocument":{"inlayHint":{"dynamicRegistration":true},)"
              R"("foldingRange":{"dynamicRegistration":true}}})"
            : R"({"textDocument":{"inlayHint":{}}})";
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" +
                     root_uri + R"(","capabilities":)" + caps + "}}");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }

    const auto result = run_command(server_bin, "< " + shell_quote(input));
    fs::remove(input);
    expect(result.exit_code == 0, "the server exits cleanly after an initialize");
    return result.stdout_text;
}

/// Send `initialize`, open a foldable buffer, ask for its folds, then `exit`.
/// Returns the server's stdout.
///
/// `[folding].enable` has to reach the request handler and not only the
/// capability reply: Neovim answers `foldingRange.dynamicRegistration = false`,
/// so a config reload cannot unregister the capability and the client keeps
/// asking for the rest of the session.  Computing whole-file folds nobody wants
/// is the most expensive thing on the edit path, so "turned off" has to mean
/// the handler declines too.
std::string folds_for_uri(const fs::path& server_bin, const std::string& root_uri,
                          const std::string& doc_uri) {
    static int counter = 0;
    const fs::path input = fs::temp_directory_path() /
                           ("lazyverilog-config-folds-" +
                            std::to_string(cli_process::current_process_id()) + "-" +
                            std::to_string(counter++) + ".jsonrpc");
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" + root_uri +
                     R"(","capabilities":{"textDocument":{"foldingRange":{}}}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        out << frame(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
                     R"("uri":")" + doc_uri +
                     R"(","languageId":"systemverilog","version":1,"text":)"
                     R"("module m;\n  always_comb begin\n    x = 1;\n  end\nendmodule\n"}}})");
        out << frame(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/foldingRange",)"
                     R"("params":{"textDocument":{"uri":")" + doc_uri + R"("}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }
    const auto result = run_command(server_bin, "< " + shell_quote(input));
    fs::remove(input);
    // `exit` arrives right behind the fold request, so this also pins that the
    // deferred answer and the shutdown do not race: a server that tears itself
    // down under a worker still holding the reply dies here instead of merely
    // going quiet, which is how it stayed invisible on one runner while the
    // stdout checks below passed anyway.
    expect(result.exit_code == 0, "the server exits cleanly after a deferred fold request");
    return result.stdout_text;
}

std::string folds_for_root(const fs::path& server_bin, const std::string& root_uri) {
    return folds_for_uri(server_bin, root_uri, root_uri + "/fold_probe.sv");
}

/// Like folds_for_root(), but the buffer sits two directories below the root,
/// where there is no lazyverilog.toml of its own.
std::string folds_for_nested_file(const fs::path& server_bin, const std::string& root_uri) {
    return folds_for_uri(server_bin, root_uri, root_uri + "/rtl/core/fold_probe.sv");
}

/// Open two buffers from two different projects in ONE session and format both.
/// Returns the server's stdout.
///
/// No rootUri is sent, so nothing but each file's own path says which project it
/// belongs to.  A session-wide formatter config cannot answer both correctly,
/// which is the point.
std::string format_two_projects(const fs::path& server_bin, const std::string& uri_a,
                                const std::string& uri_b) {
    static int counter = 0;
    const fs::path input = fs::temp_directory_path() /
                           ("lazyverilog-config-format-" +
                            std::to_string(cli_process::current_process_id()) + "-" +
                            std::to_string(counter++) + ".jsonrpc");
    // Indented one level inside the module, so indent_size is what decides the
    // leading whitespace of the middle line.
    const std::string text = R"(module m;\nlogic x;\nendmodule\n)";
    const std::string options =
        R"({"tabSize":4,"insertSpaces":true})";
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"capabilities":{"textDocument":{}}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        for (const auto& uri : {uri_a, uri_b}) {
            out << frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
                R"("uri":")" + uri +
                R"(","languageId":"systemverilog","version":1,"text":")" + text + R"("}}})");
        }
        out << frame(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/formatting",)"
                     R"("params":{"textDocument":{"uri":")" + uri_a +
                     R"("},"options":)" + options + R"(}})");
        out << frame(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/formatting",)"
                     R"("params":{"textDocument":{"uri":")" + uri_b +
                     R"("},"options":)" + options + R"(}})");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }
    const auto result = run_command(server_bin, "< " + shell_quote(input));
    fs::remove(input);
    expect(result.exit_code == 0, "the server exits cleanly after two formatting requests");
    return result.stdout_text;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string path_to_uri(const fs::path& p) {
    // The fixtures live under the repository, so a plain file:// prefix is
    // enough here; no escaping case arises.
    return "file://" + p.generic_string();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <lazyverilog-lsp-binary> <repo-root>\n";
        return 2;
    }

    const fs::path server_bin = argv[1];
    // A file:// URI has to carry an absolute path, and CTest passes an absolute
    // source directory -- but resolve it anyway so running this by hand with a
    // relative path does not silently produce URIs the server cannot open.
    const fs::path fixtures = fs::weakly_canonical(fs::absolute(argv[2])) / "tests" /
                              "fixtures" / "cli_smoke" / "config_root";

    if (!fs::exists(server_bin)) {
        std::cerr << "server binary does not exist: " << server_bin << "\n";
        return 2;
    }
    if (!fs::exists(fixtures / "hints_off" / "lazyverilog.toml")) {
        std::cerr << "fixtures are missing: " << fixtures << "\n";
        return 2;
    }

    // Both providers are advertised unconditionally, whatever the config says
    // and whatever the client offers to register dynamically.  This is clangd's
    // contract -- `{"foldingRangeProvider", true}` and `{"inlayHintProvider",
    // true}` are literals there, and no config file influences them -- and it
    // is forced here by the root moving into the server: capabilities are
    // exchanged before any file is open, so there is no one config to answer
    // from any more.
    //
    // What the config decides is the reply, checked further down.
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "hints_off"));
        expect(contains(out, R"("inlayHintProvider":true)"),
               "inlayHintProvider is advertised even where the config turns hints off");
        expect(contains(out, R"("foldingRangeProvider":true)"),
               "foldingRangeProvider is advertised even where the config turns hints off");
    }
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "folding_off"));
        expect(contains(out, R"("foldingRangeProvider":true)"),
               "foldingRangeProvider is advertised even where the config turns folding off");
    }
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "hints_on"),
                                              /*dynamic_registration=*/true);
        expect(contains(out, R"("inlayHintProvider":true)"),
               "inlayHintProvider is advertised to a client that registers dynamically");
        expect(contains(out, R"("foldingRangeProvider":true)"),
               "foldingRangeProvider is advertised to a client that registers dynamically");
    }

    // A client that sends no root at all still gets the same reply.  This is
    // the shape the Neovim plugin now sends -- it stopped choosing a root_dir,
    // because choosing one is what it was getting wrong.
    {
        const auto out = initialize_with_root(server_bin, "");
        expect(contains(out, R"("inlayHintProvider":true)"),
               "inlayHintProvider is advertised to a client that sends no root");
        expect(contains(out, R"("foldingRangeProvider":true)"),
               "foldingRangeProvider is advertised to a client that sends no root");
    }

    // The switch has to reach the handler, not only the capability reply.
    {
        const auto out = folds_for_root(server_bin, path_to_uri(fixtures / "folding_off"));
        expect(contains(out, R"("id":2)"), "a fold request is answered when folding is off");
        expect(!contains(out, R"("startLine")"),
               "no folds are computed when [folding].enable is false");
    }
    // The config that governs a file is the nearest one above the *file*, not
    // one the client named.  This probe sits two directories below
    // folding_off/lazyverilog.toml with no config of its own, and under the old
    // contract -- `<root>/lazyverilog.toml` and nothing else -- it was served
    // defaults, so folds came back.
    {
        const auto out = folds_for_nested_file(server_bin,
                                               path_to_uri(fixtures / "folding_off"));
        expect(contains(out, R"("id":2)"), "a nested fold request is answered");
        expect(!contains(out, R"("startLine")"),
               "a file below the config inherits [folding].enable = false from it");
    }

    // The same file, with no rootUri at all -- the shape the Neovim plugin now
    // sends.  Nothing but the opened file's own path says which project this
    // is, which is the point: the server walks up from the file and finds
    // folding_off/lazyverilog.toml on its own.
    {
        const auto out =
            folds_for_uri(server_bin, "",
                          path_to_uri(fixtures / "folding_off" / "rtl" / "core" /
                                      "fold_probe.sv"));
        expect(contains(out, R"("id":2)"), "a rootless fold request is answered");
        expect(!contains(out, R"("startLine")"),
               "the config is found from the opened file when no root was sent");
    }

    // Formatter options are per file too, not only the two capability switches.
    // One session, two buffers, two projects that disagree about indent_size --
    // and no rootUri, so the only thing that can tell them apart is each file's
    // own path.  A session-wide config gives both files the same indent, which
    // fails whichever project it picked.
    {
        const auto out = format_two_projects(
            server_bin, path_to_uri(fixtures / "indent_two" / "m.sv"),
            path_to_uri(fixtures / "indent_eight" / "m.sv"));
        expect(contains(out, R"(\n  logic x;)"),
               "the 2-space project's file is formatted with its own indent_size");
        expect(contains(out, R"(\n        logic x;)"),
               "the 8-space project's file is formatted with its own indent_size");
    }

    // And the same request against a root that leaves folding on must produce
    // some, or the check above would pass against a server that never folds.
    {
        const auto out = folds_for_root(server_bin, path_to_uri(fixtures / "hints_off"));
        expect(contains(out, R"("startLine")"), "folds are computed when folding is on");
    }

    std::cerr << "config-root-cli-smoke: " << (checks_run - checks_failed) << "/" << checks_run
              << " checks passed\n";
    return checks_failed == 0 ? 0 : 1;
}
