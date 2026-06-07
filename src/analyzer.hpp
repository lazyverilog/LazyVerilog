#pragma once
#include "document_state.hpp"
#include "syntax_index.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

struct OpenIndexShard {
    std::string uri;
    // Keep the immutable DocumentState alive for as long as `index` is used.
    // get_dynamic_index() returns a reference to a cache owned by DocumentState,
    // so the shard snapshot must retain the owner rather than exposing a
    // dangling raw SyntaxIndex pointer after didChange replaces docs_[uri].
    std::shared_ptr<const DocumentState> state;
    const SyntaxIndex* index{nullptr};
};

struct CompilationSourceFile {
    std::string uri;
    std::string path;
    // Non-null for open buffers.  This aliases the immutable DocumentState text
    // so background compilation sees unsaved contents without copying entire
    // large RTL buffers into every CompilationSnapshot.
    std::shared_ptr<const std::string> text;
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
    // Definition location for the displayed module.  `line` follows the
    // existing SyntaxIndex convention for ModuleEntry: 1-based, with 0 meaning
    // unknown.  `col` is 0-based.  The Neovim tree client uses these fields to
    // make <CR> jump to the module definition instead of only opening the file.
    int line{0};
    int col{0};
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

    /// Update text immediately without reparsing (tree will be null until
    /// the next change() call).  Use on every didChange so get_state()->text
    /// is always current; call change() on debounce to build the AST.
    void update_text(const std::string& uri, const std::string& text);

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

    /// Call f(uri, state) for every open document snapshot.
    ///
    /// The callback is intentionally invoked *after* releasing map_mutex_.  A
    /// number of feature callbacks lazily build structural indexes or walk large
    /// SyntaxTree snapshots.  Running that work under the global analyzer mutex
    /// would serialize unrelated LSP requests, document edits, and background
    /// index commits behind potentially expensive AST traversal.
    template <typename F> void for_each_state(F&& f) const {
        std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> snapshot;
        {
            std::lock_guard<std::mutex> lk(map_mutex_);
            snapshot.reserve(docs_.size());
            for (const auto& [uri, state] : docs_)
                snapshot.emplace_back(uri, state);
        }
        for (const auto& [uri, state] : snapshot)
            f(uri, state);
    }

    /// Set extra source files from the configured .f filelist.
    ///
    /// LazyVerilog indexes only explicit source-file entries.  +incdir+
    /// entries parsed from the same filelist are handled separately via
    /// set_include_dirs(); they are include search paths, not source files.
    ///
    /// @param filelist_path  Resolved absolute path to the .f file itself (may be empty).
    ///                       Stored as configuration provenance for reload / diagnostics paths.
    ///                       Request handlers deliberately do not poll or stat this file on
    ///                       shared filesystems; freshness is driven by explicit config reloads
    ///                       and watched-file notifications.
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

    /// Configure debounce for publishing the merged project index after shard updates.
    /// A value <= 0 publishes as soon as the background indexer can process the request.
    void set_project_index_publish_debounce_ms(int debounce_ms);

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

    /// Synchronously parse all configured project files and return temporary
    /// document snapshots for whole-project commands such as :LintAll.
    ///
    /// This is intentionally not used by normal hot LSP request paths.  It may
    /// be slow for large .f designs, but it does not mutate extra_cache_,
    /// ProjectIndexSnapshot, background queues, or open-buffer state.  Open
    /// project buffers are returned from their live DocumentState snapshots so
    /// unsaved edits participate in the command.
    std::vector<std::shared_ptr<const DocumentState>> project_file_states_sync() const;

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
    std::shared_ptr<const std::vector<ExtraFileInfo>> extra_file_snapshot_ptr() const;

    /// Return per-file project index shards without exposing full SyntaxTrees.
    ///
    /// Cross-file features should prefer this over extra_file_snapshot_ptr() when
    /// they only need indexed structural data.  This supports the indexing
    /// philosophy: current file uses AST, project files use index.
    std::shared_ptr<const std::vector<ExtraIndexInfo>> extra_index_snapshot_ptr() const;

    /// Return the last background-published project-wide shard snapshot.
    ///
    /// This is the Option-B project index: publishing records immutable per-file
    /// shards plus lightweight global lookup maps.  It does not copy/merge all
    /// SyntaxIndex entries on every live-file edit.
    std::shared_ptr<const ProjectIndexSnapshot> project_index_snapshot() const;

    /// Register a callback fired whenever the merged project index snapshot is
    /// republished.  The callback may run on the background indexer thread, so
    /// it must be non-blocking and must not call back into Analyzer.
    void set_project_index_publish_callback(std::function<void()> callback);

    /// Return dynamic/file indexes for other open buffers as per-file shards.
    ///
    /// This is the clangd-style dynamic layer: files that have been
    /// opened/parsed during this editor session, including unsaved edits.  The
    /// current file is excluded because completion/point queries inspect its
    /// SyntaxTree directly instead of materializing a current-file index.
    ///
    /// The API returns independent SyntaxIndex shards rather than a merged
    /// SyntaxIndex, so request paths do not rebuild an all-open-file merge on
    /// every edit-driven cache miss.
    std::shared_ptr<const std::vector<OpenIndexShard>>
    opened_file_index_shards(const std::string& current_uri) const;

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
    std::optional<Location>
    definition_of_state(const DocumentState& state, const std::string& uri, int line, int col,
                        std::span<const ExtraFileInfo> extra_files,
                        const std::string* skip_extra_uri = nullptr) const;

    struct ExtraFileCacheEntry {
        std::string path;
        std::string uri;
        std::shared_ptr<const SyntaxIndex> index;
    };

    void start_background_indexer_locked() const;
    void schedule_background_reindex_locked() const;
    void schedule_background_project_publish_locked() const;
    void background_index_loop() const;
    std::function<void()> publish_project_index_snapshot_locked() const;
    void clear_project_index_snapshot_locked() const;
    void invalidate_extra_snapshots_locked() const;
    std::shared_ptr<const std::vector<ExtraFileInfo>> build_extra_file_snapshot_locked() const;
    std::shared_ptr<const std::vector<ExtraIndexInfo>> build_extra_index_snapshot_locked() const;
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
    // Normalized absolute lexical filesystem paths.  Writers normalize before
    // storing so hot snapshot/request paths can trust the invariant instead of
    // repeating path normalization under map_mutex_ for large filelists.
    mutable std::vector<std::string> extra_files_;
    // Membership mirror for extra_files_.  The vector remains the ordered
    // source for iteration/background scheduling, while this set keeps the
    // didOpen/didChange critical section from scanning large filelists.
    mutable std::unordered_set<std::string> extra_file_set_;
    mutable std::unordered_map<std::string, ExtraFileCacheEntry> extra_cache_;
    mutable std::shared_ptr<const std::vector<ExtraFileInfo>> extra_file_snapshot_cache_;
    mutable std::shared_ptr<const std::vector<ExtraIndexInfo>> extra_index_snapshot_cache_;
    mutable std::shared_ptr<const ProjectIndexSnapshot> project_index_snapshot_cache_;
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
    mutable int background_publish_debounce_ms_{0};
    mutable std::chrono::steady_clock::time_point background_publish_due_time_{};
    mutable uint64_t background_generation_{0};
    mutable std::thread background_indexer_;
    mutable std::atomic<bool> background_stop_{false};

    std::unordered_map<std::string, std::vector<ParseDiagInfo>> semantic_diagnostics_;
};
