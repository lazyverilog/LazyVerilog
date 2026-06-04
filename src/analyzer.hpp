#pragma once
#include "document_state.hpp"
#include "syntax_index.hpp"
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
    std::shared_ptr<const DocumentState> state;
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
};

class Analyzer {
  public:
    Analyzer() = default;

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

    /// Return parsed/indexed extra files, refreshing stale disk entries first.
    std::vector<ExtraFileInfo> extra_file_snapshots() const;

    /// Append cached extra-file modules to an existing SyntaxIndex.
    void merge_extra_file_modules(SyntaxIndex& index) const;

    /// Return a cached project-wide index built from filelist-only extra files.
    ///
    /// This is intentionally separate from extra_file_snapshots():
    /// extra_file_snapshots() returns per-file copies and substitutes live
    /// open-buffer states, which is useful for navigation features but
    /// expensive for high-frequency completion requests.  extra_project_index()
    /// keeps one premerged disk-backed index that is invalidated whenever the
    /// extra-file cache is refreshed.  Callers that need current-buffer facts
    /// should merge this into the current DocumentState::index, letting the
    /// current buffer win for duplicate module/class/typedef names.
    std::shared_ptr<const SyntaxIndex> extra_project_index() const;

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
        std::shared_ptr<const DocumentState> state;
        SyntaxIndex index;
    };

    void refresh_extra_cache_locked() const;
    void invalidate_extra_project_index_locked() const;
    void update_extra_cache_for_live_state_locked(std::shared_ptr<const DocumentState> state);

    // Resolved .f filelist path.  We intentionally do not poll this file's
    // mtime on LSP requests: HPC projects usually do not edit filelists while
    // the editor is alive, and even one metadata operation per request is still
    // unwanted noise on shared filesystems.  Configuration reloads call
    // set_extra_files() explicitly when the filelist should be re-read.
    mutable std::string filelist_path_;

    mutable std::mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<const DocumentState>> docs_;
    std::vector<std::string> defines_;
    std::vector<std::string> include_dirs_;
    mutable std::vector<std::string> extra_files_;
    mutable std::vector<ExtraFileCacheEntry> extra_cache_;
    mutable std::shared_ptr<const SyntaxIndex> extra_project_index_cache_;
    std::unordered_map<std::string, std::vector<ParseDiagInfo>> semantic_diagnostics_;
};
