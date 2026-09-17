#pragma once
#include "document_state.hpp"
#include "index_cache.hpp"
#include "parse_inputs.hpp"
#include "syntax_index.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <deque>
#include <functional>
#include <limits>
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

/// Text of `include`d headers seen during the current background indexing burst.
///
/// Every project file is parsed through its own SourceManager, so a header that
/// hundreds of files include is opened and read once per including file.  slang
/// consults its own lookup cache before touching the filesystem
/// (SourceManager::openCached), so seeding known header text with assignText()
/// removes that repeated I/O.  On a workstation this is nearly free either way;
/// on a shared/HPC filesystem each avoided open is a network round trip.
///
/// The cache deliberately lives no longer than one indexing burst and is cleared
/// when the queue drains.  A header edited while the server is idle is therefore
/// always re-read from disk on the next parse, so cached text can never outlive
/// the work it was collected for.
struct HeaderTextCache {
    /// Total cached bytes allowed.  A design whose headers exceed this keeps the
    /// first ones seen; seeding is an optimization, and a partial cache is
    /// simply a partial win.
    static constexpr size_t kMaxBytes = 32u << 20;

    struct Entry {
        std::shared_ptr<const std::string> text;
        /// Parses in this burst that included this header.
        size_t hits{0};
        /// Digest of the header as first read, which `text` stops being once
        /// project() replaces it with the directives alone.  The shard cache
        /// keys a shard on the bytes its parse read, and a parse seeded from
        /// here read whatever this holds -- so the digest has to be taken when
        /// the full text enters, and travel with the entry from then on.
        IndexCache::Digest digest;
    };

    /// One header a parse pulled in: the key it is cached under, and its text.
    using ParsedHeader = std::pair<std::string, std::string_view>;

    mutable std::mutex mutex;
    std::unordered_map<std::string, Entry> texts;
    size_t bytes{0};
    /// Parses of *project files* in this burst, the denominator for the
    /// popularity rule below.  A parse the burst makes of a header on its own is
    /// not one of them; see record_parse().
    size_t parses{0};
    /// Background generation these texts were collected under.  Every path that
    /// invalidates parse results — config reload, a changed header, a changed
    /// open buffer — already bumps that counter, so comparing it is enough to
    /// drop text that a newer generation must re-read.  Workers do the dropping
    /// themselves, which keeps this mutex off map_mutex_'s lock order.
    uint64_t generation{0};

    /// Record one parse of the burst together with every header it pulled in.
    ///
    /// Deliberately one call under one lock, rather than a parse counter plus a
    /// store per header: `parses` is the denominator of the popularity rule in
    /// seed_candidates() and `hits` its numerator, so a reader that catches the
    /// two half updated sees a header as less shared than it is and declines to
    /// seed it.  Split across two calls that window was real: a burst's shared
    /// header sits at hits=1, parses=2 the moment the warmup gate opens, and one
    /// worker between the two calls made it hits=1, parses=3 — the header
    /// dropped off the seed list and the next file re-read it whole, which is
    /// the O(files x header) cost the projection exists to prevent.  Measured at
    /// roughly one indexing burst in 60.
    ///
    /// @p count_as_burst_parse is false for the parse build_header_shards() makes
    /// of a header on its own.  That is the burst's own bookkeeping, not a file
    /// of the project, and it is the one parse guaranteed never to want the
    /// header it is parsing — charging it to the denominator biases the rule
    /// against precisely the header being projected.
    void record_parse(uint64_t gen, const std::vector<ParsedHeader>& headers,
                      bool count_as_burst_parse) {
        std::lock_guard<std::mutex> lock(mutex);
        discard_stale(gen);
        if (count_as_burst_parse)
            ++parses;
        for (const auto& [path, text] : headers)
            store_locked(path, text);
    }

    /// Replace what this burst serves for @p path with @p text.
    ///
    /// Used to hand the remaining files a header's directives without its
    /// declarations, once the header's own shard has recorded them; see
    /// header_directives_only() for why that is equivalent for an includer.
    /// Only an entry this burst already holds is replaced: an absent one means
    /// record_parse() judged the header not worth caching, and inserting it here
    /// would enter it with a hit count no popularity rule can ever admit.
    void project(uint64_t gen, const std::string& path, std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (gen != generation)
            return;
        const auto it = texts.find(path);
        if (it == texts.end())
            return;
        bytes -= it->second.text->size();
        bytes += text.size();
        it->second.text = std::make_shared<const std::string>(std::move(text));
    }

    /// Headers worth seeding into the next parse, as shared pointers so seeding
    /// does not hold the lock while slang copies text into a SourceManager.
    ///
    /// Seeding is not free: slang copies the text into the target
    /// SourceManager, so offering every header the burst has seen costs
    /// O(files x cached bytes) and buys a saved read only for the files that
    /// actually include it.  A header at least half the burst's parses pulled
    /// in is nearly certain to be needed again; a header one file included is
    /// nearly certain not to be.  Restricting the offer to the former keeps the
    /// shared-header win and drops the fan-out cost on designs with many
    /// distinct headers.
    struct SeedCandidate {
        std::string path;
        std::shared_ptr<const std::string> text;
        /// Of the header, not of `text`; see Entry::digest.
        IndexCache::Digest digest;
    };

