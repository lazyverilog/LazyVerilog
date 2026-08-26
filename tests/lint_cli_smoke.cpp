#include "cli_process.hpp"

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

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <lazyverilog-lint-binary> <repo-root>\n";
        return 2;
    }

    const fs::path lint_bin = argv[1];
    const fs::path repo_root = argv[2];
    const fs::path fixtures = repo_root / "tests" / "fixtures" / "cli_smoke" / "lint";
    const fs::path top = fixtures / "m_top.sv";
    const fs::path filelist = fixtures / "lint.f";
    const fs::path semantic_error = fixtures / "m_semantic_error.sv";

    if (!fs::exists(lint_bin)) {
        std::cerr << "lint binary does not exist: " << lint_bin << "\n";
        return 2;
    }
    if (!fs::exists(top) || !fs::exists(filelist) || !fs::exists(semantic_error)) {
        std::cerr << "fixtures missing under " << fixtures << "\n";
        return 2;
    }

    // No arguments: usage error.
    {
        auto result = run_command(lint_bin, "");
        expect(result.exit_code == 1, "no-args exits 1");
    }

    // Single-file mode: reports both compilation and lint diagnostics.
    {
        auto result = run_command(lint_bin, shell_quote(top));
        expect(result.exit_code == 2, "m_top.sv has an error-severity diagnostic (exit 2)");
        expect(contains(result.stdout_text, "unknown macro"),
              "reports the unknown macro compilation diagnostic");
        expect(contains(result.stdout_text, "[naming]"), "reports a [naming] lint diagnostic");
        expect(contains(result.stdout_text, "m_top.sv:"),
              "diagnostic lines are prefixed with the source file path");
    }

    // --lint-only: drops compilation diagnostics, keeps lint diagnostics.
    {
        auto result = run_command(lint_bin, "--lint-only " + shell_quote(top));
        expect(!contains(result.stdout_text, "unknown macro"),
              "--lint-only drops the compilation diagnostic");
        expect(contains(result.stdout_text, "[naming]"),
              "--lint-only keeps the lint diagnostic");
    }

    // --compile-only: drops lint diagnostics, keeps compilation diagnostics.
    {
        auto result = run_command(lint_bin, "--compile-only " + shell_quote(top));
        expect(contains(result.stdout_text, "unknown macro"),
              "--compile-only keeps the compilation diagnostic");
        expect(!contains(result.stdout_text, "[naming]"),
              "--compile-only drops the lint diagnostic");
    }

    // Single-file mode: reports a real semantic (not just parse) diagnostic
    // for the target file, regardless of the [compilation] background_compilation
    // toml setting — regression coverage for a bug where Analyzer::open()'s
    // synchronous CLI path never registered a tracked version, so
    // Analyzer::semantic_diagnostics() unconditionally treated the file as
    // stale and dropped every semantic diagnostic for it.
    {
        auto result = run_command(lint_bin, shell_quote(semantic_error));
        expect(result.exit_code == 2, "semantic error has an error-severity diagnostic (exit 2)");
        expect(contains(result.stdout_text, "redefinition"),
              "reports the semantic redefinition diagnostic");
    }

    // --version: prints a version and exits 0.
    {
        auto result = run_command(lint_bin, "--version");
        expect(result.exit_code == 0, "--version exits 0");
        expect(contains(result.stdout_text, "lazyverilog-lint"),
              "--version reports the binary name");
    }

    // -f whole-project mode: lints every file in the filelist, not just one.
    {
        auto result = run_command(lint_bin, "-f " + shell_quote(filelist));
        expect(contains(result.stdout_text, "m_top.sv:"),
              "-f mode reports diagnostics for m_top.sv");
        expect(contains(result.stdout_text, "m_second.sv:"),
              "-f mode reports diagnostics for a second project file");
    }

    if (checks_failed > 0) {
        std::cerr << checks_failed << "/" << checks_run << " checks failed\n";
        return 1;
    }
    std::cout << "lint CLI smoke: " << checks_run << " checks passed\n";
    return 0;
}
