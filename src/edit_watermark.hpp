#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

/// How many `textDocument/didChange` notifications the transport has *read* for
/// a document, against how many its handler has *run*.
///
/// LspCpp reads framed messages on one thread and posts each of them to a
/// worker pool that this server sizes at one, so messages are handled strictly
/// in the order they arrived.  A request handler therefore sees the document
/// exactly as the notifications before it left it, and has no way to tell that
/// the user has typed again since: that keystroke is a message the reader has
/// already taken off the wire but the handler has not reached.  It is not
/// visible anywhere downstream either — the pending work sits in asio's own
/// task queue, which has no introspection API, and each entry is an unparsed
/// string.
///
/// Counting both ends makes the difference observable.  `received` runs ahead of
/// `dispatched` exactly while an edit to that document is in flight behind
/// whatever is being answered right now, and the gap closes as soon as the
/// notification's own handler runs.  That is what lets a whole-file request
/// notice that its answer is obsolete before spending the milliseconds to
/// compute it.
///
/// The two counters are fed from different threads — observe() from the reader,
/// on_dispatch() from the request worker — so the map is guarded.  Reading it is
/// one hash lookup in front of a request that costs milliseconds.
class EditWatermark {
public:
    /// Record a message the transport has just read, before it is queued.
    ///
    /// Runs on the reader thread, which must not be held up: anything that is
    /// not a didChange is rejected by a substring test before the JSON is
    /// touched.  This server negotiates incremental sync, so the messages that
    /// do get parsed are a few hundred bytes.
    void observe(const std::string& raw_message);

    /// Record that a didChange for @p uri is now being handled.  Call it before
    /// any early return in that handler: the counters only mean anything while
    /// they describe the same stream of messages.
    void on_dispatch(const std::string& uri);

    /// True when the transport has already read an edit to @p uri that this
    /// thread has not run yet.
    bool superseded(const std::string& uri) const;

    void forget(const std::string& uri);

private:
    struct Counts {
        uint64_t received{0};
        uint64_t dispatched{0};
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Counts> counts_;
};
