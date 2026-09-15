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
///
/// @p capabilities is the `capabilities` object of the request, so a caller can
/// vary what the client offers.
std::string initialize_reply(const fs::path& server_bin,
                             const std::string& capabilities = "{}") {
    const fs::path input = fs::temp_directory_path() /
                           ("lazyverilog-server-info-" +
                            std::to_string(cli_process::current_process_id()) + ".jsonrpc");
    {
        std::ofstream out(input, std::ios::binary);
        out << frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                     R"("processId":1,"rootUri":null,"capabilities":)" + capabilities + "}}");
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

    // ── positionEncoding (LSP 3.17) ──────────────────────────────────────────
    //
    // `ServerCapabilities.positionEncoding` is the other field LspCpp does not
    // declare and this build patches in, from CMakeLists.txt rather than a
    // template file.  It carries more than a label: it decides whether every
    // Position on the wire is counted in UTF-16 units or bytes, and the two
    // sides silently disagreeing is an off-by-a-few-columns bug that only
    // appears on lines with non-ASCII text.
    //
    // The offers below are the real ones.  vscode-languageclient 9 hardcodes
    // `positionEncodings = ['utf-16']` and *throws* on any other answer --
    // "Unsupported position encoding ... received from server", which fails the
    // session outright -- so answering a UTF-16-only client anything else is not
    // a degraded mode, it is a client that will not start.  Neovim 0.12.5 offers
    // all three with utf-8 first.
    const auto encoding_for = [&](const char* offer) {
        return initialize_reply(server_bin,
                                std::string(R"({"general":{"positionEncodings":)") + offer + "}}");
    };

    expect(contains(encoding_for(R"(["utf-16"])"), R"("positionEncoding":"utf-16")"),
           "a UTF-16-only client (VS Code) is answered utf-16");
    expect(contains(encoding_for(R"(["utf-8","utf-16","utf-32"])"),
                    R"("positionEncoding":"utf-8")"),
           "a client offering utf-8 first (Neovim) is answered utf-8");
    expect(contains(encoding_for(R"(["utf-16","utf-8"])"), R"("positionEncoding":"utf-8")"),
           "utf-8 is taken wherever it appears in the offer");
    expect(contains(encoding_for(R"(["utf-32"])"), R"("positionEncoding":"utf-16")"),
           "an offer of only utf-32 falls back to the protocol default");
    expect(contains(encoding_for("[]"), R"("positionEncoding":"utf-16")"),
           "an empty offer falls back to the protocol default");
    // A client from before 3.17 sends no `general` at all.  It ignores the
    // field, but the reply must still say utf-16 rather than guess.
    expect(contains(out, R"("positionEncoding":"utf-16")"),
           "a client that offers nothing is answered utf-16");

    std::cerr << "server-info-cli-smoke: " << (checks_run - checks_failed) << "/" << checks_run
              << " checks passed\n";
    return checks_failed == 0 ? 0 : 1;
}
