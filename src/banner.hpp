#pragma once

#include <string_view>

/// Print the lazyverilog ASCII logo and version to **stderr**, once per process.
///
/// stderr and not stdout, for every binary: `lazyverilog-lsp` speaks JSON-RPC on
/// stdout, `lazyverilog-fmt` writes formatted source there, and
/// `lazyverilog-lint` / `lazyverilog-rtltree` write results a caller greps.  A
/// banner on stdout would corrupt all four.
///
/// The banner is suppressed unless stderr is a terminal, so a piped, redirected
/// or editor-spawned run is byte-for-byte what it was before.  It is also
/// suppressed when `LAZYVERILOG_NO_BANNER` is set to anything but `0`, and when
/// @p argv carries `--version`, `-h` or `--help` -- those are the
/// machine-readable paths and stay unadorned.

/// `LAZYVERILOG_FORCE_BANNER` prints it even when stderr is not a terminal, so
/// the rendering is assertable from a test and can be stamped into a captured
/// CI log.  It does not override `LAZYVERILOG_NO_BANNER` or `--version`.
///
/// @p tool_name is the binary's own name, e.g. `"lazyverilog-lint"`.
void print_startup_banner(std::string_view tool_name, int argc, char* argv[]);