    std::vector<SeedCandidate> seed_candidates(uint64_t gen) {
        std::lock_guard<std::mutex> lock(mutex);
        discard_stale(gen);
        std::vector<SeedCandidate> candidates;
        candidates.reserve(texts.size());
        for (const auto& [path, entry] : texts) {
            if (entry.hits * 2 >= parses)
                candidates.push_back(SeedCandidate{path, entry.text, entry.digest});
        }
        return candidates;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        texts.clear();
        bytes = 0;
        parses = 0;
    }

private:
    void store_locked(const std::string& path, std::string_view text) {
        if (const auto it = texts.find(path); it != texts.end()) {
            ++it->second.hits;
            return;
        }
        if (bytes + text.size() > kMaxBytes)
            return;
        bytes += text.size();
        // Hashed once, on the parse that first read the header in full.  Every
        // later parse in the burst is seeded from this entry, so this is the
        // only reading of those bytes there is to key a shard on.
        texts.emplace(path, Entry{std::make_shared<const std::string>(text), 1,
                                  IndexCache::digest_source_buffer(text)});
    }

    void discard_stale(uint64_t gen) {
        if (gen == generation)
            return;
        texts.clear();
        bytes = 0;
        parses = 0;
        generation = gen;
    }
};

/// File extensions the server asks the client to watch for on-disk changes.
///
/// Single source of truth: server.cpp turns these into the
/// workspace/didChangeWatchedFiles glob registration, and OpenParseHeaderCache
/// refuses to cache anything outside the list, because outside it nothing would
/// ever tell the cache its text went stale.
inline constexpr std::string_view kWatchedSourceExtensions[] = {
    ".sv", ".v", ".svh", ".vh", ".f", ".vf", ".svi",
};

inline bool is_watched_source_path(std::string_view path) {
    for (const auto ext : kWatchedSourceExtensions) {
        if (path.size() > ext.size() && path.ends_with(ext))
            return true;
    }
    return false;
}

/// Text of headers an *open buffer* `include`s, kept across keystrokes.
///
/// didChange builds a fresh SourceManager per document snapshot, so without this
/// every keystroke re-opens and re-reads every header the buffer includes.  A
/// design where each module includes one multi-megabyte shared header pays that
/// read on every character typed, and on a networked filesystem the read, not
/// the parse, is what the user feels.
///
/// HeaderTextCache cannot serve this: it is scoped to one background indexing
/// burst and dropped when the queue drains, which is what keeps its contents
/// from going stale.  An edit-path cache has to survive far longer than that.
///
/// Staleness is handled by invalidation, not by re-checking the file: the cache
/// makes **no filesystem call at all**, and entries live until someone reports
/// the file changed.  That is the same event-driven rule the project shards
/// already follow (see Analyzer::refresh_changed_extra_files) — the client's
/// OS-level watcher says which files moved, so the server never polls.  A size
/// and mtime check would be two metadata calls per included header per
/// keystroke, which on a shared/HPC filesystem is two network round trips for a
/// question the watcher already answers for free.
///
/// The consequence is worth stating plainly: if the client does not deliver
/// workspace/didChangeWatchedFiles, an externally edited header stays cached
/// until the project configuration changes or the server restarts.  Both
/// shipped clients (lua/, vscode/) register the watcher, and only the
/// extensions in kWatchedSourceExtensions are cached, so a path nothing watches
/// is simply re-read every keystroke as it was before this cache existed.
struct OpenParseHeaderCache {
    /// Same budget as HeaderTextCache, for the same reason: a design whose
    /// headers exceed it keeps the ones seen first and the win is partial.
    static constexpr size_t kMaxBytes = 32u << 20;

    struct Entry {
        std::shared_ptr<const std::string> text;
        /// text with everything but its preprocessor directives blanked out,
        /// built on first use.  Not counted against the byte budget: it is a
        /// fraction of the text it is derived from, and it only exists for
        /// headers already admitted.
        std::shared_ptr<const std::string> directives;
    };

    /// Cached text for @p path, or null if nothing is cached for it.
    std::shared_ptr<const std::string> get(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = texts.find(path);
        return it == texts.end() ? nullptr : it->second.text;
    }

