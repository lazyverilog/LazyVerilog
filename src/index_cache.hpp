#pragma once

#include "syntax_index.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// On-disk cache of per-file `SyntaxIndex` shards.
///
/// Every launch otherwise reparses a project that has not changed.  The parse
/// is the cost: on a UVM design, preprocessing `uvm_macros.svh` once per file,
/// expanding its macros, and walking the expansion is work no amount of
/// in-process sharing can remove, because `include` semantics make it
/// genuinely per-file.  What *can* be removed is doing it again on the next
/// launch.
///
/// The model follows clangd's background index (`clang-tools-extra/clangd/
/// index/Background.cpp`), including the decision that matters most here:
///
///   * a shard is keyed on the **content digest** of the file it was built
///     from, never on mtime.  On the shared filesystems this server is aimed
///     at, mtime is the one input you cannot trust — clock skew between nodes,
///     NFS attribute caching, and a fresh checkout or rsync resetting stamps
///     all produce both false hits and false misses.  Hashing costs one read
///     of a file that is about to be read anyway;
///   * every header the parse pulled in is keyed the same way, so editing a
///     shared header invalidates exactly its includers;
///   * a stale or unreadable shard is never an error.  It is a miss, and a
///     miss is the behaviour this server had before the cache existed.
class IndexCache {
public:
    /// 128-bit content digest.  Two independent 64-bit streams rather than one:
    /// a project has O(10^4) files and O(10^9) launches over its life, and a
    /// silent collision here serves a wrong index with no way for the user to
    /// tell.  The extra 8 bytes per shard buy that away entirely.
    struct Digest {
        uint64_t lo{0};
        uint64_t hi{0};
        constexpr bool operator==(const Digest&) const = default;
        constexpr bool empty() const { return lo == 0 && hi == 0; }
    };

    /// What a stored shard was built from.  A shard is usable only when every
    /// one of these still matches the working tree.
    struct Key {
        /// Bytes of the file the shard indexes.
        Digest content;
        /// Defines and include directories, which change what a parse means
        /// without changing any file.
        Digest config;
        /// Every file the parse pulled in through `include`, and what it
        /// hashed to.  Stored as normalized file:// URIs, matching
        /// SyntaxIndex::include_dependencies.
        std::vector<std::pair<std::string, Digest>> dependencies;
    };

    struct Loaded {
        SyntaxIndex index;
        Key key;
        /// Whether this shard came from parsing the file on its own.  Only
        /// meaningful for an `include`d header, where it decides whether the
        /// rest of a burst may be served the header's directives alone.  A
        /// restored shard that forgot this would still be correct, but every
        /// open buffer including the header would go back to re-reading it
        /// whole on each keystroke -- see build_header_shards().
        bool stands_alone{false};
    };

    static Digest digest_bytes(std::string_view bytes);
    /// Digest of a file's contents, or nullopt when it cannot be read.  An
    /// unreadable file is deliberately not a zero digest: that would compare
    /// equal to another unreadable file and turn two different misses into a
    /// hit.
    static std::optional<Digest> digest_file(const std::filesystem::path& path);
    static Digest config_digest(const std::vector<std::string>& defines,
                                const std::vector<std::filesystem::path>& include_dirs);

    /// Open (creating if needed) the cache for @p project_root, or nullopt when
    /// the directory cannot be created — a read-only checkout is a normal
    /// condition, not a failure, and the server runs uncached.
    static std::optional<IndexCache> open(const std::filesystem::path& project_root);

    /// Directory shards live in: `<project_root>/.cache/lazyverilog/index`.
    /// Mirrors clangd's `<project_root>/.cache/clangd/index`, including the
    /// `.gitignore` written beside them.
    static std::filesystem::path directory_for(const std::filesystem::path& project_root);

    /// Read the shard stored for @p uri.  Returns nullopt when there is none,
    /// when it was written by a different format version, or when it is
    /// truncated or corrupt.  Validating the returned key against the working
    /// tree is the caller's job: it can memoize digests across shards, and a
    /// header shared by hundreds of files must not be hashed once per shard.
    std::optional<Loaded> load(std::string_view uri) const;

    /// Write @p index for @p uri.  Written to a temporary and renamed, so a
    /// reader either sees the previous shard or this one, never a half-written
    /// file, and two workers racing on the same shard cannot interleave.
    /// Failures are silent by design; a cache that cannot be written is a
    /// cache that misses.
    void store(std::string_view uri, const Key& key, const SyntaxIndex& index,
               bool stands_alone = false) const;

    const std::filesystem::path& directory() const { return directory_; }

private:
    explicit IndexCache(std::filesystem::path directory) : directory_(std::move(directory)) {}
    std::filesystem::path shard_path(std::string_view uri) const;

    std::filesystem::path directory_;
};

/// Serialize / deserialize without touching the filesystem.  Exposed for tests,
/// which round-trip a hand-built index rather than a parsed one.
std::string serialize_index_shard(const IndexCache::Key& key, const SyntaxIndex& index,
                                  bool stands_alone = false);
std::optional<IndexCache::Loaded> deserialize_index_shard(std::string_view bytes);
