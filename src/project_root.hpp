#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

/// Which project a file belongs to, decided by the server rather than by the
/// editor.
///
/// This mirrors clangd's `DirectoryBasedGlobalCompilationDatabase`, which walks
/// a file's parent directories looking for `compile_commands.json` and answers
/// `getProjectInfo(File)` with the directory it found one in.  Here the marker
/// is `lazyverilog.toml`, and the answer decides two things: which config the
/// file is served with, and which directory its index shards live in.
///
/// Why the server and not the client: an editor's root is a guess made before
/// any file is open, from a marker list it happens to be configured with.
/// Neovim's `vim.fs.root` resolves that list *by marker order, not proximity*,
/// so a `.git` higher in the tree wins over a nearer `lazyverilog.toml` and the
/// config the user wrote is never read.  The file itself is the only input that
/// cannot be wrong about which project it is in.
struct ProjectInfo {
    /// Directory containing the `lazyverilog.toml` that governs the file.
    std::filesystem::path source_root;

    bool operator==(const ProjectInfo&) const = default;
};

/// Resolves a file to its project root, with the per-directory caching that
/// makes an upward walk affordable on a hot path.
///
/// Thread-safe: background index workers resolve files concurrently with the
/// request threads.
class ProjectRootResolver {
public:
    /// Name of the marker file.  A directory containing it is a project root.
    static constexpr const char* kMarker = "lazyverilog.toml";

    ProjectRootResolver() = default;

    /// Restrict every lookup to @p directory, the way clangd's
    /// `--compile-commands-dir` bypasses the ancestor walk.  An empty path
    /// restores searching.
    void set_forced_root(std::filesystem::path directory);

    /// The project @p file belongs to, or nullopt when no `lazyverilog.toml`
    /// exists in it or any ancestor.
    ///
    /// Callers must handle nullopt rather than substituting a root of their
    /// own: "this file is in no project" is a real answer, and the shard cache
    /// answers it with a fallback directory outside the tree.  @p file may be
    /// a file or a directory, and need not exist.
    std::optional<ProjectInfo> project_info(const std::filesystem::path& file) const;

    /// Drop every cached decision.  Called when a `lazyverilog.toml` is created
    /// or deleted, where waiting out the freshness window would serve a root
    /// the user can see is wrong.
    void invalidate() const;

private:
    /// What one directory was last seen to hold, and when it was looked at.
    ///
    /// A miss is cached as deliberately as a hit.  A file five directories deep
    /// in a project rooted at the top stats five directories per lookup, and
    /// every open buffer, every indexed file and every request repeats it; the
    /// misses are most of that walk.  Without negative caching this is the
    /// cheapest way to make a deep tree feel slow on a shared filesystem.
    struct DirectoryCache {
        bool has_marker{false};
        std::chrono::steady_clock::time_point checked_at{};
    };

    /// How long a cached decision is served before the directory is stat'ed
    /// again.
    ///
    /// clangd keeps two windows for this and so do we: a directory that *has*
    /// the marker is the steady state and rechecking it buys nothing, while a
    /// directory that does not is exactly where a newly written config would
    /// appear.  Neither window is the mechanism that makes a new config take
    /// effect -- the client's watcher fires `invalidate()` for that -- they are
    /// the backstop for a client that reports nothing.
    static constexpr std::chrono::seconds kFreshFound{30};
    static constexpr std::chrono::seconds kFreshMissing{5};

    bool directory_has_marker(const std::filesystem::path& directory) const;

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, DirectoryCache> directories_;
    std::filesystem::path forced_root_;
};

/// Base directory for caches that do not belong to any project, following the
/// platform's own convention the way LLVM's `llvm::sys::path::cache_directory()`
/// does:
///
///   * Linux and other Unix: `$XDG_CACHE_HOME`, else `$HOME/.cache`
///   * macOS:                `$HOME/Library/Caches`
///   * Windows:              `%LOCALAPPDATA%`
///
/// Returns nullopt when the environment says nothing about where the user's
/// cache lives, which is a normal condition (a daemon with no HOME) and not an
/// error: the caller runs uncached.
std::optional<std::filesystem::path> user_cache_directory();
