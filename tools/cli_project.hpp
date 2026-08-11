#pragma once

#include "analyzer.hpp"
#include "background_compiler.hpp"
#include "config.hpp"

#include <filesystem>
#include <string>

/// Shared project bootstrap for the `lazyverilog-lint` and
/// `lazyverilog-rtltree` CLI tools.  Both need the same startup sequence the
/// LSP server runs on `initialize`: find `lazyverilog.toml`, load it, resolve
/// the `[design]` filelist, and index the resulting files into an Analyzer.
struct CliProject {
    Config config;
    std::filesystem::path root; // directory containing lazyverilog.toml, or `start` if none found
};

/// Walk up from `start` (a file or directory) for `lazyverilog.toml` and load
/// it (config defaults if not found). `filelist_override`, when non-empty,
/// replaces the config's `[design]` vcode path (as from a CLI `-f` flag).
CliProject resolve_cli_project(const std::filesystem::path& start,
                               const std::string& filelist_override);

/// Index `project`'s design filelist into `analyzer` as extra project files,
/// the same call the LSP server makes from its `initialize` handler. Indexing
/// runs on Analyzer's background workers; call
/// `analyzer.wait_for_background_index_idle()` before reading extra-file
/// derived facts (project index snapshot, rtl_tree(), etc).
void index_cli_project(Analyzer& analyzer, const CliProject& project);

/// Run one synchronous, blocking semantic-compile pass over `project` and
/// merge the result into `analyzer`'s semantic diagnostics cache. Reuses
/// BackgroundCompiler so output matches the LSP server's `lazyverilog.lintAll`
/// semantic diagnostics exactly. No-op if
/// `project.config.compilation.background_compilation` is false.
void run_synchronous_semantic_compile(Analyzer& analyzer, const CliProject& project);
