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
        std::cerr << "usage: " << argv[0] << " <lazyverilog-rtltree-binary> <repo-root>\n";
        return 2;
    }

    const fs::path rtltree_bin = argv[1];
    const fs::path repo_root = argv[2];
    const fs::path memory_top = repo_root / "demo" / "memory_top.sv";
    const fs::path memory = repo_root / "demo" / "memory.sv";

    if (!fs::exists(rtltree_bin)) {
        std::cerr << "rtltree binary does not exist: " << rtltree_bin << "\n";
        return 2;
    }
    if (!fs::exists(memory_top) || !fs::exists(memory)) {
        std::cerr << "fixtures missing under " << repo_root << "/demo\n";
        return 2;
    }

    // No arguments: usage error.
    {
        auto result = run_command(rtltree_bin, "");
        expect(result.exit_code == 1, "no-args exits 1");
    }

    // Forward hierarchy: memory_top instantiates memory twice.
    {
        auto result = run_command(rtltree_bin, shell_quote(memory_top));
        expect(result.exit_code == 0, "forward hierarchy exits 0");
        expect(contains(result.stdout_text, "memory_top ["), "root node is memory_top");
        expect(contains(result.stdout_text, "memory (u_mem2)"), "child instance u_mem2 listed");
        expect(contains(result.stdout_text, "memory (u_mem3)"), "child instance u_mem3 listed");
    }

    // Reverse hierarchy: memory is instantiated by memory_top.
    {
        auto result = run_command(rtltree_bin, "--reverse " + shell_quote(memory));
        expect(result.exit_code == 0, "reverse hierarchy exits 0");
        expect(contains(result.stdout_text, "memory ["), "root node is memory");
        expect(contains(result.stdout_text, "memory_top (u_mem2)"), "reverse parent u_mem2 listed");
    }

    if (checks_failed > 0) {
        std::cerr << checks_failed << "/" << checks_run << " checks failed\n";
        return 1;
    }
    std::cout << "rtltree CLI smoke: " << checks_run << " checks passed\n";
    return 0;
}