    /// Cached text for @p path with only its preprocessor directives left.
    ///
    /// @p project is header_directives_only(), passed in rather than called
    /// directly so the projection rules stay in the .cpp with the parse path
    /// that needs them.  It runs outside the mutex: it is a scan of the whole
    /// header, and this mutex is on the edit path of every open buffer.
    std::shared_ptr<const std::string>
    get_directives(const std::string& path,
                   const std::function<std::string(std::string_view)>& project) {
        std::shared_ptr<const std::string> text;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = texts.find(path);
            if (it == texts.end())
                return nullptr;
            if (it->second.directives)
                return it->second.directives;
            text = it->second.text;
        }
        auto projected = std::make_shared<const std::string>(project(*text));
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = texts.find(path);
        if (it == texts.end() || it->second.text != text)
            return projected; // invalidated while projecting; use it, cache nothing
        if (!it->second.directives)
            it->second.directives = std::move(projected);
        return it->second.directives;
    }

    void store(const std::string& path, std::string_view text) {
        if (!is_watched_source_path(path))
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (texts.contains(path) || bytes + text.size() > kMaxBytes)
            return;
        bytes += text.size();
        texts.emplace(path, Entry{std::make_shared<const std::string>(text), nullptr});
    }

    /// Drop @p path so the next parse reads it from disk again.
    void invalidate(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = texts.find(path);
        if (it == texts.end())
            return;
        bytes -= it->second.text->size();
        texts.erase(it);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        texts.clear();
        bytes = 0;
    }

    mutable std::mutex mutex;
    std::unordered_map<std::string, Entry> texts;
    size_t bytes{0};
};

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
    // How this occurrence spells the symbol.  Only rename looks at it; every
    // other consumer treats an implicit `.p,` connection as an ordinary
    // occurrence, which is what references should report.
    RefForm form{RefForm::Plain};
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
    // Shared pointer avoids deep-copying the SyntaxIndex when the snapshot
    // cache is rebuilt (e.g. on every didChange).  With 100+ project files the
    // copy cost is significant; sharing the immutable shard is safe because
    // ExtraFileCacheEntry already owns the canonical shared_ptr.
    std::shared_ptr<const SyntaxIndex> index;
    const SyntaxIndex& index_ref() const {
        static const SyntaxIndex empty{};
        return index ? *index : empty;
    }
};

struct ExtraIndexInfo {
    std::string path;
    std::string uri;
    std::shared_ptr<const SyntaxIndex> index;
    const SyntaxIndex& index_ref() const {
        static const SyntaxIndex empty{};
        return index ? *index : empty;
    }
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
    std::unordered_map<std::string, uint64_t> uri_versions;
    /// Which project each of `files` belongs to, answered by the compiler
    /// rather than here.
    ///
    /// A pointer copy, deliberately: the snapshot is built under map_mutex_ and
    /// resolving a project walks up to the nearest config, statting each
    /// directory on the way.  Doing that per file inside the critical section
    /// would put a filesystem walk per filelist entry on the lock every request
    /// handler contends for.  The compiler runs on its own thread with nothing
    /// waiting on it, and ProjectParseInputs is immutable once published.
    std::shared_ptr<const ProjectParseInputs> parse_inputs;
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

    /// Join every thread this analyzer started.  Idempotent.
    ///
    /// A parse worker calls the parse-complete callback, and the server's
    /// callback publishes diagnostics through the LSP transport -- so these
    /// threads outlive nothing that the transport's own teardown touches.
    /// Leaving them to ~Analyzer is too late: the server's members are
    /// destroyed in reverse declaration order, which frees the endpoint first
    /// and leaves a worker mid-publish writing through it.  The server calls
    /// this at the top of its destructor for that reason; ~Analyzer calls it
    /// too, for every other owner.
    void stop();

    /// Create a new DocumentState for uri with the given text.
    void open(const std::string& uri, const std::string& text);

    /// Update text for uri, creating a new immutable DocumentState snapshot.
    void change(const std::string& uri, const std::string& text);

    /// Replaces update_text() + change().  Immediately stores null-tree state
    /// with current text, assigns a monotonically increasing version, enqueues
    /// async parse.  Returns assigned version.
    /// Takes @p text by value: didChange builds the new buffer text itself and
    /// has no further use for it, so the caller can move it in and the snapshot
    /// installed here is the only copy the edit path makes.
    uint64_t enqueue_parse(const std::string& uri, std::string text);

    /// Server registers this once.  Called on worker thread after a parse commits.
    void set_parse_complete_callback(
        std::function<void(const std::string& uri)> cb);

    /// Hold queued parses instead of letting the worker take them.
    ///
    /// A snapshot with text and no tree is the state the editor's own requests
    /// land in -- it sends them from the `didChange` notification, so they
    /// arrive while the parse that notification started is still running.  The
    /// tests that pin behaviour there have to *be* in that window, and racing
    /// the worker for it is not something a test can win on a one-CPU slice:
    /// the scheduler that runs the worker first runs it first on every retry,
    /// so the test failed rather than skipped.  Pausing makes the window a
    /// state the test enters, not one it hopes to catch.
    ///
    /// An unpause releases the worker; a parse already in progress when this is
    /// set still commits.  Nothing in the server calls this.
    void set_parse_paused(bool paused);

    /// Remove document from cache.
    void close(const std::string& uri);

    /// Return a snapshot (or nullptr if not open).
    std::shared_ptr<const DocumentState> get_state(const std::string& uri) const;

