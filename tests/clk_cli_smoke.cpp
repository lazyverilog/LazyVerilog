// End-to-end smoke test for `lazyverilog-clk`.
//
// The unit tests in test_clock_domain.cpp pin clock *extraction* from a single
// procedural block.  This pins the whole pipeline instead: filelist bootstrap,
// full (non-LintMode) elaboration, hierarchical instance lookup, and the
// forward/backward traces that cross child instance boundaries.
//
// Fixture design lives in tests/rtl/clk_fixtures/; each port there exercises a
// different trace shape, so the expectations below double as the tool's
// specification.
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

/// Matches a whole rendered table row, so a domain of "clk_a, rst_n" is not
/// satisfied by a row that only happens to mention clk_a.
bool has_row(const std::string& output, const std::string& port, const std::string& domain) {
    size_t pos = output.find("| " + port + " ");
    if (pos == std::string::npos)
        return false;
    const size_t end = output.find('\n', pos);
    const std::string row = output.substr(pos, end - pos);
    return contains(row, "| " + domain + " ");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <lazyverilog-clk-binary> <repo-root>\n";
        return 2;
    }

    const fs::path clk_bin = argv[1];
    const fs::path repo_root = argv[2];
    const fs::path fixtures = repo_root / "tests" / "rtl" / "clk_fixtures";
    const fs::path filelist = fixtures / "files.f";

    if (!fs::exists(clk_bin)) {
        std::cerr << "clk binary does not exist: " << clk_bin << "\n";
        return 2;
    }
    if (!fs::exists(filelist)) {
        std::cerr << "fixtures missing under " << fixtures << "\n";
        return 2;
    }

    const std::string base = "-f " + shell_quote(filelist) + " ";

    // No arguments: usage error.
    {
        auto result = run_command(clk_bin, "");
        expect(result.exit_code == 1, "no-args exits 1");
    }

    // Hierarchical path resolves, and every trace shape reports as expected.
    {
        auto result = run_command(clk_bin, base + "top.u_dut");
        expect(result.exit_code == 0, "hierarchical path exits 0");
        expect(contains(result.stdout_text, "top.u_dut (dut)"), "reports resolved instance");

        // A clock port is an edge source, not combinational feedthrough.
        expect(has_row(result.stdout_text, "clk_a", "(edge source)"), "clk_a is an edge source");

        // Forward traces: input -> comb -> flop, and input -> flop directly.
        expect(has_row(result.stdout_text, "data_i", "clk_a, rst_n"),
               "data_i captured by the clk_a flop");
        expect(has_row(result.stdout_text, "direct_i", "clk_b"),
               "direct_i captured by the clk_b flop");

        // Every capturing clock is listed when a port fans out to several.
        expect(has_row(result.stdout_text, "dual_i", "clk_a, clk_b, rst_n"),
               "dual_i lists both capture clocks");

        // Backward traces: output <- flop, and output <- comb <- flop.
        expect(has_row(result.stdout_text, "data_o", "clk_a, rst_n"),
               "data_o driven by the clk_a flop");
        expect(has_row(result.stdout_text, "comb_o", "clk_b"),
               "comb_o driven through comb by the clk_b flop");

        // A path with no state element on it is called out, not left blank.
        expect(has_row(result.stdout_text, "thru_i", "(comb feedthrough)"),
               "thru_i reaches no state element");
        expect(has_row(result.stdout_text, "thru_o", "(comb feedthrough)"),
               "thru_o reaches no state element");

        // Crossing a child instance: comb inside the child, flop in the parent.
        expect(has_row(result.stdout_text, "sub_o", "clk_a, rst_n"),
               "sub_o traced back through the comb child instance");

        // Flop inside the child, clocked by the child's own port: the clock
        // must be reported as the parent's net, not the child's local name.
        expect(has_row(result.stdout_text, "ff_i", "clk_b"),
               "ff_i clock mapped out to the parent net");
        expect(has_row(result.stdout_text, "ff_o", "clk_b"),
               "ff_o clock mapped out to the parent net");
    }

    // A bare module type name resolves when it is unambiguous.
    {
        auto result = run_command(clk_bin, base + "dut");
        expect(result.exit_code == 0, "module type name exits 0");
        expect(contains(result.stdout_text, "top.u_dut (dut)"),
               "module type name resolves to the single instance");
    }

    // An unknown instance fails and lists what is available.
    {
        auto result = run_command(clk_bin, base + "no_such_instance");
        expect(result.exit_code == 1, "unknown instance exits 1");
        expect(contains(result.stderr_text, "Known instances:"), "unknown instance lists candidates");
        expect(contains(result.stderr_text, "top.u_dut"), "candidate list includes top.u_dut");
    }

    // --version: prints a version and exits 0.
    {
        auto result = run_command(clk_bin, "--version");
        expect(result.exit_code == 0, "--version exits 0");
        expect(contains(result.stdout_text, "lazyverilog-clk"),
               "--version reports the binary name");
    }

    if (checks_failed > 0) {
        std::cerr << checks_failed << "/" << checks_run << " checks failed\n";
        return 1;
    }
    std::cout << "clk CLI smoke: " << checks_run << " checks passed\n";
    return 0;
}
