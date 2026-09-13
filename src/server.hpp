#pragma once
#include "analyzer.hpp"
#include "config.hpp"
#include "cancelled_requests.hpp"
#include "edit_watermark.hpp"
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

    /// Project root handed to the analyzer's shard cache, or empty when
    /// [index].cache is off -- an empty root is what makes it run uncached.
    std::string index_cache_root() const {
        return config_.index.cache ? root_.string() : std::string{};
    }

    std::filesystem::path root_;
    std::string config_diagnostic_uri_;
    Config config_;

    /// `[folding].enable` and `[inlay_hint].enable`, mirrored out of config_.
    ///
    /// Both are read by handlers that run on the worker pool, while a
    /// didChangeConfiguration may be replacing config_ on the dispatch thread.
    /// Mirroring the two flags keeps that off the whole config's lifetime.
    std::atomic<bool> folding_enabled_{true};
    std::atomic<bool> inlay_hint_enabled_{true};

    /// What the initialize reply said for each per-keystroke capability, and
    /// whether the client will let us revise it.  Capabilities are normally
    /// exchanged once, so without dynamic registration a later config edit
    /// cannot reach the client and only takes effect on restart.  Neovim opts
    /// in for `inlayHint` but not for `foldingRange`.
    bool folding_advertised_{true};
    bool folding_dynamic_registration_{false};
    bool inlay_hint_advertised_{true};
    bool inlay_hint_dynamic_registration_{false};

    /// Send client/registerCapability or client/unregisterCapability so
    /// @p method matches @p want.  No-op when @p advertised already says so, or
    /// when @p client_supports is false -- then it only logs, naming
    /// @p config_key.
    void sync_dynamic_registration(const char* method, const char* registration_id,
                                   const char* config_key, bool want, bool client_supports,
                                   bool& advertised);

    /// Bring textDocument/foldingRange and textDocument/inlayHint into line with
    /// `[folding].enable` and `[inlay_hint].enable` after a config reload.
    void sync_folding_registration();
    void sync_inlay_hint_registration();
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
