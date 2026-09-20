// Startup-banner smoke test.
//
// The banner is deliberately invisible to everything that captures output:
// run_command() redirects both streams to files, so stderr is never a terminal
// here and the default answer is "print nothing".  That is exactly the property
// worth pinning -- a banner that leaked into a pipe would corrupt
// lazyverilog-fmt's formatted source and lazyverilog-lsp's JSON-RPC -- but it
// also means the rendering itself can only be reached through
// LAZYVERILOG_FORCE_BANNER.  Both halves are asserted below.
//
// The assertions are written against the banner's *shape* (line count, longest
// line, the name and version it carries) rather than against bytes of
// assets/ascii_logo.txt, so re-exporting the art does not rewrite this file.

#include "cli_process.hpp"

#include <algorithm>
#include <filesystem>
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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

size_t line_count(const std::string& text) {
    return static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
}

/// Longest line in @p text, in bytes.  The logo is pure ASCII, so this is also
/// its column count.
size_t longest_line(const std::string& text) {
    size_t best = 0;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        size_t width = end - start;
        if (width > 0 && text[end - 1] == '\r')
            --width;
        best = std::max(best, width);
        if (end == text.size())
            break;
        start = end + 1;
    }
    return best;
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    // Windows CI has no POSIX libc; _putenv_s is the MSVC/MinGW spelling.
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    // An empty value is how the CRT spells "remove"; there is no unsetenv.
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/// Everything the banner is switched by, cleared, so each case starts from the
/// same place no matter what the developer's shell happens to export.
void clear_banner_env() {
    unset_env("LAZYVERILOG_FORCE_BANNER");
    unset_env("LAZYVERILOG_NO_BANNER");
    unset_env("NO_COLOR");
    unset_env("COLUMNS");
}

std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0]
                  << " <lazyverilog-lint-binary> <lazyverilog-fmt-binary> <repo-root>\n";
        return 2;
    }

    const fs::path lint_bin = argv[1];
    const fs::path fmt_bin = argv[2];
    const fs::path repo_root = argv[3];
    const fs::path source = repo_root / "tests" / "fixtures" / "cli_smoke" / "rtltree" / "m_leaf.sv";

    if (!fs::exists(lint_bin) || !fs::exists(fmt_bin)) {
        std::cerr << "binaries missing: " << lint_bin << " / " << fmt_bin << "\n";
        return 2;
    }
    if (!fs::exists(source)) {
        std::cerr << "fixture missing: " << source << "\n";
        return 2;
    }

    clear_banner_env();

    // The version the banner is expected to carry, taken from the binary itself
    // so a release bump does not have to be mirrored here.
    std::string version;
    {
        auto result = run_command(lint_bin, "--version");
        const std::string text = trimmed(result.stdout_text);
        const size_t space = text.rfind(' ');
        expect(result.exit_code == 0, "--version exits 0");
        expect(space != std::string::npos, "--version prints a name and a version");
        if (space != std::string::npos)
            version = text.substr(space + 1);
    }
    expect(!version.empty(), "a version string was recovered");

    // Redirected output is not a terminal, so nothing is printed.  This is the
    // case every other CLI smoke test and every scripted caller runs in.
    {
        auto result = run_command(lint_bin, shell_quote(source));
        expect(result.stderr_text.empty(), "a redirected run prints no banner on stderr");
        expect(!contains(result.stdout_text, version),
               "a redirected run prints no banner on stdout");
    }

    // Forced: the full drawing, on stderr, with the tool name and version under
    // it.  131 columns of art means a wide terminal and a tall block.
    {
        set_env("LAZYVERILOG_FORCE_BANNER", "1");
        set_env("COLUMNS", "200");
        set_env("NO_COLOR", "1");
        auto result = run_command(lint_bin, shell_quote(source));

        expect(line_count(result.stderr_text) >= 10,
               "a forced wide banner spans the whole drawing");
        expect(longest_line(result.stderr_text) >= 100,
               "a forced wide banner emits the art at its full width");
        expect(contains(result.stderr_text, "lazyverilog-lint"),
               "a forced banner names the binary it came from");
        expect(contains(result.stderr_text, version), "a forced banner carries the version");
        expect(!contains(result.stdout_text, "lazyverilog-lint"),
               "the banner never reaches stdout");
        clear_banner_env();
    }

    // Too narrow for the art: one line carrying the same two facts, rather than
    // a drawing wrapped into nonsense.
    {
        set_env("LAZYVERILOG_FORCE_BANNER", "1");
        set_env("COLUMNS", "60");
        set_env("NO_COLOR", "1");
        auto result = run_command(lint_bin, shell_quote(source));

        expect(contains(result.stderr_text, "lazyverilog-lint"),
               "a narrow banner still names the binary");
        expect(contains(result.stderr_text, version), "a narrow banner still carries the version");
        expect(longest_line(result.stderr_text) < 100, "a narrow banner does not emit the art");
        clear_banner_env();
    }

    // An explicit "off" beats an explicit "on".
    {
        set_env("LAZYVERILOG_FORCE_BANNER", "1");
        set_env("LAZYVERILOG_NO_BANNER", "1");
        set_env("COLUMNS", "200");
        auto result = run_command(lint_bin, shell_quote(source));
        expect(result.stderr_text.empty(), "LAZYVERILOG_NO_BANNER wins over the force flag");
        clear_banner_env();
    }

    // --version and --help are the machine-readable paths and stay unadorned
    // even when the banner is forced on.
    {
        set_env("LAZYVERILOG_FORCE_BANNER", "1");
        set_env("COLUMNS", "200");
        auto result = run_command(lint_bin, "--version");
        expect(result.stderr_text.empty(), "--version prints no banner");
        expect(trimmed(result.stdout_text) == "lazyverilog-lint " + version,
               "--version stdout is exactly the version line");

        auto help = run_command(lint_bin, "--help");
        expect(!contains(help.stderr_text, "@@@"), "--help prints no banner");
        clear_banner_env();
    }

    // lazyverilog-fmt writes the formatted source to stdout.  Forcing the
    // banner must not change one byte of it.
    {
        auto plain = run_command(fmt_bin, shell_quote(source));

        set_env("LAZYVERILOG_FORCE_BANNER", "1");
        set_env("COLUMNS", "200");
        set_env("NO_COLOR", "1");
        auto forced = run_command(fmt_bin, shell_quote(source));
        clear_banner_env();

        expect(!plain.stdout_text.empty(), "lazyverilog-fmt produced formatted source");
        expect(plain.stdout_text == forced.stdout_text,
               "forcing the banner leaves lazyverilog-fmt's stdout byte-identical");
        expect(longest_line(forced.stderr_text) >= 100,
               "lazyverilog-fmt's banner went to stderr");
    }

    std::cerr << "banner-cli-smoke: " << (checks_run - checks_failed) << "/" << checks_run
              << " checks passed\n";
    return checks_failed == 0 ? 0 : 1;
}
