#include "banner.hpp"

#include "lazyverilog_ascii_logo.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <stdio.h>
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <iostream>

namespace {

/// True when the environment variable is set to anything other than "0".
///
/// An empty value counts as set: `LAZYVERILOG_NO_BANNER=` is how a shell
/// spells "I want this off" as often as `=1` is.
bool env_flag_set(const char* name) {
    const char* value = std::getenv(name);
    if (!value)
        return false;
    return std::strcmp(value, "0") != 0;
}

bool stderr_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

/// Terminal width in columns, or 0 when it cannot be determined.
///
/// `COLUMNS` is consulted first because it is the only thing a user can set to
/// correct a wrong answer -- inside `screen`, `tmux` or a pty with no window
/// size, the ioctl reports 0 and the logo would be dropped for a terminal that
/// is in fact wide enough.
int terminal_width() {
    if (const char* columns = std::getenv("COLUMNS")) {
        const int parsed = std::atoi(columns);
        if (parsed > 0)
            return parsed;
    }
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info)) {
        const int width = info.srWindow.Right - info.srWindow.Left + 1;
        if (width > 0)
            return width;
    }
#else
    struct winsize size {};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        return static_cast<int>(size.ws_col);
#endif
    return 0;
}

/// Turn on ANSI escape handling and report whether colour may be emitted.
///
/// Windows consoles need `ENABLE_VIRTUAL_TERMINAL_PROCESSING` switched on
/// explicitly; without it the escapes are printed literally, which is uglier
/// than no colour at all.  `NO_COLOR` (https://no-color.org) wins over both.
bool enable_color() {
    if (env_flag_set("NO_COLOR"))
        return false;
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode))
        return false;
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0 &&
        !SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return false;
    return true;
#else
    const char* term = std::getenv("TERM");
    return !term || std::strcmp(term, "dumb") != 0;
#endif
}

/// Width of the widest line in the embedded logo.
///
/// Measured rather than hardcoded so re-exporting `assets/ascii_logo.txt` at a
/// different size cannot leave the wrap threshold pointing at the old one.  The
/// art is pure ASCII, so bytes and columns are the same number.
int logo_width() {
    int widest = 0;
    int current = 0;
    for (const char* c = kLazyVerilogAsciiLogo; *c; ++c) {
        if (*c == '\n') {
            current = 0;
            continue;
        }
        if (*c == '\r')
            continue;
        if (++current > widest)
            widest = current;
    }
    return widest;
}

bool wants_unadorned_output(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--version") == 0 || std::strcmp(arg, "-h") == 0 ||
            std::strcmp(arg, "--help") == 0)
            return true;
    }
    return false;
}

/// Leading spaces that centre @p text_width columns inside @p field_width.
std::string centering_pad(int text_width, int field_width) {
    if (text_width >= field_width)
        return {};
    return std::string(static_cast<size_t>((field_width - text_width) / 2), ' ');
}

} // namespace

void print_startup_banner(std::string_view tool_name, int argc, char* argv[]) {
    static bool printed = false;
    if (printed)
        return;
    printed = true;

    if (env_flag_set("LAZYVERILOG_NO_BANNER") || wants_unadorned_output(argc, argv))
        return;

    // LAZYVERILOG_FORCE_BANNER is the only way to see this rendering without a
    // terminal: it is what the smoke test asserts against, and what a user can
    // set to get the version stamped into a captured CI log.  It does not
    // override the two switches above -- an explicit "off" stays off.
    if (!env_flag_set("LAZYVERILOG_FORCE_BANNER") && !stderr_is_terminal())
        return;

    const bool color = enable_color();
    const char* logo_style = color ? "\033[1;36m" : "";
    const char* name_style = color ? "\033[1m" : "";
    const char* version_style = color ? "\033[2m" : "";
    const char* reset = color ? "\033[0m" : "";

    const int width = logo_width();
    const int columns = terminal_width();

    // A wrapped 131-column drawing is worse than no drawing.  When the terminal
    // cannot hold it -- or will not say how wide it is -- fall back to the one
    // line that carries the same two facts.
    const std::string label = std::string(tool_name) + "  " + LAZYVERILOG_VERSION;
    if (columns < width) {
        std::cerr << "  " << name_style << tool_name << reset << "  " << version_style
                  << LAZYVERILOG_VERSION << reset << "\n\n";
        std::cerr.flush();
        return;
    }

    // Pad first, then style.  The escape sequences occupy no columns, so the
    // centering has to be computed from the plain label and the styling applied
    // inside the space it reserved.
    std::cerr << logo_style << kLazyVerilogAsciiLogo << reset;
    std::cerr << centering_pad(static_cast<int>(label.size()), width) << name_style << tool_name
              << reset << "  " << version_style << LAZYVERILOG_VERSION << reset << "\n\n";
    std::cerr.flush();
}
