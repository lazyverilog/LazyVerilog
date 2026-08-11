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
    const fs::path memory_top = repo_root / "demo" / "memory_top.sv";
    const fs::path vcode = repo_root / "demo" / "vcode.f";

    if (!fs::exists(lint_bin)) {
        std::cerr << "lint binary does not exist: " << lint_bin << "\n";
        return 2;
    }
    if (!fs::exists(memory_top) || !fs::exists(vcode)) {
        std::cerr << "fixtures missing under " << repo_root << "/demo\n";
        return 2;
    }

    // No arguments: usage error.
    {
        auto result = run_command(lint_bin, "");
        expect(result.exit_code == 1, "no-args exits 1");
    }

    // Single-file mode: reports both compilation and lint diagnostics.
    {
        auto result = run_command(lint_bin, shell_quote(memory_top));
        expect(result.exit_code == 2, "memory_top.sv has an error-severity diagnostic (exit 2)");
        expect(contains(result.stdout_text, "unknown macro"),
              "reports the unknown `BB compilation diagnostic");
        expect(contains(result.stdout_text, "[naming]"), "reports a [naming] lint diagnostic");
        expect(contains(result.stdout_text, "memory_top.sv:"),
              "diagnostic lines are prefixed with the source file path");
    }

    // --lint-only: drops compilation diagnostics, keeps lint diagnostics.
    {
        auto result = run_command(lint_bin, "--lint-only " + shell_quote(memory_top));
        expect(!contains(result.stdout_text, "unknown macro"),
              "--lint-only drops the compilation diagnostic");
        expect(contains(result.stdout_text, "[naming]"),
              "--lint-only keeps the lint diagnostic");
    }

    // -f whole-project mode: lints every file in the filelist, not just one.
    {
        auto result = run_command(lint_bin, "-f " + shell_quote(vcode));
        expect(contains(result.stdout_text, "memory_top.sv:"),
              "-f mode reports diagnostics for memory_top.sv");
        expect(contains(result.stdout_text, "folding_demo.sv:"),
              "-f mode reports diagnostics for a second project file");
    }

    if (checks_failed > 0) {
        std::cerr << checks_failed << "/" << checks_run << " checks failed\n";
        return 1;
    }
    std::cout << "lint CLI smoke: " << checks_run << " checks passed\n";
    return 0;
}
