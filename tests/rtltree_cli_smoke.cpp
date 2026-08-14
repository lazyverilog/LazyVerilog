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
    const fs::path fixtures = repo_root / "tests" / "fixtures" / "cli_smoke" / "rtltree";
    const fs::path top = fixtures / "m_top.sv";
    const fs::path leaf = fixtures / "m_leaf.sv";
    const fs::path filelist = fixtures / "rtltree.f";

    if (!fs::exists(rtltree_bin)) {
        std::cerr << "rtltree binary does not exist: " << rtltree_bin << "\n";
        return 2;
    }
    if (!fs::exists(top) || !fs::exists(leaf) || !fs::exists(filelist)) {
        std::cerr << "fixtures missing under " << fixtures << "\n";
        return 2;
    }

    // No arguments: usage error.
    {
        auto result = run_command(rtltree_bin, "");
        expect(result.exit_code == 1, "no-args exits 1");
    }

    // Forward hierarchy: m_top instantiates m_leaf twice.
    {
        auto result = run_command(rtltree_bin, "-f " + shell_quote(filelist) + " " + shell_quote(top));
        expect(result.exit_code == 0, "forward hierarchy exits 0");
        expect(contains(result.stdout_text, "m_top ["), "root node is m_top");
        expect(contains(result.stdout_text, "m_leaf (u_leaf_a)"), "child instance u_leaf_a listed");
        expect(contains(result.stdout_text, "m_leaf (u_leaf_b)"), "child instance u_leaf_b listed");
    }

    // Reverse hierarchy: m_leaf is instantiated by m_top.
    {
        auto result = run_command(
            rtltree_bin, "-f " + shell_quote(filelist) + " --reverse " + shell_quote(leaf));
        expect(result.exit_code == 0, "reverse hierarchy exits 0");
        expect(contains(result.stdout_text, "m_leaf ["), "root node is m_leaf");
        expect(contains(result.stdout_text, "m_top (u_leaf_a)"), "reverse parent u_leaf_a listed");
    }

    if (checks_failed > 0) {
        std::cerr << checks_failed << "/" << checks_run << " checks failed\n";
        return 1;
    }
    std::cout << "rtltree CLI smoke: " << checks_run << " checks passed\n";
    return 0;
}
