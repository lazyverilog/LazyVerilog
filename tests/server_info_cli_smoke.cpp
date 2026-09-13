// The `initialize` reply must identify the server.
//
// `InitializeResult.serverInfo` is where a client learns the server's name and
// version, and it is the only place: capabilities are exchanged once and never
// revised, so a reply that omits the field leaves the client with nothing to
// report for the rest of the session.  Neovim assigns `client.server_info`
// straight from it, and `:LspInfo` prints the literal
// "? (no serverInfo.version response)" when it is nil -- which is exactly what
// it printed before this reply carried the field.
//
// That field does not exist in LspCpp's own InitializeResult; it is added by
// the anchored patch in cmake/lspcpp/initialize_server_info.h.in, applied at
// configure time.  An upstream bump that moved the anchor would fail the
// configure -- but a patch quietly dropped some other way would not, and the
// symptom is a reply that is still well-formed and still passes every other
// check.  This test is what notices.
//
// The version is read back out of the same binary via `--version` rather than
// hard-coded: the string comes from `git describe` at configure time, so the
// contract worth pinning is that the two agree, not what either one says
// today.  A reply that reported some other build would be worse than none.
//
// This drives the real binary over stdio because the behaviour under test is
// the content of an LSP reply.

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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// The version out of `--version`, whose output is `lazyverilog-lsp <version>`.
/// Empty when the binary printed something else, which the caller reports as a
/// failed check rather than silently comparing against nothing.
std::string version_from_cli(const fs::path& server_bin) {
    const auto result = run_command(server_bin, "--version");
    expect(result.exit_code == 0, "--version exits cleanly");

    const std::string prefix = "lazyverilog-lsp ";
    const auto start = result.stdout_text.find(prefix);
    if (start == std::string::npos)
        return {};

    std::string version = result.stdout_text.substr(start + prefix.size());
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' ||
                                version.back() == ' '))
        version.pop_back();
    return version;
}

/// Send `initialize`, then `exit`, and return the server's stdout.  Writing
/// both messages up front and letting the server read to EOF keeps this to one
/// blocking call with no timing assumptions.
std::string initialize_reply(const fs::path& server_bin) {
    const fs::path input = fs::temp_directory_path() /
                           ("lazyverilog-server-info-" +
                            std::to_string(cli_process::current_process_id()) + ".jsonrpc");
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":null,"capabilities":{}}})");
        out << frame(R"({"jsonrpc":"2.0","method":"exit","params":{}})");
    }

    const auto result = run_command(server_bin, "< " + shell_quote(input));
    fs::remove(input);
    expect(result.exit_code == 0, "the server exits cleanly after an initialize");
    return result.stdout_text;
}

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

    const std::string version = version_from_cli(server_bin);
    // Without this the comparison below would pass against a reply carrying an
    // empty version, which is the same "nothing to show" this test exists to
    // rule out.
    expect(!version.empty(), "--version prints a version");

    const std::string out = initialize_reply(server_bin);

    expect(contains(out, R"("serverInfo":)"), "the initialize reply carries serverInfo");
    expect(contains(out, R"("serverInfo":{"name":"lazyverilog")"),
           "serverInfo names the server");
    expect(!version.empty() && contains(out, R"("version":")" + version + R"(")"),
           "serverInfo reports the same version the binary prints");

    std::cerr << "server-info-cli-smoke: " << (checks_run - checks_failed) << "/" << checks_run
              << " checks passed\n";
    return checks_failed == 0 ? 0 : 1;
}
