#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

/// Request ids the client has asked to cancel.
///
/// A client sends `$/cancelRequest` when the answer has stopped being wanted --
/// the cursor moved, the buffer changed, the popup closed.  Answering anyway
/// costs the whole computation and, on a server that answers one request at a
/// time, delays whatever is behind it.  LSP's reply for a cancelled request is
/// `RequestCancelled`, which releases the client's pending entry without
/// pretending the work was done.
///
/// Recorded from the thread that reads messages, before the request is handed
/// to a worker.  That is the only place where a cancel can overtake the request
/// it cancels, which is the case worth catching: by the time a cancel reaches a
/// handler queue it is usually behind the work it was meant to stop.
///
/// The transport's own cancel bookkeeping is not reachable from here --
/// `RemoteEndPoint::getCancelMonitor()` is private -- so this reads the raw
/// message itself.  It is a substring test in front of a JSON parse, on the
/// thread that must not be held up, so the test comes first.
class CancelledRequests {
public:
    /// Note a `$/cancelRequest` if that is what @p raw_message is.
    void observe(std::string_view raw_message) {
        if (raw_message.find(kMethod) == std::string_view::npos)
            return;
        // The substring is a filter, not the test.  It matches anywhere in the
        // message, including inside the document text a didChange carries, and
        // the scan below then looked for an `"id"` after `"params"` without
        // ever checking what the method was -- so a buffer holding both that
        // string and a quoted `"id"` could have an unrelated request's id
        // recorded as cancelled, and that request answered with
        // RequestCancelled instead of a result.  EditWatermark::observe() does
        // this correctly for its own method; this is the same check.
        if (!method_is_cancel(raw_message))
            return;
        auto id = id_from_cancel(raw_message);
        if (id.empty())
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        // Bounded: a client that cancels without the request ever arriving --
        // it was answered first, or never sent -- would otherwise grow this
        // forever.  Cancels are rare and ids monotonic, so dropping the whole
        // set when it gets implausible costs at most a few late cancels.
        if (ids_.size() >= kMaxTracked)
            ids_.clear();
        ids_.insert(std::move(id));
    }

    /// Whether @p id was cancelled.  Consumes it: a request is answered once,
    /// so the record has done its job and should not outlive it.
    bool take(const std::string& id) {
        if (id.empty())
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return ids_.erase(id) > 0;
    }

    /// Key for a request id, matching what `id_from_cancel()` produces.
    /// Rendered with the type in it, because JSON-RPC lets an id be a number or
    /// a string and `1` is not the same request as `"1"`.
    static std::string key(int value) { return "i" + std::to_string(value); }
    static std::string key(std::string_view value) { return "s" + std::string(value); }

private:
    static constexpr size_t kMaxTracked = 4096;
    static constexpr std::string_view kMethod = "$/cancelRequest";

    /// Whether the message's `"method"` member *is* the cancel method.
    ///
    /// Still a scan rather than a JSON parse, for the reason the whole class is
    /// one: this runs on the thread that reads messages and the shape is fixed
    /// by the protocol.
    static bool method_is_cancel(std::string_view message) {
        const auto key = message.find("\"method\"");
        if (key == std::string_view::npos)
            return false;
        auto at = message.find(':', key + 8);
        if (at == std::string_view::npos)
            return false;
        ++at;
        while (at < message.size() && (message[at] == ' ' || message[at] == '\t'))
            ++at;
        if (at >= message.size() || message[at] != '"')
            return false;
        ++at;
        return message.compare(at, kMethod.size(), kMethod) == 0 &&
               at + kMethod.size() < message.size() && message[at + kMethod.size()] == '"';
    }

    /// The `id` member of a `$/cancelRequest`'s params, as a key.
    ///
    /// Deliberately a scan rather than a JSON parse: this runs on the reader
    /// thread for every message carrying that substring, and the shape is fixed
    /// by the protocol -- `"params":{"id":<number or string>}`.
    static std::string id_from_cancel(std::string_view message) {
        const auto params = message.find("\"params\"");
        if (params == std::string_view::npos)
            return {};
        auto at = message.find("\"id\"", params);
        if (at == std::string_view::npos)
            return {};
        at = message.find(':', at + 4);
        if (at == std::string_view::npos)
            return {};
        ++at;
        while (at < message.size() && (message[at] == ' ' || message[at] == '\t'))
            ++at;
        if (at >= message.size())
            return {};

        if (message[at] == '"') {
            const auto end = message.find('"', at + 1);
            if (end == std::string_view::npos)
                return {};
            return key(message.substr(at + 1, end - at - 1));
        }
        const bool negative = message[at] == '-';
        size_t     digits   = at + (negative ? 1 : 0);
        size_t     end      = digits;
        while (end < message.size() && message[end] >= '0' && message[end] <= '9')
            ++end;
        if (end == digits)
            return {};
        return "i" + std::string(message.substr(at, end - at));
    }

    std::mutex                      mutex_;
    std::unordered_set<std::string> ids_;
};
