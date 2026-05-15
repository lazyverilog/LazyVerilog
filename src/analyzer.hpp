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

    /// Set extra files from .f filelist.
    void set_extra_files(const std::vector<std::string>& paths);

    /// Return extra files from .f filelist.
    std::vector<std::string> extra_files() const;

    /// Return parsed/indexed extra files, refreshing stale disk entries first.
    std::vector<ExtraFileInfo> extra_file_snapshots() const;

    /// Append cached extra-file modules to an existing SyntaxIndex.
    void merge_extra_file_modules(SyntaxIndex& index) const;

    /// Check mtime of extra files and re-parse if stale.
    void refresh_if_stale(const std::string& uri) const;

  private:
    std::shared_ptr<DocumentState> make_state(const std::string& uri,
                                              const std::string& text) const;
    std::optional<Location>
    definition_of_state(const DocumentState& state, const std::string& uri, int line, int col,
                        const std::vector<ExtraFileInfo>& extra_files) const;

    struct ExtraFileCacheEntry {
        std::string path;
        std::string uri;
        std::optional<std::filesystem::file_time_type> mtime;
        std::shared_ptr<const DocumentState> state;
        SyntaxIndex index;
    };

    void refresh_extra_cache_locked() const;

    mutable std::mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<const DocumentState>> docs_;
    mutable std::vector<std::string> extra_files_;
    mutable std::vector<ExtraFileCacheEntry> extra_cache_;
};