    /// Return a snapshot that has a parsed tree, for handlers that cannot
    /// answer without one.
    ///
    /// The editor issues its requests from the `didChange` notification itself,
    /// so they routinely arrive while the parse that notification started is
    /// still running and `get_state()` hands back a text-only placeholder.  A
    /// handler that gives up there answers every request during typing with
    /// nothing, and the client renders that.
    ///
    /// Waits up to @p timeout for the reparse to commit, then falls back to the
    /// last snapshot that did parse -- one keystroke stale, which is what the
    /// user was looking at a moment ago, rather than empty.  Returns whatever
    /// `get_state()` would when neither exists.
    ///
    /// The wait is bounded because requests are answered one at a time: a file
    /// whose parse never keeps up must not wedge the request thread.
    std::shared_ptr<const DocumentState>
    get_parsed_state(const std::string& uri,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(150)) const;

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
    /// @param file_sizes  byte size of each entry of @p paths, in the same
    ///        order, when the caller already knows them -- the filelist loader
    ///        stats every entry it records, so handing those numbers over spares
    ///        the background queue a second metadata pass over the project.
    ///        Empty means "not known"; the sizes are then read here as before.
    void set_extra_files(const std::vector<std::string>& paths,
                         const std::vector<uintmax_t>& file_sizes = {});

    /// Apply all project-parse inputs from one loaded configuration in a single
    /// analyzer transaction.
    ///
    /// This is preferred by the LSP server when loading or reloading
    /// lazyverilog.toml because defines, include directories, and filelist
    /// entries all affect project shards.  Updating them independently can
    /// schedule multiple full-project background reindex generations for a
    /// single user-visible config change.  This batched setter clears the old
    /// project cache once and schedules at most one asynchronous reindex.
    /// @param cache_storage  decides which directory each file's shards live
    ///        in.  Null runs uncached, which is what a server with no project
    ///        should do.  It is a storage rather than a single root because a
    ///        project's files are not all under one: a filelist can name
    ///        sources from a sibling project, and each belongs beside its own
    ///        lazyverilog.toml.
    /// @param extra_file_sizes  see set_extra_files(); empty when unknown.
    void set_project_config(const std::vector<std::string>& defines,
                            const std::vector<std::string>& include_dirs,
                            const std::vector<std::string>& extra_files,
                            std::shared_ptr<IndexCacheStorage> cache_storage = nullptr,
                            const std::vector<uintmax_t>& extra_file_sizes = {});

    /// Point per-file parse-input lookups at @p resolver.
    ///
    /// Without one every file is parsed with the defaults, which is what a
    /// single-project session and every CLI tool want.  The LSP server hands
    /// over the same resolver its shard storage uses, so a file's config, its
    /// parse inputs and its shard directory are all decided by one walk.
    void set_project_root_resolver(std::shared_ptr<const ProjectRootResolver> resolver);

    /// Whether a setter schedules a background reindex itself, or leaves it to
    /// the set_project_config() the caller is about to make.
    ///
    /// Deferring matters because the generation scheduled here would parse the
    /// *previous* filelist: it is superseded before it can commit, but the
    /// workers already dispatched keep parsing, and on a large design that is
    /// a full reindex of CPU and shared-filesystem bandwidth spent for nothing.
    /// Registering N projects and then applying once costs N+1 generations
    /// instead of 1.
    enum class Reindex { Now, Deferred };

    /// Register how files under @p root are preprocessed.
    ///
    /// This is clangd's compilation database learning a project: one indexer,
    /// commands looked up per file.  Files under no registered root keep using
    /// the inputs from set_project_config().
    void set_parse_inputs_for_root(const std::filesystem::path& root,
                                   const std::vector<std::string>& defines,
                                   const std::vector<std::string>& include_dirs,
                                   Reindex reindex = Reindex::Now);

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

    /// The same shards, ordered nearest-first against @p from_path.
    ///
    /// This is the order every by-name scan over the project has to run in --
    /// see by_path_proximity() -- and it is memoized because computing it is
    /// linear in the filelist while the thing it depends on is not: the
    /// snapshot is immutable and shared until the next publish, and a person
    /// asks several questions about the same buffer before either changes.
    /// Ordering per request instead measured 129us on 1500 files and 455us on
    /// 5000, against a go-to-definition that otherwise answers in single-digit
    /// microseconds.
    ///
    /// Holding @p files keeps the pointers valid and makes its address a sound
    /// cache key; without that a freed snapshot could be replaced by a new one
    /// at the same address and this would hand back pointers into it.
    std::shared_ptr<const std::vector<const ExtraFileInfo*>>
    ranked_extra_files(const std::shared_ptr<const std::vector<ExtraFileInfo>>& files,
                       std::string_view from_path) const;

    /// Return the last background-published project-wide shard snapshot.
    ///
    /// This is the Option-B project index: publishing records immutable per-file
    /// shards plus lightweight global lookup maps.  It does not copy/merge all
    /// SyntaxIndex entries on every live-file edit.
    std::shared_ptr<const ProjectIndexSnapshot> project_index_snapshot() const;

