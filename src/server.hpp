#pragma once
#include "analyzer.hpp"
#include "config.hpp"
#include "cancelled_requests.hpp"
#include "edit_watermark.hpp"
#include "index_cache.hpp"
#include "project_root.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Forward declarations to avoid pulling in LspCpp headers here
class RemoteEndPoint;
class BackgroundCompiler;

class LazyVerilogServer {
  public:
    explicit LazyVerilogServer();
    ~LazyVerilogServer();

    /// Block until server exits (stdin closed or exit notification received).
    void run();

  private:
    void register_handlers();
    /// Move @p method's handler off the thread that reads messages.
    ///
    /// clangd's shape: one thread reads and dispatches, so notifications stay in
    /// the order they arrived and a document update is applied before anything
    /// behind it; the *work* of a read-only request is handed to a pool, and the
    /// reply goes back from whichever thread finished it, serialized by the
    /// transport's own send mutex.  See `ClangdLSPServer::MessageHandler` and
    /// its `ReplyOnce`.
    ///
    /// Only for requests that are pure functions of an immutable document
    /// snapshot.  Anything that mutates server state, or whose order relative to
    /// a notification matters, stays on the dispatch thread.
    void answer_off_the_dispatch_thread(const char* method);
    /// Wrap the transport's per-method JSON converters so a request whose
    /// params do not fit their fields is still answered.  See the comment at
    /// the definition; must run after every handler is registered, because the
    /// converters are what registering one installs.
    void harden_request_parsing();
    void publish_diagnostics(const std::string& uri);
    void clear_published_diagnostics_for_owner(const std::string& owner_uri);
    void publish_config_diagnostic(const ConfigWarning* warning);
    void request_inlay_hint_refresh();
    void configure_background_compiler();
    void schedule_background_compilation();

    /// Storage handed to the analyzer, or null when [index].cache is off --
    /// a null storage is what makes it run uncached.
    ///
    /// Not a root: the server no longer has one root to give.  Each file's
    /// shards go beside its own lazyverilog.toml, resolved by
    /// `root_resolver_`, and a file with no config above it goes to the user's
    /// cache directory instead of littering a tree it was never part of.
    std::shared_ptr<IndexCacheStorage> index_cache_storage() const {
        return config_.index.cache ? std::make_shared<IndexCacheStorage>(root_resolver_)
                                   : nullptr;
    }

    /// Decides which project any file belongs to, and therefore which config it
    /// is served with and where its shards live.  Shared with the storage the
    /// analyzer holds, so both answer from one cache of one walk.
    std::shared_ptr<ProjectRootResolver> root_resolver_ =
        std::make_shared<ProjectRootResolver>();

    /// The config governing @p uri: the nearest lazyverilog.toml above it, or
    /// built-in defaults when there is none.
    ///
    /// Never null, and immutable once returned -- a handler running on the
    /// worker pool holds it across its whole reply while a didChangeConfiguration
    /// replaces the cache entry underneath.  This is why it is a
    /// shared_ptr<const Config> and not a reference into a map.
    ///
    /// Cached per root, because inlay hints and folding ranges are answered at
    /// keystroke rate and parsing TOML there would be absurd.
    std::shared_ptr<const Config> config_for(std::string_view uri) const;

    /// Drop the per-root config cache and the resolver's decisions.  Called
    /// when a lazyverilog.toml is saved: which file it governs is the
    /// resolver's answer, and the answer can now be different.
    void invalidate_config_cache();

    /// Fold @p uri's project into the analyzer's parse inputs, if it has one
    /// this session has not seen.
    ///
    /// clangd discovers a project the same way -- from a file, not from the
    /// client -- and broadcasts it so the background index picks up its files.
    /// Here the discovery has to *merge*, because there is one Analyzer with
    /// one set of parse inputs, not one per project.  Returns whether anything
    /// changed.
    bool discover_project_for(std::string_view uri);

    /// Project roots already folded in by discover_project_for(), so a burst of
    /// didOpens in one project reloads its filelist once rather than per file.
    /// The empty entry stands for "no project", which is discovered once and
    /// then never again.
    std::unordered_set<std::string> discovered_roots_;
    /// Parse inputs accumulated across every discovered project, so that
    /// reloading one does not drop another's.
    std::vector<std::string> project_defines_;
    std::vector<std::string> project_include_dirs_;
    std::vector<std::string> project_files_;
    std::vector<uintmax_t> project_file_sizes_;

    mutable std::mutex config_cache_mutex_;
    /// Keyed by project root; the empty key is "no project", served defaults.
    mutable std::unordered_map<std::string, std::shared_ptr<const Config>> config_cache_;

    std::filesystem::path root_;
    std::string config_diagnostic_uri_;
    Config config_;


    Analyzer analyzer_;
    std::unique_ptr<BackgroundCompiler> background_compiler_;
    // Last observed textDocument version per open URI.  The server does not
    // predict WorkspaceEdits; it waits for the client to apply them and report
    // the resulting text through normal didChange notifications.
    std::unordered_map<std::string, int> document_versions_;
    // Edits the transport has read against edits this thread has run.  Fed from
    // the reader thread through RemoteEndPoint's message preview hook, which is
    // the only place a newer keystroke is visible while an older request is
    // still being answered.  See EditWatermark.
    EditWatermark edit_watermark_;
    // Request ids the client has withdrawn, noted from the reader thread so a
    // cancel that overtakes its request still catches it.  See CancelledRequests.
    CancelledRequests cancelled_requests_;
    // The folds last computed for each open buffer.
    //
    // When an edit is already in flight behind a foldingRange request, the folds
    // this request would compute are obsolete before they are serialized, and
    // the client has a request for the new text queued right behind it.  The
    // protocol's own guidance for that case is that a result computed on an
    // older state is still useful while an error is not -- see the note on
    // lsErrorCodes::ContentModified, which a server is explicitly told NOT to
    // send for a change it spots in its own unprocessed messages.  So the
    // superseded request is answered from here instead of recomputing.
    //
    // Guarded: the whole-file read-only requests are answered on a worker pool
    // (see answer_off_the_dispatch_thread()), so a fold reply can be stored
    // while didClose is erasing the entry on the dispatch thread.
    mutable std::mutex last_folding_result_mutex_;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<FoldingRange>>>
        last_folding_result_;
    // The buffers didClose has not yet taken back, under the same lock.
    //
    // The mutex alone stops the two threads corrupting the map; it does not stop
    // a fold reply *landing after* the erase, which strands that buffer's folds
    // for the life of the process.  Storing only for a URI still in here, in the
    // same critical section the erase takes, closes that: a reply that beats
    // didClose is erased by it, and one that loses finds no entry to write to.
    std::unordered_set<std::string> folding_result_live_uris_;
    std::unordered_map<std::string, std::unordered_set<std::string>> diagnostic_uris_by_owner_;

    // Background project indexing and optional semantic compilation can request
    // diagnostic / refresh notifications from worker threads, while normal LSP
    // didOpen/didChange handlers publish from the endpoint thread.  LspCpp
    // endpoint sends and diagnostic ownership bookkeeping are not treated as
    // concurrently mutable server state, so serialize those outbound paths here.
    std::mutex outbound_mutex_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
