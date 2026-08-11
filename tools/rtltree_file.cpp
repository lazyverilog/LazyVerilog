#include "cli_project.hpp"
#include "string_utils.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void print_usage() {
    std::cerr << "Usage: lazyverilog-rtltree [-f <filelist>] [--reverse] <file>\n";
}

void print_tree(const RtlTreeNode& node, const CliProject& project, int depth, std::ostream& out) {
    out << std::string(static_cast<size_t>(depth) * 2, ' ') << node.name;
    if (project.config.rtltree.show_instance_name && !node.inst.empty())
        out << " (" << node.inst << ")";
    if (project.config.rtltree.show_file && !node.file.empty())
        out << " [" << path_from_file_uri(node.file) << "]";
    if (node.recursive)
        out << " (recursive)";
    if (node.truncated)
        out << " (truncated)";
    out << "\n";
    for (const auto& child : node.children)
        print_tree(child, project, depth + 1, out);
}

} // namespace

int main(int argc, char* argv[]) {
    std::string filelist_arg;
    std::string file_arg;
    bool reverse = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-f" || arg == "--filelist") {
            if (i + 1 >= argc) {
                print_usage();
                return 1;
            }
            filelist_arg = argv[++i];
        } else if (arg == "--reverse") {
            reverse = true;
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

    if (file_arg.empty()) {
        print_usage();
        return 1;
    }

    std::ifstream f(file_arg);
    if (!f) {
        std::cerr << "Cannot open " << file_arg << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    const std::filesystem::path start = std::filesystem::absolute(file_arg).parent_path();
    CliProject project = resolve_cli_project(start, filelist_arg);

    Analyzer analyzer;
    index_cli_project(analyzer, project);

    const std::string target_uri = uri_from_path(std::filesystem::absolute(file_arg));
    analyzer.open(target_uri, ss.str());
    analyzer.wait_for_background_index_idle();

    auto tree = reverse ? analyzer.rtl_tree_reverse(target_uri) : analyzer.rtl_tree(target_uri);
    if (!tree) {
        std::cerr << "No module found in " << file_arg << "\n";
        return 1;
    }

    print_tree(*tree, project, 0, std::cout);
    return 0;
}
