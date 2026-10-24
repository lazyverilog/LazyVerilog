// The formatter test corpus, shared by the invariant and CST oracle tests.
// See docs/dev/formatter-contracts.md.
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace lvtest {

namespace fs = std::filesystem;

inline fs::path find_repo_dir(const fs::path& relative) {
    auto dir = fs::current_path();
    while (true) {
        if (fs::exists(dir / relative))
            return dir / relative;
        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return relative;
}

struct CorpusFile {
    std::string name;
    std::string text;
};

inline const std::vector<CorpusFile>& corpus() {
    static const std::vector<CorpusFile> files = [] {
        std::vector<CorpusFile> out;
        for (const char* dir : {"tests/fixtures/formatter_corpus", "tests/rtl/hpc60",
                                "tests/rtl/lazyverilog_formatter_corpus/rtl"}) {
            const fs::path root = find_repo_dir(dir);
            if (!fs::is_directory(root))
                continue;
            std::vector<fs::path> paths;
            for (const auto& e : fs::directory_iterator(root))
                if (e.is_regular_file() && (e.path().extension() == ".sv" || e.path().extension() == ".svh"))
                    paths.push_back(e.path());
            std::sort(paths.begin(), paths.end());
            for (const auto& p : paths) {
                std::ifstream in(p, std::ios::binary);
                out.push_back({p.filename().string(),
                               std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())});
            }
        }
        return out;
    }();
    return files;
}

} // namespace lvtest
