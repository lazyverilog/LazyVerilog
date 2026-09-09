#include "cli_project.hpp"
#include "document_state.hpp"
#include "features/lint.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: lazyverilog-lint [-f <filelist>] [--lint-only] [--compile-only] "
                  "[--maxerror <n>] [--version] [<file>]\n";
}

/// Parse a `--maxerror` value.  Returns false for anything that is not a plain
/// non-negative decimal integer, so a typo'd flag value fails loudly instead of
/// silently becoming 0 ("unlimited") and quietly changing what gets reported.
bool parse_error_limit(const std::string& text, uint32_t& out) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    try {
        unsigned long long value = std::stoull(text);
        if (value > std::numeric_limits<uint32_t>::max())
            return false;
        out = static_cast<uint32_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string_view severity_text(int severity) {
    switch (severity) {
    case 1:
        return "error";
    case 2:
        return "warning";
    case 4:
        return "hint";
    default:
        return "info";
    }
}

struct FlatDiag {
    std::string file;
    ParseDiagInfo diag;
};

void collect(std::vector<FlatDiag>& out, const std::string& fallback_uri,
            std::vector<ParseDiagInfo> diags) {
    for (auto& d : diags) {
        const std::string target_uri = d.uri.empty() ? fallback_uri : d.uri;
        out.push_back(FlatDiag{path_from_file_uri(target_uri), std::move(d)});
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string filelist_arg;
    std::string file_arg;
    bool lint_only = false;
    bool compile_only = false;
    uint32_t error_limit = kDefaultCompilationErrorLimit;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-f" || arg == "--filelist") {
            if (i + 1 >= argc) {
                print_usage();
                return 1;
            }
            filelist_arg = argv[++i];
        } else if (arg == "--maxerror" || arg == "--maxerrors") {
            if (i + 1 >= argc) {
                std::cerr << arg << " requires a value\n";
                print_usage();
                return 1;
            }
            const std::string value = argv[++i];
            if (!parse_error_limit(value, error_limit)) {
                std::cerr << "Invalid " << arg << " value: " << value
                          << " (expected a non-negative integer; 0 means unlimited)\n";
                return 1;
            }
        } else if (arg == "--lint-only") {
            lint_only = true;
        } else if (arg == "--compile-only") {
            compile_only = true;
        } else if (arg == "--version") {
            std::cout << "lazyverilog-lint " << LAZYVERILOG_VERSION << "\n";
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        } else {
            file_arg = arg;
        }
    }

    if (filelist_arg.empty() && file_arg.empty()) {
        print_usage();
        return 1;
    }

    std::string file_text;
    if (!file_arg.empty()) {
        std::ifstream f(file_arg);
        if (!f) {
            std::cerr << "Cannot open " << file_arg << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        file_text = ss.str();
    }

    const std::filesystem::path start = file_arg.empty()
                                            ? std::filesystem::current_path()
                                            : std::filesystem::absolute(file_arg).parent_path();
    CliProject project = resolve_cli_project(start, filelist_arg);

    Analyzer analyzer;
    index_cli_project(analyzer, project);

    std::string target_uri;
    if (!file_arg.empty()) {
        target_uri = uri_from_path(std::filesystem::absolute(file_arg));
        analyzer.open(target_uri, file_text);
    }

    analyzer.wait_for_background_index_idle();
    if (!lint_only)
        run_synchronous_semantic_compile(analyzer, project, error_limit);

    std::shared_ptr<const ProjectIndexSnapshot> project_lint_index;
    if (!compile_only && project.config.lint.instance.stale_instance_diagnostic)
        project_lint_index = analyzer.project_index_snapshot();

    std::vector<std::shared_ptr<const DocumentState>> states;
    if (!file_arg.empty()) {
        if (auto state = analyzer.get_state(target_uri))
            states.push_back(std::move(state));
    } else {
        states = analyzer.project_file_states_sync();
    }

    std::vector<FlatDiag> flat;
    for (const auto& state : states) {
        if (!state)
            continue;
        if (!lint_only) {
            collect(flat, state->uri, state->parse_diagnostics);
            collect(flat, state->uri, analyzer.semantic_diagnostics(state->uri));
        }
        if (!compile_only)
            collect(flat, state->uri,
                    run_lint(*state, project.config.lint, project_lint_index.get()));
    }

    std::sort(flat.begin(), flat.end(), [](const FlatDiag& a, const FlatDiag& b) {
        if (a.file != b.file)
            return a.file < b.file;
        if (a.diag.line != b.diag.line)
            return a.diag.line < b.diag.line;
        return a.diag.col < b.diag.col;
    });

    bool has_error = false;
    for (const auto& item : flat) {
        std::cout << item.file << ":" << (item.diag.line + 1) << ":" << (item.diag.col + 1) << ": "
                  << severity_text(item.diag.severity) << ": " << item.diag.message << "\n";
        if (item.diag.severity == 1)
            has_error = true;
    }

    return has_error ? 2 : 0;
}
