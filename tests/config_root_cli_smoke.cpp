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
std::string folds_for_root(const fs::path& server_bin, const std::string& root_uri) {
    static int counter = 0;
    const fs::path input = fs::temp_directory_path() /
                           ("lazyverilog-config-folds-" +
                            std::to_string(cli_process::current_process_id()) + "-" +
                            std::to_string(counter++) + ".jsonrpc");
    const std::string doc_uri = root_uri + "/fold_probe.sv";
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

    // The supported layout: the config sits in the root the client sent, and a
    // setting turned off in it reaches the capability reply.
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "hints_off"));
        expect(contains(out, R"("inlayHintProvider":false)"),
               "hints disabled by the config in the workspace root");
    }

    // The same layout against a config that turns the setting on.  Without this
    // a server that always answered `false` would pass the case above, and a
    // server that ignored the file entirely would pass the case below.
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "hints_on"));
        expect(contains(out, R"("inlayHintProvider":true)"),
               "hints enabled by the config in the workspace root");
    }

    // The client's root is two directories below the config, so by the contract
    // there is no config: the reply must come from built-in defaults, where
    // inlay hints are on.  An upward walk would find `enable = false` above and
    // answer `false` here.
    {
        const auto out = initialize_with_root(
            server_bin, path_to_uri(fixtures / "hints_off" / "rtl" / "core"));
        expect(contains(out, R"("inlayHintProvider":true)"),
               "no config in the workspace root, so defaults -- not the one above it");
    }

    // `[folding].enable` reaches the wire the same way.  Neovim re-requests the
    // whole file's folds from every didChange and answers `dynamicRegistration
    // = false` for foldingRange, so this reply is the only chance to stop it
    // asking -- there is no second one later in the session.
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "folding_off"));
        expect(contains(out, R"("foldingRangeProvider":false)"),
               "folding disabled by the config in the workspace root");
    }
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "hints_off"));
        expect(contains(out, R"("foldingRangeProvider":true)"),
               "folding on by default when the config does not mention it");
    }

    // A client that takes dynamic registration must NOT also be told statically.
    // Neovim's supports_method() answers from the static capability when one is
    // there, so advertising both makes a later client/unregisterCapability do
    // nothing and the client keeps requesting for the rest of the session.
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "hints_on"),
                                              /*dynamic_registration=*/true);
        expect(!contains(out, R"("inlayHintProvider")"),
               "inlayHintProvider withheld from a client that registers dynamically");
        expect(!contains(out, R"("foldingRangeProvider")"),
               "foldingRangeProvider withheld from a client that registers dynamically");
    }

    // The switch has to reach the handler, not only the capability reply.
    {
        const auto out = folds_for_root(server_bin, path_to_uri(fixtures / "folding_off"));
        expect(contains(out, R"("id":2)"), "a fold request is answered when folding is off");
        expect(!contains(out, R"("startLine")"),
               "no folds are computed when [folding].enable is false");
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
