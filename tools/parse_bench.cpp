#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Options {
    fs::path filelist;
    fs::path verible_root;
    fs::path verible_bin;
    bool build_verible{false};
    bool ignore_missing{false};
};

std::string shell_quote(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::vector<std::string> split_filelist_line(const std::string& line) {
    std::vector<std::string> tokens;
    std::string token;
    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) && !in_single && !in_double) {
            if (!token.empty()) {
                tokens.push_back(std::move(token));
                token.clear();
            }
            continue;
        }
        token += c;
    }
    if (!token.empty())
        tokens.push_back(std::move(token));
    return tokens;
}

std::vector<fs::path> parse_filelist(const fs::path& filelist) {
    std::ifstream input(filelist);
    if (!input)
        throw std::runtime_error("failed to open filelist: " + filelist.string());

    std::vector<fs::path> files;
    const auto base = fs::absolute(filelist).parent_path();
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find("//");
        if (comment != std::string::npos)
            line.erase(comment);

        for (const auto& item : split_filelist_line(line)) {
            if (item.empty() || item.starts_with("+") || item.starts_with("-"))
                continue;
            fs::path path(item);
            if (path.is_relative())
                path = base / path;
            files.push_back(fs::absolute(path).lexically_normal());
        }
    }
    return files;
}

fs::path expand_home(fs::path path) {
    auto text = path.string();
    if (text == "~" || text.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home)
            return fs::path(home) / text.substr(text == "~" ? 1 : 2);
    }
    return path;
}

std::optional<fs::path> find_verible_bin(const Options& options) {
    if (!options.verible_bin.empty())
        return expand_home(options.verible_bin);

    const auto root = expand_home(options.verible_root);
    const auto candidate = root / "bazel-bin/verible/verilog/tools/syntax/verible-verilog-syntax";
    if (fs::exists(candidate))
        return candidate;

    if (!options.build_verible)
        return std::nullopt;

    const std::string command =
        "cd " + shell_quote(root.string()) +
        " && bazel build //verible/verilog/tools/syntax:verible-verilog-syntax";
    if (std::system(command.c_str()) != 0)
        throw std::runtime_error("failed to build verible-verilog-syntax");
    return candidate;
}

std::string make_verible_command(const fs::path& verible_bin, const std::vector<fs::path>& files,
                                 size_t begin, size_t end) {
    std::string command = shell_quote(verible_bin.string()) + " --lang=sv";
    for (size_t i = begin; i < end; ++i)
        command += " " + shell_quote(files[i].string());
    command += " >/dev/null 2>/dev/null";
    return command;
}

struct VeribleResult {
    double seconds;
    size_t batches;
    size_t failed_batches;
    int last_status;
};

VeribleResult run_verible(const fs::path& verible_bin, const std::vector<fs::path>& files) {
    constexpr size_t batch_size = 128;
    size_t failed_batches = 0;
    int last_status = 0;

    const auto start = Clock::now();
    size_t batches = 0;
    for (size_t begin = 0; begin < files.size(); begin += batch_size) {
        const size_t end = std::min(begin + batch_size, files.size());
        const int status =
            std::system(make_verible_command(verible_bin, files, begin, end).c_str());
        ++batches;
        if (status != 0) {
            ++failed_batches;
            last_status = status;
        }
    }

    return {seconds_since(start), batches, failed_batches, last_status};
}

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " <filelist.f> [--verible-root ~/dev/verible]"
                 " [--verible-bin PATH] [--build-verible] [--ignore-missing]\n";
}

Options parse_args(int argc, char** argv) {
    Options options;
    options.verible_root = "~/dev/verible";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verible-root" && i + 1 < argc) {
            options.verible_root = argv[++i];
        } else if (arg == "--verible-bin" && i + 1 < argc) {
            options.verible_bin = argv[++i];
        } else if (arg == "--build-verible") {
            options.build_verible = true;
        } else if (arg == "--ignore-missing") {
            options.ignore_missing = true;
        } else if (arg.starts_with("--")) {
            throw std::runtime_error("unknown option: " + arg);
        } else if (options.filelist.empty()) {
            options.filelist = arg;
        } else {
            throw std::runtime_error("unexpected argument: " + arg);
        }
    }

    if (options.filelist.empty())
        throw std::runtime_error("missing filelist");
    return options;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        print_usage(argv[0]);
        return 2;
    }

    try {
        auto files = parse_filelist(options.filelist);
        std::vector<fs::path> existing;
        for (const auto& file : files) {
            if (fs::exists(file)) {
                existing.push_back(file);
            } else {
                std::cerr << "missing file: " << file << "\n";
                if (!options.ignore_missing)
                    return 2;
            }
        }
        files = std::move(existing);
        if (files.empty()) {
            std::cerr << "no source files to parse\n";
            return 2;
        }

        std::cout << "filelist: " << fs::absolute(options.filelist).lexically_normal() << "\n";
        std::cout << "files:    " << files.size() << "\n";

        size_t slang_ok = 0;
        size_t slang_failed = 0;
        slang::SourceManager source_manager;
        auto slang_start = Clock::now();
        for (const auto& file : files) {
            auto tree = slang::syntax::SyntaxTree::fromFile(file.string(), source_manager);
            if (tree)
                ++slang_ok;
            else
                ++slang_failed;
        }
        const double slang_seconds = seconds_since(slang_start);
        std::cout << "lazyverilog/slang: " << slang_seconds << "s"
                  << " parsed=" << slang_ok << " failed=" << slang_failed << "\n";

        auto verible_bin = find_verible_bin(options);
        if (!verible_bin) {
            std::cout << "verible: unavailable; pass --verible-bin or --build-verible\n";
            return 0;
        }

        const auto verible = run_verible(*verible_bin, files);
        std::cout << "verible:           " << verible.seconds << "s"
                  << " batches=" << verible.batches << " failed_batches=" << verible.failed_batches
                  << " last_status=" << verible.last_status << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
