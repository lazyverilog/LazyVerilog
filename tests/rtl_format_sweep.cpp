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
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static volatile std::sig_atomic_t interrupted = 0;

static void handle_signal(int) {
    interrupted = 1;
}

static int current_process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

struct Failure {
    fs::path path;
    std::string kind;
    std::string message;
};

struct SlowFile {
    fs::path path;
    long long elapsed_ms{0};
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
#ifdef _WIN32
    // `std::system()` on Windows runs through cmd.exe.  Double quotes preserve
    // spaces in paths; embedded double quotes are doubled to keep the generated
    // command line well-formed for the simple file paths used by this sweep.
    std::string result = "\"";
    for (char c : path.string()) {
        if (c == '"')
            result += "\"\"";
        else
            result += c;
    }
    result += "\"";
    return result;
#else
    std::string result = "'";
    for (char c : path.string()) {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }
    result += "'";
    return result;
#endif
}

static CommandResult run_formatter(const fs::path& formatter, const fs::path& input) {
    const fs::path base = fs::temp_directory_path() /
                          ("lazyverilog-format-sweep-" + std::to_string(current_process_id()) + "-" +
                           std::to_string(reinterpret_cast<std::uintptr_t>(&input)));
    const fs::path stdout_path = base.string() + ".out";
    const fs::path stderr_path = base.string() + ".err";

    std::string command = shell_quote(formatter) + " " + shell_quote(input) + " > " +
                          shell_quote(stdout_path) + " 2> " + shell_quote(stderr_path);
#ifdef _WIN32
    // cmd.exe strips the outer quote pair whenever a command line both starts
    // and ends with '"' (its heuristic for "the whole thing is one quoted
    // program path"). Our command starts with the quoted formatter path and
    // ends with the quoted stderr path, so it hits that rule and cmd drops the
    // wrong quotes, breaking the trailing redirection ("The filename,
    // directory name, or volume label syntax is incorrect."). Wrapping the
    // whole line in one more quote pair gives cmd's heuristic that outer pair
    // to strip instead, leaving the real argument quoting intact.
    command = "\"" + command + "\"";
#endif
    const int status = std::system(command.c_str());

    CommandResult result;
    if (status == -1) {
        result.exit_code = 127;
        result.stderr_text = std::strerror(errno);
    }
#ifdef _WIN32
    else {
        // MSVC's system() returns the command interpreter's exit code directly,
        // unlike POSIX where the return value encodes wait status bits.
        result.exit_code = status;
        result.interrupted = result.exit_code == 130;
    }
#else
    else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.interrupted = result.exit_code == 130;
    } else if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        result.exit_code = 128 + sig;
        result.interrupted = sig == SIGINT || sig == SIGTERM;
    } else {
        result.exit_code = 128;
    }
#endif

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
    return source.parent_path() / (".__lazyverilog_format_sweep_" +
                                   std::to_string(current_process_id()) +
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
    std::cerr << "usage: " << argv0 << " [--perf] <formatter-binary> <rtl-root>\n";
    std::cerr << "  --perf    print per-file timing and report files taking more than 1000ms\n";
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    bool perf = false;
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--perf") == 0) {
            perf = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            std::cerr << "warning: invalid option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 2;
        } else {
            args.emplace_back(argv[i]);
        }
    }

    if (args.size() != 2) {
        std::cerr << "warning: expected <formatter-binary> and <rtl-root>\n";
        print_usage(argv[0]);
        return 2;
    }

    const fs::path formatter = args[0];
    const fs::path root = args[1];
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
    std::vector<SlowFile> slow_files;
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
        std::chrono::steady_clock::time_point file_start;
        if (perf)
            file_start = std::chrono::steady_clock::now();
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
        std::cout << "  [" << (i + 1) << "/" << files.size() << "] pass=" << passed
                  << " fail=" << failed << " not-idempotent=" << not_idempotent_count;
        if (perf) {
            const auto file_end = std::chrono::steady_clock::now();
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(file_end - file_start).count();
            if (elapsed_ms > 1000)
                slow_files.push_back({file, elapsed_ms});
            std::cout << " time=" << elapsed_ms << "ms";
        }
        std::cout << " " << file << "\n" << std::flush;
    }

    if (perf) {
        std::cout << "  slow >1000ms: " << slow_files.size() << "\n";
        for (const auto& slow : slow_files)
            std::cout << "slow: " << slow.elapsed_ms << "ms: " << slow.path << "\n";
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
