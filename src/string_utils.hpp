#pragma once
#include "position_encoding.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

inline std::string trim_copy(std::string text) {
    auto first = std::find_if_not(text.begin(), text.end(),
                                   [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(),
                                  [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

/// Count the UTF-16 code units in a UTF-8 slice.
///
/// LSP measures `Position.character` in UTF-16 code units, while every byte
/// column slang reports is a byte count, so this is the conversion that sits
/// between them: take the bytes of a line up to a byte column and re-measure
/// them here.  clangd calls the same primitive `lspLength()` and routes every
/// SourceLocation through it for exactly this reason.
///
/// ASCII is the overwhelmingly common case and costs one predicted branch per
/// byte.  Malformed UTF-8 is deliberately counted as one byte / one UTF-16 unit
/// rather than rejected, so a partially-edited or mis-detected buffer still
/// yields monotonic offsets instead of an error the caller cannot act on.
inline size_t utf16_length(std::string_view text) {
    size_t units = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        // Whole-word ASCII skip.  Source lines are ASCII almost everywhere, and
        // a column conversion runs from the line start for every indexed token,
        // so this is the loop that decides the cost.  A set high bit in any of
        // the eight bytes means a multi-byte sequence starts somewhere in them;
        // fall through to the per-byte walk and let it find the boundary.
        while (pos + 8 <= text.size()) {
            uint64_t chunk;
            std::memcpy(&chunk, text.data() + pos, sizeof(chunk));
            if (chunk & 0x8080808080808080ULL)
                break;
            units += 8;
            pos += 8;
        }
        if (pos >= text.size())
            break;

        const unsigned char c = static_cast<unsigned char>(text[pos]);
        if (c < 0x80) { // ASCII: one byte, one UTF-16 unit.
            ++units;
            ++pos;
            continue;
        }
        int bytes = 1;
        int width = 1;
        if ((c & 0xE0) == 0xC0) {
            bytes = 2;
        } else if ((c & 0xF0) == 0xE0) {
            bytes = 3;
        } else if ((c & 0xF8) == 0xF0) {
            bytes = 4;
            width = 2; // Astral plane: encoded as a UTF-16 surrogate pair.
        }

        bool valid_sequence = pos + static_cast<size_t>(bytes) <= text.size();
        for (int i = 1; valid_sequence && i < bytes; ++i) {
            const unsigned char cc = static_cast<unsigned char>(text[pos + static_cast<size_t>(i)]);
            valid_sequence = (cc & 0xC0) == 0x80;
        }
        if (!valid_sequence) {
            bytes = 1;
            width = 1;
        }

        units += static_cast<size_t>(width);
        pos += static_cast<size_t>(bytes);
    }
    return units;
}

/// UTF-16 column -> byte offset, the inverse of utf16_length().
///
/// Advances from `pos` by `col` UTF-16 code units without leaving the current
/// line, and returns the resulting byte offset.  This is the incoming half of
/// the LSP position boundary: a client sends UTF-16 columns, and anything that
/// slices the document text needs bytes.  clangd calls the same primitive
/// `measureUnits()`.
///
/// Malformed UTF-8 is treated as one byte / one UTF-16 unit so the walk stays
/// monotonic and can never step past the buffer or over unrelated text.
inline size_t utf16_col_to_byte_offset(std::string_view text, size_t pos, int col) {
    int units = 0;
    while (pos < text.size() && text[pos] != '\n' && units < col) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        int bytes, extra;
        if      (c < 0x80)           { bytes = 1; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { bytes = 2; extra = 0; }
        else if ((c & 0xF0) == 0xE0) { bytes = 3; extra = 0; }
        else if ((c & 0xF8) == 0xF0) { bytes = 4; extra = 1; } // surrogate pair
        else                         { bytes = 1; extra = 0; } // continuation/invalid

        bool valid_sequence = pos + static_cast<size_t>(bytes) <= text.size();
        for (int i = 1; valid_sequence && i < bytes; ++i) {
            const unsigned char cc = static_cast<unsigned char>(text[pos + static_cast<size_t>(i)]);
            valid_sequence = (cc & 0xC0) == 0x80;
        }
        if (!valid_sequence) {
            bytes = 1;
            extra = 0;
        }

        if (units + 1 + extra > col)
            break;
        units += 1 + extra;
        pos += static_cast<size_t>(bytes);
    }
    return pos;
}

/// Count UTF-16 code units from byte offset `pos` until a newline or end of
/// string.  LSP positions use UTF-16 columns, while lazyverilog stores document
/// text as UTF-8, so this helper advances over one UTF-8 scalar at a time and
/// counts supplementary-plane scalars as two UTF-16 units.  Malformed UTF-8 is
/// deliberately treated as one byte / one UTF-16 unit to keep offset math
/// monotonic for partially-edited documents.
inline size_t utf16_units_until_newline(std::string_view text, size_t pos) {
    size_t units = 0;
    while (pos < text.size() && text[pos] != '\n') {
        unsigned char c = static_cast<unsigned char>(text[pos]);
        int bytes = 1;
        int width = 1;
        if (c < 0x80) {
            bytes = 1;
        } else if ((c & 0xE0) == 0xC0) {
            bytes = 2;
        } else if ((c & 0xF0) == 0xE0) {
            bytes = 3;
        } else if ((c & 0xF8) == 0xF0) {
            bytes = 4;
            width = 2;
        }

        bool valid_sequence = pos + static_cast<size_t>(bytes) <= text.size();
        for (int i = 1; valid_sequence && i < bytes; ++i) {
            unsigned char cc = static_cast<unsigned char>(text[pos + static_cast<size_t>(i)]);
            valid_sequence = (cc & 0xC0) == 0x80;
        }
        if (!valid_sequence) {
            bytes = 1;
            width = 1;
        }

        units += static_cast<size_t>(width);
        pos += static_cast<size_t>(bytes);
    }
    return units;
}

// ── The LSP column boundary ───────────────────────────────────────────────────
//
// The four functions above measure UTF-16 unconditionally, which is what they
// are for: they are the definition of the conversion, and the tests that pin it
// call them directly.  The four below are what the rest of the server uses.
// They answer in whatever unit this session negotiated, so a client that asked
// for UTF-8 is served byte offsets with no conversion at all and every caller
// stays as it was.
//
// The split is deliberate and the names are the whole point: `utf16_` says "a
// UTF-16 count, always", `lsp_` says "whatever the client is counting in".
// Anything reaching a Position, a Range or an index shard wants the second.

/// Width of @p text in LSP columns.
inline int lsp_column_width(std::string_view text) {
    return static_cast<int>(lsp_columns_are_bytes() ? text.size() : utf16_length(text));
}

/// Byte offset of LSP column @p col within the line starting at @p line_start.
inline size_t lsp_col_to_byte_offset(std::string_view text, size_t line_start, int col) {
    if (!lsp_columns_are_bytes())
        return utf16_col_to_byte_offset(text, line_start, col);
    if (col <= 0)
        return line_start;
    // Already a byte count, but still bounded by the line: a column past the end
    // of one must not run into the next, which is the guarantee the UTF-16 walk
    // gives and every caller slicing document text relies on.
    auto line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos)
        line_end = text.size();
    const size_t wanted = line_start + static_cast<size_t>(col);
    return wanted < line_end ? wanted : line_end;
}

/// LSP columns from byte offset @p pos until a newline or the end of @p text.
inline size_t lsp_columns_until_newline(std::string_view text, size_t pos) {
    if (!lsp_columns_are_bytes())
        return utf16_units_until_newline(text, pos);
    const auto line_end = text.find('\n', pos);
    return (line_end == std::string_view::npos ? text.size() : line_end) - pos;
}

/// Byte offset of the first character of 0-based @p line.
///
/// Returns the end of the text when the document has fewer lines than asked
/// for, so a position past the end clamps instead of failing: LSP clients
/// legitimately send one-past-the-end positions for an append at EOF.
inline size_t lsp_line_start_offset(std::string_view text, int line) {
    if (line <= 0)
        return 0;
    int cur = 0;
    size_t pos = 0;
    while (pos < text.size() && cur < line) {
        if (text[pos] == '\n')
            ++cur;
        ++pos;
    }
    return pos;
}

/// Byte offset of an incoming LSP position.
///
/// The one place a `Position` from the client becomes an index into the
/// document's UTF-8 bytes.  Every feature that slices document text at a
/// request position must come through here, because `Position.character` is a
/// count of UTF-16 code units and the text is UTF-8: on a line carrying any
/// non-ASCII character the two disagree, and a handler that indexes with the
/// raw column lands somewhere earlier in the line.
///
/// That is not a hypothetical.  Hover, completion and signature help each
/// walked to the line themselves and then added `character` as if it were a
/// byte count, so a Korean comment or a `µ` earlier on the line was enough to
/// make hover answer nothing and completion fall back to a keyword dump.  The
/// duplicated line walks are what let the three drift apart from the
/// incremental-sync path, which had it right, so the walk lives here too.
///
/// This is also the single switch point for `positionEncoding`: a client that
/// negotiates UTF-8 sends byte offsets and wants this to be the identity.
inline size_t lsp_position_to_byte_offset(std::string_view text, int line, int character) {
    return lsp_col_to_byte_offset(text, lsp_line_start_offset(text, line), character);
}

/// UTF-16 column of a byte offset that is known to lie on @p line_start's line.
///
/// The outgoing half of the boundary above, for positions the server computed
/// as byte offsets into its own text rather than reading off a slang location.
inline int lsp_column_from_byte_offset(std::string_view text, size_t line_start, size_t offset) {
    if (offset <= line_start)
        return 0;
    return lsp_column_width(text.substr(line_start, offset - line_start));
}



inline std::optional<std::string> read_file_text_optional(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;

    // Prefer a single allocation/read for regular files.  The fallback keeps the
    // helper robust for paths where the size cannot be queried or the stream is
    // not seekable.  Large RTL sources are common, so avoiding repeated string
    // growth keeps project/background parsing from wasting allocator work.
    //
    // Sized by seeking the handle that is already open rather than by asking the
    // filesystem about the path a second time: file_size() is another metadata
    // call for a question this stream can answer, and on a shared filesystem
    // that is a round trip per file read.
    in.seekg(0, std::ios::end);
    const auto hint = in.tellg();
    in.seekg(0, std::ios::beg);

    // tellg() is a hint, not a promise.  libstdc++ opens a directory
    // successfully and reports LLONG_MAX as its size, so sizing a string from it
    // outright throws bad_alloc -- on a background thread, with nothing to catch
    // it.  A filelist naming a directory is a user's typo, not a reason to take
    // the server down.  Cap what the hint may reserve and read anything larger
    // in chunks, which costs one extra pass for a file nobody has and cannot be
    // talked into an absurd allocation.
    constexpr std::streamoff kMaxSizeHint = std::streamoff{1} << 30; // 1 GiB
    if (in && hint >= 0 && hint <= kMaxSizeHint) {
        std::string text(static_cast<size_t>(hint), '\0');
        if (hint == 0)
            return text;
        in.read(text.data(), hint);
        text.resize(static_cast<size_t>(in.gcount()));
        return text;
    }

    // No usable hint: unseekable, or a size that cannot be true.  Read what the
    // stream actually gives.  An explicit read() loop rather than
    // istreambuf_iterator, because the iterator reaches basic_filebuf::underflow,
    // which throws on a directory whatever the stream's exception mask says.
    in.clear();
    in.seekg(0, std::ios::beg);
    std::string text;
    char chunk[64 * 1024];
    while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0)
        text.append(chunk, static_cast<size_t>(in.gcount()));
    return text;
}

inline std::string read_file_text_or_empty(const std::filesystem::path& path) {
    return read_file_text_optional(path).value_or(std::string{});
}

inline std::filesystem::path normalize_filesystem_path(const std::filesystem::path& path);

/// Record @p result for @p key and hand it back, so each return path below is
/// one line rather than three.
inline std::filesystem::path
cache_normalized(std::mutex& mutex, std::unordered_map<std::string, std::string>& cache,
                 std::string key, std::filesystem::path result) {
    std::lock_guard<std::mutex> lock(mutex);
    cache.insert_or_assign(std::move(key), result.string());
    return result;
}

inline std::filesystem::path normalize_filesystem_path(const std::filesystem::path& path) {
    // weakly_canonical resolves the longest existing prefix through canonical(),
    // which collapses symlinks (e.g. macOS /tmp -> /private/tmp) and Windows 8.3
    // short names (e.g. RUNNER~1 -> runneradmin) to a single stable spelling.
    // Any non-existing remainder (unsaved/virtual paths) is appended lexically.
    // Without this, two code paths that resolve the same on-disk file through
    // different OS APIs can disagree on its URI string even though they name
    // the same file.
    //
    // weakly_canonical stats every component of the path, so a deep project path
    // costs a whole chain of metadata calls.  On shared/HPC filesystems each of
    // those is a network round trip, and startup normalizes the same filelist,
    // include, and buffer paths over and over (once per configured file, again
    // per background parse, again per URI conversion).  Memoize by input
    // spelling: a path that resolves on disk does not change spelling while the
    // server is alive, and a not-yet-created file resolves through its existing
    // parent directory, so its result is stable across creation too.
    //
    // Memoizing the whole spelling is not enough on its own, because the files
    // of one project share their directories: every file still paid the full
    // walk of a prefix the walk before it had just resolved.  Measured on a
    // 61-file project, 439 readlinks against six distinct directories, every one
    // of them failing; the same project eight directories deeper cost 943 --
    // linear in files x depth, against a handful of distinct answers.
    //
    // So resolve the parent through this same memo and append the last
    // component.  The prefix is then walked once per directory rather than once
    // per file, and a second file in a directory costs a single lookup.
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, std::string> cache;

    auto key = path.string();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (const auto it = cache.find(key); it != cache.end())
            return std::filesystem::path(it->second);
    }

    std::error_code ec;
    std::filesystem::path result;

    // POSIX only, and the reason is the shortcut's correctness argument rather
    // than any call it makes: appending a name to an already-canonical parent
    // is the canonical spelling of the child *provided a symlink is the only
    // thing canonical() would have rewritten*.  On Windows it is not -- that
    // function also expands 8.3 short names (the RUNNER~1 -> runneradmin case
    // this comment block already had in mind) and settles case, and a component
    // needing either is not a symlink, so the leaf check below says "nothing to
    // resolve" and the short spelling survives.  Two paths to one file then
    // normalize differently, which is the single thing this function exists to
    // prevent; it cost the CI's Windows job two tests, one of them the
    // symlinked-directory case in test_references.cpp.
    //
    // Windows therefore keeps the full walk.  The cost this avoids is a
    // per-component metadata round trip on a shared/HPC filesystem, which is
    // not where that build runs.  A Windows fast path would need
    // GetLongPathNameW and a Windows machine to prove it on.
#ifndef _WIN32
    // The last component still has to be resolved itself -- a symlinked source
    // file is a real thing, and collapsing it is the point of this function --
    // but that is one lstat rather than one call per component.  "." and ".."
    // address the parent rather than naming a component, so they fall through
    // to the full walk, which already handles them.
    const auto parent = path.parent_path();
    const auto filename = path.filename();
    if (!parent.empty() && parent != path && !filename.empty() && filename != "." &&
        filename != "..") {
        const auto candidate = normalize_filesystem_path(parent) / filename;
        std::error_code link_ec;
        // Not a symlink (or not there at all) is the overwhelmingly common case,
        // and the answer is then the parent's canonical spelling plus this name.
        (void)std::filesystem::read_symlink(candidate, link_ec);
        if (link_ec)
            return cache_normalized(cache_mutex, cache, std::move(key),
                                    candidate.lexically_normal());
        // A symlink at the leaf: hand it to the full walk.  It re-resolves the
        // prefix, which is wasted, but it is correct and it is rare.
        result = std::filesystem::weakly_canonical(candidate, ec);
        if (!ec)
            return cache_normalized(cache_mutex, cache, std::move(key),
                                    result.lexically_normal());
    }
#endif

    result = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        result = result.lexically_normal();
    } else {
        result = std::filesystem::absolute(path, ec);
        if (ec)
            result = path;
        result = result.lexically_normal();
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache.insert_or_assign(std::move(key), result.string());
    }
    return result;
}

