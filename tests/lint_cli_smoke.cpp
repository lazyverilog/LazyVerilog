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

    // Diagnostic codes are printed, because --nowarn is unusable if the value it
    // takes is not visible anywhere.  m_top.sv carries one of each family: a
    // lint rule and a slang error.
    {
        auto result = run_command(lint_bin, shell_quote(top));
        expect(contains(result.stdout_text, "[lint-naming-input-port]"),
              "a lint diagnostic prints its code");
        expect(contains(result.stdout_text, "[UnknownDirective]"),
              "a slang error prints its slang diagnostic name as its code");
    }

    // --nowarn on an exact lint code drops that rule and nothing else.
    {
        auto result = run_command(lint_bin, "--nowarn lint-naming-input-port " + shell_quote(top));
        expect(!contains(result.stdout_text, "[lint-naming-input-port]"),
              "--nowarn <lint code> drops that rule");
        expect(contains(result.stdout_text, "unknown macro"),
              "--nowarn <lint code> leaves the compilation diagnostic alone");
        expect(result.exit_code == 2, "an unsuppressed error still sets exit 2");
    }

    // The lint namespace is hierarchical: a parent silences its children.
    {
        auto category = run_command(lint_bin, "--nowarn lint-naming " + shell_quote(top));
        expect(!contains(category.stdout_text, "[lint-naming-input-port]"),
              "--nowarn lint-naming silences a rule inside that category");

        auto everything = run_command(lint_bin, "--nowarn lint " + shell_quote(top));
        expect(!contains(everything.stdout_text, "[lint-"),
              "--nowarn lint silences every lint rule");
        expect(contains(everything.stdout_text, "unknown macro"),
              "--nowarn lint is not a --compile-only in disguise");
    }

    // A suppressed error stops setting the exit status.  Silencing a diagnostic
    // and still failing the build on it would make the flag useless in CI,
    // which is the one place it is most wanted.
    {
        auto result = run_command(lint_bin,
                                  "--nowarn lint --nowarn UnknownDirective " + shell_quote(top));
        expect(result.stdout_text.empty(), "both families suppressed leaves no output");
        expect(result.exit_code == 0, "a suppressed error no longer sets exit 2");
    }

    // slang group names stand for their members.  m_dut.sv's type mismatch is
    // `implicit-conv`, which lives in slang's `conversion` group.
    {
        const std::string flist = " -f " + shell_quote(maxerror_filelist) + " " +
                                  shell_quote(maxerror_dut);

        auto plain = run_command(lint_bin, "--maxerror 0" + flist);
        expect(contains(plain.stdout_text, "[implicit-conv]"),
              "a slang warning prints its -W option name as its code");

        auto by_code = run_command(lint_bin, "--maxerror 0 --nowarn implicit-conv" + flist);
        expect(!contains(by_code.stdout_text, "implicit conversion from"),
              "--nowarn <slang option name> drops that warning");

        auto by_group = run_command(lint_bin, "--maxerror 0 --nowarn conversion" + flist);
        expect(!contains(by_group.stdout_text, "implicit conversion from"),
              "--nowarn <slang group> drops the warnings in that group");
    }

    // A --nowarn value that silences nothing is a typo, and a typo that looks
    // like it worked is worse than no flag at all.
    {
        for (const std::string bad : {"lint-nam", "lint-bogus", "no-such-warning", "lint-naming-mod"}) {
            auto result = run_command(lint_bin, "--nowarn " + shell_quote(bad) + " " +
                                                    shell_quote(top));
            expect(result.exit_code == 1, "--nowarn " + bad + " exits 1");
            expect(contains(result.stderr_text, "Unknown --nowarn code"),
                  "--nowarn " + bad + " says which value it rejected");
        }

        auto missing = run_command(lint_bin, "--nowarn");
        expect(missing.exit_code == 1, "--nowarn with no value exits 1");
    }

    // --help is where the code names come from, so it has to carry them.
    {
        auto help = run_command(lint_bin, "--help");
        expect(help.exit_code == 0, "--help exits 0");
        expect(contains(help.stdout_text, "--nowarn"), "--help advertises --nowarn");
        expect(contains(help.stdout_text, "lint-naming-module"),
              "--help lists an individual lint code");
        expect(contains(help.stdout_text, "lint-style-trailing-whitespace"),
              "--help lists the whole table, not just the first section");
        expect(contains(help.stdout_text, "conversion"),
              "--help lists slang's warning groups");
        expect(contains(help.stdout_text, "MissingTimeScale"),
              "--help explains how slang errors are named");
    }

    if (checks_failed > 0) {
        std::cerr << checks_failed << "/" << checks_run << " checks failed\n";
        return 1;
    }
    std::cout << "lint CLI smoke: " << checks_run << " checks passed\n";
    return 0;
}
