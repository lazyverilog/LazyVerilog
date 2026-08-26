#include "cli_project.hpp"
#include "document_state.hpp"
#include "features/lint.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: lazyverilog-lint [-f <filelist>] [--lint-only] [--compile-only] "
                  "[--version] [<file>]\n";
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-f" || arg == "--filelist") {
            if (i + 1 >= argc) {
                print_usage();
                return 1;
            }
            filelist_arg = argv[++i];
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
        run_synchronous_semantic_compile(analyzer, project);

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
