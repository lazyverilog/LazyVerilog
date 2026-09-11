#pragma once

#include "string_utils.hpp"

#include <slang/text/SourceLocation.h>
#include <slang/text/SourceManager.h>

#include <string_view>

/// Byte column -> UTF-16 column, for every position that reaches an LSP client.
///
/// `SourceManager::getColumnNumber()` is a byte count: it subtracts the offset
/// of the last newline from the offset of the location.  `Position.character`
/// in LSP is a count of UTF-16 code units unless the server negotiates
/// otherwise, and lazyverilog cannot negotiate -- the vendored lspcpp has
/// neither `general.positionEncodings` on the client side nor
/// `positionEncoding` on the server side -- so the two must be reconciled here.
///
/// The conversion happens where the bytes are: an index shard retains no source
/// text, so a position stored for a closed file can never be converted
/// afterwards.  clangd reaches the same conclusion and documents it on
/// `SymbolLocation::Position`, whose column is UTF-16 for exactly this reason.
///
/// Cost is one pass over the bytes from the line start to the column, with a
/// predicted branch per ASCII byte.  That is what clangd pays per indexed
/// token, unconditionally and with no per-file fast path.
inline int utf16_column(const slang::SourceManager& sm, slang::SourceLocation location) {
    if (!location.valid())
        return 0;

    const size_t byte_col = sm.getColumnNumber(location);
    if (byte_col <= 1)
        return 0;

    // getColumnNumber() is 1-based, so the line starts this many bytes back.
    const size_t bytes_into_line = byte_col - 1;
    const size_t offset = location.offset();
    if (bytes_into_line > offset)
        return static_cast<int>(bytes_into_line);

    const std::string_view buffer = sm.getSourceText(location.buffer());
    if (buffer.empty() || offset > buffer.size())
        return static_cast<int>(bytes_into_line);

    return static_cast<int>(utf16_length(buffer.substr(offset - bytes_into_line, bytes_into_line)));
}
