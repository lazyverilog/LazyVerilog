#pragma once

// Small cross-platform helper for CLI smoke tests that shell out to a built
// binary and capture its stdout/stderr/exit code. Quoting and exit-status
// decoding mirror rtl_format_sweep.cpp's run_formatter(), which is the
// existing vetted pattern in this repo for driving a CLI binary from a CTest
// executable across Linux/macOS/Windows.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace cli_process {

namespace fs = std::filesystem;

inline int current_process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

inline std::string shell_quote(const fs::path& path) {
#ifdef _WIN32
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

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct CommandResult {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
};

/// Runs `binary` with `args` (a single already-quoted argument string, for
/// example `shell_quote(a) + " " + shell_quote(b)`) and captures its output.
inline CommandResult run_command(const fs::path& binary, const std::string& args) {
    static int counter = 0;
    const fs::path base =
        fs::temp_directory_path() / ("lazyverilog-cli-smoke-" +
                                     std::to_string(current_process_id()) + "-" +
                                     std::to_string(counter++));
    const fs::path stdout_path = base.string() + ".out";
    const fs::path stderr_path = base.string() + ".err";

    std::string command = shell_quote(binary);
    if (!args.empty())
        command += " " + args;
    command += " > " + shell_quote(stdout_path) + " 2> " + shell_quote(stderr_path);
#ifdef _WIN32
    // See rtl_format_sweep.cpp's run_formatter() for why the whole command
    // line needs an extra pair of outer quotes under cmd.exe.
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
        result.exit_code = status;
    }
#else
    else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
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

} // namespace cli_process
