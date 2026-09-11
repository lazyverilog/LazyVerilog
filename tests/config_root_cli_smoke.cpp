// The server's `initialize` reply must be built from the project's real
// lazyverilog.toml.
//
// Capabilities are exchanged once and never revised, so a config found after
// the reply cannot take a feature back off: the client goes on requesting it
// for the whole session and the server answers every request with nothing.
// `[inlay_hint].enable` is the case that actually reaches the wire --
// `caps.inlayHintProvider` is built from it -- so it is what this test reads.
//
// The client's root is whatever its own root markers picked, which is regularly
// *below* the config: Neovim's `vim.fs.root` resolves a flat marker list by
// marker order rather than by proximity, so a `.git` high in the tree wins over
// a nearer lazyverilog.toml, and with no marker at all the root is the opened
// file's own directory.  So `initialize` has to walk up for the config the same
// way didOpen does.
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
std::string initialize_with_root(const fs::path& server_bin, const std::string& root_uri) {
    static int counter = 0;
    const fs::path input = fs::temp_directory_path() /
                           ("lazyverilog-config-root-" +
                            std::to_string(cli_process::current_process_id()) + "-" +
                            std::to_string(counter++) + ".jsonrpc");
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":")" +
                     root_uri +
                     R"(","capabilities":{"textDocument":{"inlayHint":{}}}}})");
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

    // The client's root is the config's own directory: the case that already
    // worked, kept so a regression cannot pass by breaking everything equally.
    {
        const auto out = initialize_with_root(server_bin, path_to_uri(fixtures / "hints_off"));
        expect(contains(out, R"("inlayHintProvider":false)"),
               "hints disabled, client rooted at the config directory");
    }

    // The client's root is two directories below the config.  Before the
    // upward walk this answered `true` from built-in defaults, and the client
    // then requested hints on every keystroke for the rest of the session.
    {
        const auto out = initialize_with_root(
            server_bin, path_to_uri(fixtures / "hints_off" / "rtl" / "core"));
        expect(contains(out, R"("inlayHintProvider":false)"),
               "hints disabled, client rooted below the config directory");
    }

    // The same depth against a config that enables hints.  Without this, a
    // server that simply always answered `false` would pass the two above.
    {
        const auto out = initialize_with_root(
            server_bin, path_to_uri(fixtures / "hints_on" / "rtl" / "core"));
        expect(contains(out, R"("inlayHintProvider":true)"),
               "hints enabled, client rooted below the config directory");
    }

    std::cerr << "config-root-cli-smoke: " << (checks_run - checks_failed) << "/" << checks_run
              << " checks passed\n";
    return checks_failed == 0 ? 0 : 1;
}
