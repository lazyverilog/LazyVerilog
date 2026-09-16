#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

/// Which units `Position.character` is measured in for this session.
///
/// LSP 3.17 lets the client offer encodings in `general.positionEncodings` and
/// the server pick one in `ServerCapabilities.positionEncoding`.  Before that
/// the protocol had exactly one answer, UTF-16, and a server that says nothing
/// still means it -- so Utf16 is the default and the only value a session can
/// hold without the client having offered an alternative.
///
/// Picking UTF-8 is what removes the conversion rather than merely making it
/// correct: the document is UTF-8, so a byte offset from the client is already
/// an index into it and a byte column of ours is already what it wants back.
enum class PositionEncoding { Utf16, Utf8 };

constexpr std::string_view position_encoding_name(PositionEncoding encoding) {
    return encoding == PositionEncoding::Utf8 ? "utf-8" : "utf-16";
}

/// The encoding this session negotiated.
///
/// Process-wide, and deliberately so.  The conversions are free functions
/// reached from the analyzer, every feature and the index builder, and the
/// alternative is threading one enum through all of them and every struct they
/// pass -- for a value that is fixed at `initialize` and never changes again.
/// One server runs per process; the setter exists for that one call and for
/// tests.
///
/// Relaxed ordering is enough: it is written once, on the thread that reads
/// `initialize`, before the handler that starts any indexing runs, and read
/// everywhere afterwards.  There is no other datum it has to be ordered against.
namespace position_encoding_detail {
inline std::atomic<PositionEncoding>& slot() {
    static std::atomic<PositionEncoding> value{PositionEncoding::Utf16};
    return value;
}
} // namespace position_encoding_detail

inline PositionEncoding negotiated_position_encoding() {
    return position_encoding_detail::slot().load(std::memory_order_relaxed);
}

inline void set_negotiated_position_encoding(PositionEncoding encoding) {
    position_encoding_detail::slot().store(encoding, std::memory_order_relaxed);
}

/// True when an LSP column is a count of bytes, so no conversion is needed.
inline bool lsp_columns_are_bytes() {
    return negotiated_position_encoding() == PositionEncoding::Utf8;
}

/// Sets the encoding for a scope and restores it, so a test can exercise one
/// without leaking it into the next.
class ScopedPositionEncoding {
public:
    explicit ScopedPositionEncoding(PositionEncoding encoding)
        : previous_(negotiated_position_encoding()) {
        set_negotiated_position_encoding(encoding);
    }
    ~ScopedPositionEncoding() { set_negotiated_position_encoding(previous_); }
    ScopedPositionEncoding(const ScopedPositionEncoding&) = delete;
    ScopedPositionEncoding& operator=(const ScopedPositionEncoding&) = delete;

private:
    PositionEncoding previous_;
};

/// The encoding to answer with, given what an `initialize` request offered.
///
/// Read out of the raw message rather than from the parsed request: lspcpp's
/// `lsClientCapabilities` carries `workspace`, `textDocument`, `window` and
/// `experimental` and has no `general`, so the field simply is not there to
/// look at.  Scanning the message is also what puts the answer in place at the
/// right time -- the transport hands every message to this before queueing it,
/// which is strictly before the `initialize` handler that configures the
/// project and starts indexing.
///
/// Returns nullopt when @p raw_message is not an `initialize`, when it offers
/// no encodings, or when it offers none this server implements; the caller
/// leaves the default alone, which is what the protocol says an unanswered
/// offer means.
std::optional<PositionEncoding> position_encoding_from_initialize(std::string_view raw_message);
