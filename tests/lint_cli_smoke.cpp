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

    const fs::path maxerror_fixtures = repo_root / "tests" / "fixtures" / "cli_smoke" / "maxerror";
    const fs::path maxerror_dut = maxerror_fixtures / "m_dut.sv";
    const fs::path maxerror_filelist = maxerror_fixtures / "maxerror.f";

    if (!fs::exists(lint_bin)) {
        std::cerr << "lint binary does not exist: " << lint_bin << "\n";
        return 2;
    }
    if (!fs::exists(top) || !fs::exists(filelist) || !fs::exists(semantic_error)) {
        std::cerr << "fixtures missing under " << fixtures << "\n";
        return 2;
    }
    if (!fs::exists(maxerror_dut) || !fs::exists(maxerror_filelist)) {
        std::cerr << "fixtures missing under " << maxerror_fixtures << "\n";
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

    // --maxerror: raises slang's compilation error limit.
    //
    // The fixture design holds one element with a `timescale and 80+ without
    // one, so slang emits MissingTimeScale past its default 64-error limit,
    // sets sawFatalError, and abandons the rest of elaboration.  Every
    // diagnostic it had not reached yet -- here the type mismatch in m_dut.sv's
    // `foo` -- is then silently never produced.  This is the exact failure mode
    // that makes a real design report nothing but timescale errors.
    {
        const std::string target = shell_quote(maxerror_dut);
        const std::string flist = " -f " + shell_quote(maxerror_filelist) + " " + target;

        auto capped = run_command(lint_bin, flist);
        expect(contains(capped.stdout_text, "does not have a time scale"),
              "default limit still reports the timescale errors");
        expect(!contains(capped.stdout_text, "implicit conversion"),
              "default 64-error limit suppresses the later type diagnostic");

        auto unlimited = run_command(lint_bin, "--maxerror 0" + flist);
        expect(contains(unlimited.stdout_text, "implicit conversion from 'type_b' to 'type_a'"),
              "--maxerror 0 lifts the limit so the type diagnostic survives");

        auto raised = run_command(lint_bin, "--maxerror 500" + flist);
        expect(contains(raised.stdout_text, "implicit conversion from 'type_b' to 'type_a'"),
              "--maxerror 500 also clears the limit for this design");

        // Lowering the limit is honored too, so the flag is not just a
        // one-way "raise it" switch.
        auto lowered = run_command(lint_bin, "--maxerror 1" + flist);
        expect(!contains(lowered.stdout_text, "implicit conversion"),
              "--maxerror 1 cuts elaboration off earlier than the default");
    }

    // --maxerror argument validation: a bad value must fail loudly rather than
    // silently falling back to 0, which would mean "unlimited" and quietly
    // change which diagnostics are reported.
    {
        for (const std::string bad : {"abc", "-5", "3.5", "99999999999999999999"}) {
            auto result = run_command(lint_bin, "--maxerror " + shell_quote(bad) + " " +
                                                    shell_quote(maxerror_dut));
            expect(result.exit_code == 1, "--maxerror " + bad + " exits 1");
        }

        auto missing = run_command(lint_bin, "--maxerror");
        expect(missing.exit_code == 1, "--maxerror with no value exits 1");

        auto usage = run_command(lint_bin, "--help");
        expect(contains(usage.stderr_text + usage.stdout_text, "--maxerror"),
              "usage text advertises --maxerror");
    }

    if (checks_failed > 0) {
        std::cerr << checks_failed << "/" << checks_run << " checks failed\n";
        return 1;
    }
    std::cout << "lint CLI smoke: " << checks_run << " checks passed\n";
    return 0;
}
