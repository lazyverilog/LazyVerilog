#pragma once
#include "document_state.hpp"
#include "syntax_index.hpp"
#include <condition_variable>
#include <filesystem>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SymbolInfo {
    std::string name;
    std::string kind; // module, port, signal, instance, etc.
    std::string detail;
    std::string doc;
    int line{-1};
    int col{-1};
};

struct Location {
    std::string uri;
    int line{0};
    int col{0};
    int end_line{0};
    int end_col{0};
};

struct IdentifierAtPosition {
    std::string name;
    int line{0};
    int col{0};
    int end_col{0};
};

struct ExtraFileInfo {
    std::string path;
    std::string uri;
    // Non-null only when this file is currently open in the editor.  Closed
    // project files must not keep DocumentState / SyntaxTree alive; they are
    // represented by `index` only.
    std::shared_ptr<const DocumentState> state;
    SyntaxIndex index;
};

struct ExtraIndexInfo {
    std::string path;
    std::string uri;
    SyntaxIndex index;
};

struct CompilationSourceFile {
    std::string uri;
    std::string path;
    std::optional<std::string> text;
};

struct CompilationSnapshot {
    std::vector<CompilationSourceFile> files;
    std::vector<std::string> defines;
    std::vector<std::string> include_dirs;
    std::vector<std::string> open_uris;
};

struct RtlTreeNode {
    std::string name;
    std::string inst;
    std::string file;
    std::vector<RtlTreeNode> children;
    bool recursive{false};
    bool truncated{false};
};

class Analyzer {
  public:
    Analyzer() = default;
    ~Analyzer();

    /// Create a new DocumentState for uri with the given text.
    void open(const std::string& uri, const std::string& text);

    /// Update text for uri, creating a new immutable DocumentState snapshot.
    void change(const std::string& uri, const std::string& text);

    /// Remove document from cache.
    void close(const std::string& uri);

    /// Return a snapshot (or nullptr if not open).
    std::shared_ptr<const DocumentState> get_state(const std::string& uri) const;

    /// Find symbol at (line, col) using SyntaxTree only.
    std::optional<SymbolInfo> symbol_at(const std::string& uri, int line, int col) const;

    /// Resolve definition location for symbol at (line, col).
    std::optional<Location> definition_of(const std::string& uri, int line, int col) const;

    /// Return the identifier token under a position using the parsed SyntaxTree.
    std::optional<IdentifierAtPosition> identifier_at(const std::string& uri, int line,
                                                      int col) const;

    /// Find references to the symbol under a position by walking SyntaxTrees and
    /// verifying candidate tokens resolve to the same definition.
    std::vector<Location> find_references(const std::string& uri, int line, int col,
                                          bool include_declaration = true) const;

    /// Return all (0-based line, 0-based col) positions where `name` appears
    /// as a whole identifier in the document.
    std::vector<std::pair<int, int>> find_occurrences(const std::string& uri,
                                                      const std::string& name) const;

    /// Call f(uri, state) for every open document (under lock).
    template <typename F> void for_each_state(F&& f) const {
        std::lock_guard<std::mutex> lk(map_mutex_);
        for (const auto& [uri, state] : docs_)
            f(uri, state);
    }

    /// Set extra source files from the configured .f filelist.
    ///
    /// LazyVerilog indexes only explicit source-file entries.  +incdir+
    /// entries parsed from the same filelist are handled separately via
    /// set_include_dirs(); they are include search paths, not source files.
    ///
    /// @param filelist_path  Resolved absolute path to the .f file itself (may be empty).
    ///                       Used to detect filelist changes with a single stat() per request
    ///                       instead of stat()-ing every individual extra file.
    void set_extra_files(const std::vector<std::string>& paths,
                         const std::string& filelist_path = {});

    /// Apply all project-parse inputs from one loaded configuration in a single
    /// analyzer transaction.
    ///
    /// This is preferred by the LSP server when loading or reloading
    /// lazyverilog.toml because defines, include directories, and filelist
    /// entries all affect project shards.  Updating them independently can
    /// schedule multiple full-project background reindex generations for a
    /// single user-visible config change.  This batched setter clears the old
    /// project cache once and schedules at most one asynchronous reindex.
    void set_project_config(const std::vector<std::string>& defines,
                            const std::vector<std::string>& include_dirs,
                            const std::vector<std::string>& extra_files,
                            const std::string& filelist_path = {});

    /// Block until all currently queued project-index work is published.
    ///
    /// Production LSP request paths should not call this: project files are
    /// intentionally indexed asynchronously.  Tests and command-line utilities
    /// that need deterministic assertions after set_extra_files() may wait for
    /// the background worker to become idle.
    void wait_for_background_index_idle() const;

    /// Set preprocessor defines (from config.design.define).
    /// Applied on every subsequent make_state call.
    void set_defines(const std::vector<std::string>& defines);

    /// Set include directories parsed from +incdir+ entries in the design filelist.
    ///
    /// They are given to slang's SourceManager so `include "..." directives
    /// inside opened files and explicit filelist sources can resolve library
    /// headers without parsing every header as a separate top-level extra file.
    void set_include_dirs(const std::vector<std::string>& include_dirs);

    /// Return extra files from .f filelist.
    std::vector<std::string> extra_files() const;

    /// Refresh project-index shards for files that the client reports changed.
    ///
    /// This is intentionally event-driven: callers pass the exact changed URIs
    /// from workspace/didChangeWatchedFiles or an equivalent client-side edit
    /// hook.  The analyzer does not scan the workspace, poll mtimes, or stat
    /// every configured file, which keeps shared/HPC filesystems off normal
    /// request paths.
    void refresh_changed_extra_files(const std::vector<std::string>& changed_uris,
                                     const std::vector<std::string>& deleted_uris = {});