inline bool is_windows_drive_path(std::string_view text) {
    return text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) &&
           text[1] == ':';
}

inline int uri_hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

inline std::string percent_decode_uri_path(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = uri_hex_value(text[i + 1]);
            const int lo = uri_hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

/// Convert an LSP `file://` URI to an OS path string.
///
/// LSP clients such as VS Code send Windows paths as `file:///C:/repo/top.sv`.
/// A plain `substr(7)` conversion turns that into `/C:/repo/top.sv`, which is
/// not the same path on Windows and can make SourceManager file names disagree
/// with the document URI used by go-to-definition.  This helper keeps the POSIX
/// behavior unchanged (`file:///tmp/a.sv` -> `/tmp/a.sv`) while accepting common
/// Windows forms:
///
///   file:///C:/repo/top.sv     -> C:/repo/top.sv
///   file://server/share/a.sv   -> //server/share/a.sv
///
/// The returned string intentionally uses forward slashes for Windows paths;
/// `std::filesystem::path` accepts them on Windows, and keeping slashes stable
/// prevents URI/path round-trips from depending on the caller's formatting.
inline std::string path_from_file_uri(std::string_view uri) {
    constexpr std::string_view prefix = "file://";
    if (!uri.starts_with(prefix))
        return std::string(uri);

    std::string_view rest = uri.substr(prefix.size());

    // URI form with an authority, usually UNC: file://server/share/file.sv.
    // Drive-letter URIs are normally file:///C:/..., but tolerate file://C:/...
    // as a local path rather than interpreting "C:" as an authority.
    if (!rest.empty() && rest.front() != '/' && !is_windows_drive_path(rest)) {
        const auto slash = rest.find('/');
        const auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        if (authority == "localhost") {
            std::string path = slash == std::string_view::npos
                                   ? std::string{}
                                   : percent_decode_uri_path(rest.substr(slash));
            if (path.size() >= 3 && path[0] == '/' &&
                is_windows_drive_path(std::string_view(path).substr(1)))
                path.erase(path.begin());
            return path;
        }
        if (slash == std::string_view::npos)
            return "//" + percent_decode_uri_path(authority);
        return "//" + percent_decode_uri_path(authority) +
               percent_decode_uri_path(rest.substr(slash));
    }

    std::string path = percent_decode_uri_path(rest);

    // Windows local drive URI: file:///C:/repo/top.sv.  Remove the URI-only
    // leading slash so std::filesystem sees a drive path instead of a POSIX
    // absolute path named "/C:/...".  On POSIX this only affects Windows-style
    // file URIs, not native paths like file:///tmp/top.sv.
    if (path.size() >= 3 && path[0] == '/' && is_windows_drive_path(std::string_view(path).substr(1)))
        path.erase(path.begin());

    return path;
}

inline std::string uri_from_path(const std::filesystem::path& path) {
    std::string normalized = normalize_filesystem_path(path).string();
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // Windows absolute paths stringify as "C:/repo/top.sv" (or "C:\\..." before
    // slash normalization).  File URIs require an extra slash before the drive:
    // "file:///C:/repo/top.sv".  POSIX absolute paths already start with '/', so
    // the historical Linux result remains exactly "file:///tmp/top.sv".
    if (is_windows_drive_path(normalized))
        return "file:///" + normalized;
    return "file://" + normalized;
}

/// Split source text into logical lines without copying.  The returned
/// string_views refer to the input buffer, so callers must keep the source text
/// alive for as long as they use the views.  The behavior intentionally matches
/// the formatter/index helpers this replaces: a trailing newline produces a
/// final empty line, and an empty input produces one empty line.
inline std::vector<std::string_view> split_lines_view(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty())
        lines.push_back({});
    return lines;
}

/// Owning-string convenience wrapper for callers that edit lines in place or
/// store them independently of the original source buffer.
inline std::vector<std::string> split_lines_owned(std::string_view text) {
    const auto views = split_lines_view(text);
    std::vector<std::string> lines;
    lines.reserve(views.size());
    for (std::string_view line : views)
        lines.emplace_back(line);
    return lines;
}