    /// Definition for a hierarchical path such as `tb.u_dut.u_sub.sig`, resolved
    /// by walking instance names through published index snapshots.  Returns
    /// nothing when any segment is unresolved.
    std::optional<Location> hierarchical_definition(const DocumentState& state,
                                                    const std::string& uri, int line,
                                                    int col) const;

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
        std::unordered_map<std::string, std::vector<ParseDiagInfo>> diagnostics,
        const std::unordered_map<std::string, uint64_t>& snapshot_versions);

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
    /// @p ranked is the candidate files **in the order they should be tried**.
    /// Every search below takes the first that can answer, so that order is
    /// what decides which project a name resolves into; the caller owns it
    /// (ranked_extra_files()) because it owns the request.  Passing an empty
    /// span restricts the search to the current document, which is what
    /// find_references() wants.
    std::optional<Location>
    definition_of_state(const DocumentState& state, const std::string& uri, int line, int col,
                        std::span<const ExtraFileInfo* const> ranked,
                        const std::string* skip_extra_uri = nullptr) const;

    struct ExtraFileCacheEntry {
        std::string path;
        std::string uri;
        std::shared_ptr<const SyntaxIndex> index;
    };

    /// Append @p path to the background queue unless it is already waiting.
    /// @p front puts it ahead of the cold-start filelist backlog, which is what
    /// edit-driven refreshes want.
    /// Queue every open buffer and project shard that `include`s @p uri for a
    /// background reparse.  Returns whether anything was queued; the caller
    /// bumps the generation and wakes the pool.
    bool queue_include_dependents_locked(const std::string& uri) const;
    void queue_background_file_locked(std::string path, bool front) const;
    void start_background_indexer_locked() const;
    void schedule_background_reindex_locked() const;
    void schedule_background_project_publish_locked() const;
    void background_index_loop() const;
    /// Install every shard the on-disk cache can still vouch for and drop those
    /// files from @p background_pending_files_.  Runs on one worker with
    /// map_mutex_ released: it reads and hashes files.
    void preload_cached_shards(uint64_t generation) const;
    /// Record @p index for @p uri, keyed on what it was built from.  @p extra
    /// names a file the shard depends on beyond its own `include`s -- the
    /// includer a fragment header's shard was derived from, which nothing in
    /// the shard itself records.
    /// @param include_resolutions  how the `include`s written *in this file*
    ///        resolved, filtered from the parse that produced @p index.
    void store_shard_in_cache(const std::string& uri, const SyntaxIndex& index,
                              const std::vector<IncludeResolution>& include_resolutions,
                              const std::string& extra_dependency_uri = {},
                              bool stands_alone = false) const;
    std::function<void()> publish_project_index_snapshot_locked() const;
    void clear_project_index_snapshot_locked() const;
    /// The entries of a shard map in a fixed order.
    ///
    /// Both shard maps are unordered, and every consumer of the snapshots built
    /// from them resolves a name by taking the first match -- so without this
    /// the answer was decided by whichever bucket order the last rehash
    /// produced, and could change on any republish.  Path is the one key that
    /// is unique per shard, already held, and the same from one launch to the
    /// next.  Pointers, so nothing is copied.
    static std::vector<const ExtraFileCacheEntry*>
    sorted_by_path(const std::unordered_map<std::string, ExtraFileCacheEntry>& entries);

    void invalidate_extra_snapshots_locked() const;
    /// Narrower counterpart for a change to docs_ rather than to the shards.
    /// Requires map_mutex_.
    void invalidate_open_file_snapshot_locked(const std::string& uri) const;
    std::shared_ptr<const std::vector<ExtraFileInfo>> build_extra_file_snapshot_locked() const;
    std::shared_ptr<const std::vector<ExtraIndexInfo>> build_extra_index_snapshot_locked() const;
    /// Install, drop or empty an `extra_cache_` entry, keeping
    /// `extra_cache_includers_` in step.  The map is the only reason these
    /// exist; nothing else may touch `extra_cache_` directly.  Require
    /// map_mutex_.
    void put_extra_cache_locked(const std::string& uri, ExtraFileCacheEntry entry) const;
    void erase_extra_cache_locked(const std::string& uri) const;
    void clear_extra_cache_locked() const;

    void update_extra_cache_for_live_state_locked(std::shared_ptr<const DocumentState> state,
                                                  SyntaxIndex index);

    mutable std::mutex map_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<const DocumentState>> docs_;

    // One entry, because requests arrive about one buffer at a time.  Its own
    // mutex: this is read on every definition/hover, and map_mutex_ is what the
    // index workers hold.
    mutable std::mutex ranked_extra_mutex_;
    mutable std::shared_ptr<const std::vector<ExtraFileInfo>> ranked_extra_source_;
    mutable std::string ranked_extra_from_;
    mutable std::shared_ptr<const std::vector<const ExtraFileInfo*>> ranked_extra_cache_;
    /// Install @p inputs as the defaults, keeping every registered project's.
    /// Requires map_mutex_.
    void replace_default_parse_inputs_locked(ParseInputs inputs);

    /// How each file is parsed, looked up per file.
    ///
    /// This replaced flat defines_/include_dirs_ members for the reason clangd
    /// keeps compile commands in a database rather than on the server: two
    /// files open at once can be in different projects, and one project's
    /// `+incdir+` entries are meaningless for the other.  Immutable and held by
    /// shared_ptr, so a worker can hold one across a whole parse while a config
    /// reload installs a replacement -- there is no window in which a parse
    /// reads half of one project's inputs and half of another's.
    std::shared_ptr<const ProjectParseInputs> parse_inputs_ =
        std::make_shared<const ProjectParseInputs>();
    // Normalized absolute lexical filesystem paths.  Writers normalize before
    // storing so hot snapshot/request paths can trust the invariant instead of
    // repeating path normalization under map_mutex_ for large filelists.
    mutable std::vector<std::string> extra_files_;
    // The same paths, largest first, which is the order the background queue is
    // filled in.
    //
    // Indexing a file costs roughly what its size says it will, and a filelist
    // is written in whatever order a design is assembled -- so the generated
    // register blocks that dominate a project's total are as likely to be at
    // the end as anywhere.  Taking them last strands one worker on a file the
    // others cannot help with while they idle: simulated over 961 files of RTL
    // whose four largest are 1.6 MB, 1.5 MB, 1.4 MB and 776 KB, filelist order
    // finishes 18% behind longest-first on four workers, which itself reaches
    // the ideal split.
    //
    // Built off map_mutex_ -- it costs one stat per file, and that lock is the
    // one every request handler contends for.  Ties keep filelist order, so the
    // queue stays deterministic for a given project.
    mutable std::vector<std::string> extra_files_by_size_;
    // Membership mirror for extra_files_.  The vector remains the ordered
    // source for iteration/background scheduling, while this set keeps the
    // didOpen/didChange critical section from scanning large filelists.
    mutable std::unordered_set<std::string> extra_file_set_;
    mutable std::unordered_map<std::string, ExtraFileCacheEntry> extra_cache_;
    /// Which shards `include a given file: header URI -> the URIs of the
    /// entries of `extra_cache_` that list it.
    ///
    /// Answering "does anything include this file" used to be a walk of every
    /// shard in the project, comparing against each one's dependency list.
    /// That ran on *every* didChange, from queue_include_dependents_locked(),
    /// under the lock every request handler and index worker contends for --
    /// and for an ordinary .sv file, which nothing includes, it ran to
    /// completion and found nothing every time.  It ran again per changed
    /// header in refresh_changed_extra_files().
    ///
    /// Only entries that `include something appear, so a design of plain
    /// sources costs nothing to keep, and maintaining it is one pass over a
    /// dependency list the caller already holds.
    mutable std::unordered_map<std::string, std::unordered_set<std::string>>
        extra_cache_includers_;
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
    // Membership mirror for background_pending_files_.  Include fanout queues
    // one entry per dependent file per commit, so editing a widely included
    // header would otherwise append the same path once per keystroke.  A path
    // leaves the set when a worker pops it, so an edit that lands mid-parse
    // still re-queues the file.
    mutable std::unordered_set<std::string> background_pending_set_;
    // Number of workers currently parsing a file.  Idle/publish decisions need
    // the count, not a flag, because several workers drain the queue at once.
    mutable int background_index_active_{0};
    mutable bool background_publish_requested_{false};
    mutable int background_publish_debounce_ms_{0};
    mutable std::chrono::steady_clock::time_point background_publish_due_time_{};
    mutable uint64_t background_generation_{0};
    // Cold-burst warmup gate.
    //
    // A shared header is only discovered by parsing a file that `include`s it,
    // and its directives-only projection is only installed once its own shard
    // exists (see build_header_shards).  Every worker that starts a file before
    // that lands re-parses the whole header, so how much the projection saves is
    // decided by how many files slip through that window -- a scheduling race,
    // not a bound.  Letting one worker take the first file of a burst by itself
    // closes the window: the others wait exactly the time they would otherwise
    // have spent each re-reading the same header.
    //
    // Keyed by generation rather than a flag, so the gate re-arms wherever the
    // generation is bumped.  That is what a changed header needs: it drops the
    // header's shard and re-queues every includer, so the projection has to be
    // rebuilt before that fan-out is released.
    mutable uint64_t background_warmup_generation_{std::numeric_limits<uint64_t>::max()};
    mutable bool background_warmup_running_{false};
    // Cache-preload gate, ahead of the warmup gate and shaped the same way.
    //
    // A burst first asks the on-disk cache which of its files are unchanged
    // since the last launch, installs those shards, and drops them from the
    // queue; only what is left is parsed.  One worker does it while the others
    // wait, for the same reason the warmup gate exists -- a worker that starts
    // parsing a file the preload was about to satisfy has already paid the cost
    // the cache is there to avoid.
    //
    // Keyed by generation, so a config reload re-runs it: new defines or
    // include directories change what every shard's key hashes to.
    mutable uint64_t background_preload_generation_{std::numeric_limits<uint64_t>::max()};
    mutable bool background_preload_running_{false};
    // The generation of the last burst that queued the *whole* filelist.
    //
    // Only such a burst can say which shards are live: its preload walks every
    // configured file, so a shard the sweep does not see named is genuinely
    // unreferenced.  An incremental burst -- the one or two includers an edited
    // header re-queues -- knows about those files and nothing else, so its
    // "live" set is two entries out of thousands and a sweep run against it
    // reads and stats every other shard in the directory to prove it should
    // keep them.  That is one full directory sweep per keystroke on a shared
    // header; see preload_cached_shards().
    mutable uint64_t background_full_reindex_generation_{std::numeric_limits<uint64_t>::max()};
    /// Cache for this project, and the config digest every shard is keyed on.
    /// Empty when no project root is known or the directory cannot be written,
    /// which is a normal read-only-checkout condition and simply runs uncached.
    /// Where each file's shards go.  Shared with the server, which resolves the
    /// same roots to answer config lookups, and held by shared_ptr because a
    /// queued shard write outlives the config reload that replaced it.
    mutable std::shared_ptr<IndexCacheStorage> index_cache_storage_;
    // The shard config digest moved into ParseInputs: it is per project, like
    // the defines and include directories it is computed from.
    /// Memoized content digests for the current generation, shared by the
    /// preload and the store path.
    ///
    /// Storing a shard hashes every file it `include`s, and a central header is
    /// included by every module in the design -- hashing it per includer put
    /// the whole O(files x header) cost back, on the one launch that has to
    /// build the cache from nothing.  Cleared whenever the generation moves,
    /// which is the same point at which the parse path stops trusting anything
    /// it read earlier.
    mutable std::mutex index_cache_digest_mutex_;
    mutable uint64_t index_cache_digest_generation_{std::numeric_limits<uint64_t>::max()};
    mutable std::unordered_map<std::string, std::optional<IndexCache::Digest>>
        index_cache_digests_;
    /// Point the digest memo at @p generation, clearing it, if @p generation is
    /// newer than the one it holds.  True when it was adopted (and so emptied).
    ///
    /// Forward only.  The background generation counts up and never down, and a
    /// caller behind it is a worker whose parse has already been superseded;
    /// letting it re-tag the memo backwards made the current burst's digests
    /// disappear.  Requires index_cache_digest_mutex_.
    bool adopt_digest_generation_locked(uint64_t generation) const;
    std::optional<IndexCache::Digest> cached_file_digest(const std::string& uri,
                                                         uint64_t generation) const;
    /// Digests of the bytes the burst's parses actually read, as opposed to
    /// what the files hold now.
    ///
    /// These are two different questions and one memo cannot answer both.  The
    /// preload asks what is on disk, because that is what it validates a stored
    /// shard against.  The store path asks what this parse read, because that
    /// is what the shard it is about to write was built from.  Answering the
    /// second from a disk read -- which is what sharing one memo did -- keys a
    /// shard built from bytes A on the digest of bytes B whenever the file
    /// moves in between, and the writer thread makes that window seconds wide
    /// on a large project.  The result is a false hit that no later launch can
    /// detect.
    ///
    /// Filled from DocumentState::parsed_digests, first parse of a file wins,
    /// and cleared with the generation like the disk memo beside it.
    mutable std::unordered_map<std::string, IndexCache::Digest> index_cache_parsed_digests_;
    void remember_parsed_digests(const DocumentState& state, uint64_t generation) const;
    std::optional<IndexCache::Digest> parsed_file_digest(const std::string& uri,
                                                         uint64_t generation) const;

    /// Shard writes, drained by one dedicated thread.
    ///
    /// A cache write is an optimization for the *next* launch, so it must never
    /// delay this one.  Done inline in the worker it did exactly that: on a
    /// 5953-shard project the serializing and writing of 116 MB sat between the
    /// last parse and the publish, and cost 44% of a cold start.  Workers now
    /// hand the finished shard over -- a shared_ptr and two strings -- and go
    /// back to parsing.
    struct PendingShardWrite {
        /// No shard to write: a request to sweep the cache directory, which the
        /// preload queues once per burst.  A launch that reuses everything
        /// writes nothing, and that is exactly the launch a stale shard
        /// survives -- so the sweep cannot hang off a write.
        bool prune_only{false};
        std::string uri;
        std::shared_ptr<const SyntaxIndex> index;
        std::vector<IncludeResolution> include_resolutions;
        std::string extra_dependency_uri;
        bool stands_alone{false};
        uint64_t generation{0};
        /// Files the preload knows are on disk, for prune_only entries: their
        /// shards are skipped by name instead of opened to read one back.
        std::unordered_set<std::string> live_uris;
    };
    void queue_shard_write(PendingShardWrite write) const;
    void index_cache_writer_loop() const;
    /// Perform every queued shard write on the calling thread.
    ///
    /// Only used when there is no writer thread, which is the one-CPU case:
    /// see queue_shard_write().  Called once a burst has published its index,
    /// and by wait_for_index_cache_writes_idle() so the queue can never be left
    /// with nobody to drain it.
    void drain_shard_writes_inline() const;
    /// True when writes go on the queue with no thread behind it.
    bool shard_writes_need_inline_drain() const;

    mutable std::mutex index_cache_write_mutex_;
    mutable std::condition_variable index_cache_write_cv_;
    mutable std::deque<PendingShardWrite> index_cache_write_queue_;
    mutable std::thread index_cache_writer_;
    mutable bool index_cache_writer_stop_{false};
    mutable bool index_cache_writing_{false};
    /// Shards a worker has committed and will hand over, but has not yet.
    ///
    /// A worker finishes a file inside map_mutex_ -- decrementing the active
    /// count and waking wait_for_background_index_idle() -- and only then, with
    /// the lock released, queues that file's shards.  Without this counter the
    /// two waits in sequence are not a barrier: the first returns as the last
    /// worker leaves the lock, and the second looks at a queue the worker has
    /// not reached yet, finds it empty, and reports the cache flushed.  The
    /// reservation is taken while the worker still holds map_mutex_, so
    /// "drained" cannot be observed in between.
    mutable size_t index_cache_writes_reserved_{0};
    void reserve_shard_writes(size_t count) const;
    void prune_cache_once_per_generation(uint64_t generation,
                                         const std::unordered_set<std::string>& live_uris) const;
    /// Generation whose shards have been swept for sources that no longer
    /// exist, so it happens once per burst rather than once per write.
    mutable uint64_t index_cache_pruned_generation_{std::numeric_limits<uint64_t>::max()};