    /// Return project-file index shards.
    ///
    /// Historical note: this used to return cached closed-file DocumentState
    /// objects as well.  It now keeps DocumentState only for open buffers, so
    /// callers must treat `state` as optional and use `index` for closed files.
    std::vector<ExtraFileInfo> extra_file_snapshots() const;

    /// Return per-file project index shards without exposing full SyntaxTrees.
    ///
    /// Cross-file features should prefer this over extra_file_snapshots() when
    /// they only need indexed structural data.  This supports the indexing
    /// philosophy: current file uses AST, project files use index.
    std::vector<ExtraIndexInfo> extra_index_snapshots() const;

    /// Append cached extra-file modules to an existing SyntaxIndex.
    void merge_extra_file_modules(SyntaxIndex& index) const;

    /// Return the last background-published project-wide index snapshot.
    ///
    /// The request path never merges per-file shards.  The background worker
    /// parses/replaces shards and publishes an immutable merged SyntaxIndex
    /// snapshot.  While the worker is still catching up, callers see the last
    /// published snapshot (or an empty one before the first publish).
    std::shared_ptr<const SyntaxIndex> extra_project_index() const;

    /// Register a callback fired whenever the merged project index snapshot is
    /// republished.  The callback may run on the background indexer thread, so
    /// it must be non-blocking and must not call back into Analyzer.
    void set_project_index_publish_callback(std::function<void()> callback);

    /// Return a merged dynamic/file index for other open buffers.
    ///
    /// This is the clangd "dynamic" layer: files that have been opened/parsed
    /// during this editor session, including unsaved edits.  The current file
    /// is excluded because completion/point queries inspect its SyntaxTree
    /// directly instead of materializing a current-file index.
    std::shared_ptr<const SyntaxIndex> opened_files_index(const std::string& current_uri) const;

    /// Return an immutable snapshot for background semantic compilation.
    /// Open documents include their in-memory text; filelist-only documents
    /// include only paths and are read by the background worker.
    CompilationSnapshot compilation_snapshot() const;

    /// Replace cached semantic diagnostics from the background compiler.
    void set_semantic_diagnostics(
        std::unordered_map<std::string, std::vector<ParseDiagInfo>> diagnostics);

    /// Drop cached semantic diagnostics for one URI because its text changed.
    void clear_semantic_diagnostics(const std::string& uri);

    /// Drop all cached semantic diagnostics, e.g. when background compilation is disabled.
    void clear_all_semantic_diagnostics();

    /// Return cached semantic diagnostics for one URI.
    std::vector<ParseDiagInfo> semantic_diagnostics(const std::string& uri) const;

    /// Return URIs that currently have cached semantic diagnostics.
    std::vector<std::string> semantic_diagnostic_uris() const;

    /// Build a forward RTL instantiation tree rooted at the first module in uri.
    std::optional<RtlTreeNode> rtl_tree(const std::string& uri) const;

    /// Build a reverse RTL instantiation tree: modules that instantiate uri's first module.
    std::optional<RtlTreeNode> rtl_tree_reverse(const std::string& uri) const;

  private:
    std::shared_ptr<DocumentState> make_state(const std::string& uri,
                                              const std::string& text) const;
    std::shared_ptr<DocumentState> make_file_state(const std::filesystem::path& path) const;
    std::optional<Location>
    definition_of_state(const DocumentState& state, const std::string& uri, int line, int col,
                        const std::vector<ExtraFileInfo>& extra_files) const;

    struct ExtraFileCacheEntry {
        std::string path;
        std::string uri;
        SyntaxIndex index;
    };

    void start_background_indexer_locked() const;
    void schedule_background_reindex_locked() const;
    void schedule_background_project_publish_locked() const;
    void background_index_loop(std::stop_token stop) const;
    void publish_extra_project_index_locked() const;
    void clear_extra_project_index_locked() const;
    void update_extra_cache_for_live_state_locked(std::shared_ptr<const DocumentState> state,
                                                  SyntaxIndex index);

    // Resolved .f filelist path.  We intentionally do not poll this file's
    // mtime on LSP requests: HPC projects usually do not edit filelists while
    // the editor is alive, and even one metadata operation per request is still
    // unwanted noise on shared filesystems.  Configuration reloads call
    // set_extra_files() explicitly when the filelist should be re-read.
    mutable std::string filelist_path_;

    mutable std::mutex map_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<const DocumentState>> docs_;
    std::vector<std::string> defines_;
    std::vector<std::string> include_dirs_;
    mutable std::vector<std::string> extra_files_;
    // Membership mirror for extra_files_.  The vector remains the ordered
    // source for iteration/background scheduling, while this set keeps the
    // didOpen/didChange critical section from scanning large filelists.
    mutable std::unordered_set<std::string> extra_file_set_;
    mutable std::unordered_map<std::string, ExtraFileCacheEntry> extra_cache_;
    mutable std::shared_ptr<const SyntaxIndex> extra_project_index_cache_;
    mutable std::function<void()> project_index_publish_callback_;

    // clangd-style background index state.
    //
    // The map mutex protects both the foreground document shards and this
    // background queue.  The worker copies one path + config snapshot, releases
    // the mutex while slang parses that file, then reacquires it only to commit
    // the resulting per-file shard.  This keeps the LSP request path from
    // blocking behind a full .f parse.
    mutable std::condition_variable_any background_cv_;
    mutable std::deque<std::string> background_pending_files_;
    mutable bool background_index_active_{false};
    mutable bool background_publish_requested_{false};
    mutable uint64_t background_generation_{0};
    mutable std::jthread background_indexer_;

    std::unordered_map<std::string, std::vector<ParseDiagInfo>> semantic_diagnostics_;
};
