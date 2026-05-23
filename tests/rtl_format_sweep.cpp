#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static volatile std::sig_atomic_t interrupted = 0;

static void handle_signal(int) {
    interrupted = 1;
}

struct Failure {
    fs::path path;
    std::string kind;
    std::string message;
};

struct CommandResult {
    int exit_code{0};
    bool interrupted{false};
    std::string stdout_text;
    std::string stderr_text;
};

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + path.string());
    out << text;
}

static std::string shell_quote(const fs::path& path) {
    std::string result = "'";
    for (char c : path.string()) {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }
    result += "'";
    return result;
}

static CommandResult run_formatter(const fs::path& formatter, const fs::path& input) {
    const fs::path base = fs::temp_directory_path() /
                          ("lazyverilog-format-sweep-" + std::to_string(getpid()) + "-" +
                           std::to_string(reinterpret_cast<std::uintptr_t>(&input)));
    const fs::path stdout_path = base.string() + ".out";
    const fs::path stderr_path = base.string() + ".err";

    const std::string command = shell_quote(formatter) + " " + shell_quote(input) + " > " +
                                shell_quote(stdout_path) + " 2> " + shell_quote(stderr_path);
    const int status = std::system(command.c_str());

    CommandResult result;
    if (status == -1) {
        result.exit_code = 127;
        result.stderr_text = std::strerror(errno);
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.interrupted = result.exit_code == 130;
    } else if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        result.exit_code = 128 + sig;
        result.interrupted = sig == SIGINT || sig == SIGTERM;
    } else {
        result.exit_code = 128;
    }

    if (fs::exists(stdout_path)) {
        result.stdout_text = read_file(stdout_path);
        fs::remove(stdout_path);
    }
    if (fs::exists(stderr_path)) {
        result.stderr_text = read_file(stderr_path);
        fs::remove(stderr_path);
    }
    return result;
}

static fs::path make_temp_source_path(const fs::path& source) {
    static int counter = 0;
    return source.parent_path() / (".__lazyverilog_format_sweep_" + std::to_string(getpid()) +
                                   "_" + std::to_string(counter++) + source.extension().string());
}

static bool is_sv_file(const fs::path& path) {
    const auto ext = path.extension().string();
    return ext == ".sv" || ext == ".svh";
}

static std::vector<fs::path> collect_sv_files(const fs::path& root) {
    std::vector<fs::path> files;
    for (const auto& entry :
         fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && is_sv_file(entry.path()))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

static void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " <formatter-binary> <rtl-root>\n";
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (argc != 3) {
        print_usage(argv[0]);
        return 2;
    }

    const fs::path formatter = argv[1];
    const fs::path root = argv[2];
    if (!fs::exists(formatter)) {
        std::cerr << "Formatter binary does not exist: " << formatter << "\n";
        return 2;
    }
    if (!fs::exists(root)) {
        std::cerr << "RTL root does not exist: " << root << "\n";
        return 2;
    }

    std::vector<Failure> failures;
    std::vector<Failure> not_idempotent;
    const auto files = collect_sv_files(root);
    if (files.empty()) {
        std::cerr << "No .sv or .svh files found under " << root << "\n";
        return 2;
    }

    std::cout << "RTL formatter sweep\n"
              << "  formatter: " << formatter << "\n"
              << "  root: " << root << "\n"
              << "  files: " << files.size() << "\n"
              << std::flush;

    int passed = 0;
    int failed = 0;
    int not_idempotent_count = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (interrupted) {
            std::cout << "  interrupted\n";
            return 130;
        }

        const auto& file = files[i];
        const auto file_start = std::chrono::steady_clock::now();
        bool file_failed = false;
        bool file_not_idempotent = false;
        try {
            const CommandResult once = run_formatter(formatter, file);
            if (once.interrupted) {
                std::cout << "  interrupted\n";
                return 130;
            } else if (once.exit_code != 0) {
                failures.push_back({file, "fail", once.stderr_text});
                file_failed = true;
            } else {
                const fs::path temp = make_temp_source_path(file);
                write_file(temp, once.stdout_text);
                const CommandResult twice = run_formatter(formatter, temp);
                fs::remove(temp);

                if (twice.interrupted) {
                    std::cout << "  interrupted\n";
                    return 130;
                } else if (twice.exit_code != 0) {
                    failures.push_back({file, "fail", twice.stderr_text});
                    file_failed = true;
                } else if (twice.stdout_text != once.stdout_text) {
                    not_idempotent.push_back(
                        {file, "not-idempotent", "second format changed formatter output"});
                    file_not_idempotent = true;
                }
            }
        } catch (const std::exception& e) {
            failures.push_back({file, "fail", e.what()});
            file_failed = true;
        }

        if (file_failed)
            ++failed;
        else if (file_not_idempotent)
            ++not_idempotent_count;
        else
            ++passed;
        const auto file_end = std::chrono::steady_clock::now();
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(file_end - file_start).count();
        std::cout << "  [" << (i + 1) << "/" << files.size() << "] pass=" << passed
                  << " fail=" << failed << " not-idempotent=" << not_idempotent_count << " "
                  << "time=" << elapsed_ms << "ms " << file << "\n";
    }

    if (failures.empty() && not_idempotent.empty()) {
        std::cout << "  result: pass\n";
        return 0;
    }

    std::cout << "  result: fail\n"
              << "  fail: " << failures.size() << "\n"
              << "  not-idempotent: " << not_idempotent.size() << "\n";
    for (const auto& failure : failures)
        std::cout << failure.kind << ": " << failure.path << ": " << failure.message << "\n";
    for (const auto& failure : not_idempotent)
        std::cout << failure.kind << ": " << failure.path << ": " << failure.message << "\n";
    return 1;
}