public:
    /// Block until every shard write a finished parse will make has been
    /// flushed, including the ones not handed over yet.  Only tests need this:
    /// the server has no reason to wait for a cache that exists for the next
    /// launch.
    void wait_for_index_cache_writes_idle() const;

private:
    // Guarded by its own mutex, never by map_mutex_: workers touch it while
    // parsing, which happens outside the analyzer lock.
    mutable HeaderTextCache background_header_texts_;
    // Also guarded by its own mutex, and for the same reason: make_state()
    // parses outside map_mutex_.
    mutable OpenParseHeaderCache open_parse_header_texts_;

    /// Shards for `include`d headers, one per header rather than one per
    /// including file.
    ///
    /// A header's declarations and reference occurrences are identical no matter
    /// which file pulled it in, so rebuilding them inside every dependent is pure
    /// duplication: 60 modules sharing two large headers measured 7.9 s and
    /// 1.8 GB where the unique work is ~2.0 s.  The first worker to parse a file
    /// that includes a header claims it, builds its shard from a parse of the
    /// header on its own, and every later dependent skips the header entirely.
    /// The warmup gate above is what makes "every later dependent" mean all of
    /// them rather than whichever ones the scheduler had not started yet.
    ///
    /// Both maps are guarded by map_mutex_ rather than a lock of their own.  The
    /// claim has to be taken in the same critical section that commits a shard,
    /// and HeaderTextCache's rule — never take its mutex under map_mutex_ —
    /// exists precisely to avoid the nested-lock shape that a second mutex here
    /// would reintroduce.  Both are cleared when the generation is bumped, so a
    /// changed header is always re-indexed.
    mutable std::unordered_map<std::string, ExtraFileCacheEntry> background_header_shards_;
    mutable std::unordered_set<std::string> background_header_claims_;
    /// Headers whose shard was built from a parse of the header on its own, so
    /// that shard is the authoritative record of their declarations.  Only those
    /// are safe to hand an open buffer as directives alone; see
    /// header_directives_only() and preload_open_parse_headers().  Cleared with
    /// the two maps above, so a re-indexed header re-earns its place.
    mutable std::unordered_set<std::string> standalone_header_uris_;
    mutable std::vector<std::thread> background_indexers_;
    mutable std::atomic<bool> background_stop_{false};

    struct VersionedSemanticDiags {
        uint64_t version{0};
        std::vector<ParseDiagInfo> diags;
    };
    std::unordered_map<std::string, VersionedSemanticDiags> semantic_diagnostics_;

    // Async parse worker state.
    struct ParseJob {
        std::string uri;
        // The placeholder snapshot enqueue_parse() installed, not a second copy
        // of its text.  The worker parses pending->text, and docs_ holds the
        // same snapshot until the reparse lands, so the buffer exists once
        // instead of once per queue it passes through.
        std::shared_ptr<const DocumentState> pending;
        uint64_t version{0};
    };

    std::function<void(const std::string&)> parse_complete_cb_;

    // Version tracking — under map_mutex_
    std::unordered_map<std::string, uint64_t> latest_version_;
    std::atomic<uint64_t> version_counter_{0};

    // Parse job queue — coalesced per URI
    std::unordered_map<std::string, ParseJob> parse_pending_;
    std::mutex parse_mutex_;
    std::condition_variable parse_cv_;
    /// Notified under map_mutex_ whenever a parse commits a snapshot into
    /// docs_, so get_parsed_state() can wait for the reparse it raced rather
    /// than poll for it.  Paired with map_mutex_, not parse_mutex_.
    mutable std::condition_variable parse_committed_cv_;
    std::atomic<bool> parse_stop_{false};
    /// Whether stop() has already run, so the destructor's call is a no-op.
    std::atomic<bool> stopped_{false};
    /// See set_parse_paused().  Read by the worker under parse_mutex_.
    std::atomic<bool> parse_paused_{false};
    std::thread parse_worker_;

    void parse_worker_loop();
};
