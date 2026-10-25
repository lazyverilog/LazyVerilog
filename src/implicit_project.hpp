#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The implicit project: what a folder with no lazyverilog.toml is indexed as.
///
/// A file no lazyverilog.toml governs still sits in a tree the user opened --
/// a workspace folder, or failing that a version-controlled checkout.  Scanning
/// that tree for SystemVerilog sources and indexing them is what lets
/// go-to-definition, hover and the RTL tree reach a closed file without the
/// user writing a config first.
///
/// What it does not do is guess anything the "no config, never guess" rule
/// protects: an implicit project is served built-in defaults, parses with no
/// defines, writes its shards to the user's cache rather than into the tree,
/// and is never compiled.  A lazyverilog.toml anywhere at or above a file
/// always wins over it.
enum class ImplicitProjectMode {
    Off,       ///< No implicit projects; files under no toml stay in none.
    Workspace, ///< Client workspace folders only.
    Auto,      ///< Workspace folders, else the nearest VCS root of an open file.
};

/// "off" / "workspace" / "auto", or nullopt for anything else.
std::optional<ImplicitProjectMode> parse_implicit_project_mode(std::string_view text);
std::string_view implicit_project_mode_name(ImplicitProjectMode mode);

/// Bounds on one scan.  Crossing any of them gives up on the whole root rather
/// than indexing a truncated set: a project that finds some files and not
/// others, depending on directory order, is worse than one that finds none and
/// says so.
struct ImplicitScanLimits {
    /// opentitan's whole tree (about 4000 sources) fits, at roughly 1 GB peak
    /// while it indexes cold.
    size_t max_source_files = 5000;
    /// Every directory entry looked at, sources or not.
    size_t max_dir_entries = 300000;
    std::chrono::milliseconds max_time{5000};
    /// Directories on the include path.  Every `include slang cannot find next
    /// to its includer is looked for in each of them, so this one truncates
    /// (in sorted order) rather than giving up; tests/rtl's seven designs
    /// together have 182.
    size_t max_include_dirs = 256;
};

struct ImplicitScan {
    /// `.sv` / `.v` sources, normalized and sorted by path.
    std::vector<std::string> files;
    /// Byte size of each entry in `files`; a scheduling hint, as for a filelist.
    std::vector<uintmax_t> file_sizes;
    /// Every directory holding a `.svh` / `.vh` / `.svi`, sorted.  The include
    /// path this project parses with.
    std::vector<std::string> include_dirs;
    /// Set when a limit was crossed; `files` and `include_dirs` are then empty.
    bool over_limit = false;
    /// Which limit, for the message the user sees.
    std::string over_limit_reason;
};

/// Scan @p root for SystemVerilog sources.
///
/// Skipped: directories whose name starts with '.', the build and simulator
/// output directories every flow litters a checkout with, symlinked
/// directories, any subtree holding its own lazyverilog.toml (that subtree is
/// an explicit project), directory names and `*.ext` patterns from
/// `.gitignore` files, and paths matching @p exclude (globs relative to
/// @p root, `*`, `?` and `**`).
ImplicitScan scan_implicit_root(const std::filesystem::path& root,
                                const std::vector<std::string>& exclude = {},
                                const ImplicitScanLimits& limits = {});

/// Glob match of @p path (relative, '/'-separated) against @p pattern: `*` and
/// `?` stop at '/', `**` crosses it.
bool implicit_glob_match(std::string_view pattern, std::string_view path);
