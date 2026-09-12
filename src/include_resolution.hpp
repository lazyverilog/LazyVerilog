#pragma once

#include <string>
#include <vector>

/// How one `` `include `` directive resolved when a file was parsed.
///
/// The shard cache hashes every file a parse read, which catches an `include`
/// whose *target* changed.  It cannot catch an `include` that now finds a
/// different target: no file anyone hashed has changed, so the shard still
/// validates and the stale answer is served.  Two ways that happens, both
/// ordinary:
///
///   * a header a file already `include`s is created for the first time --
///     write the includer, then the header, which is the usual order;
///   * a header is added to a directory earlier in the search order than the
///     one the previous parse found, which is how an override directory is
///     meant to work.
///
/// Recording the decision is what makes it checkable.  Re-running the search is
/// a handful of stats against a memo, not a parse.
struct IncludeResolution {
    /// URI of the file the directive is written in.  slang searches that file's
    /// own directory first, so the answer depends on it.
    std::string from_uri;
    /// The spelling between the delimiters, exactly as written.
    std::string spelling;
    /// `` `include <x> `` rather than `` `include "x" ``.  A system include
    /// skips the including file's directory entirely.
    bool is_system{false};
    /// URI it resolved to, or empty when nothing was found.  An unresolved
    /// directive is recorded precisely because it is the case with no file to
    /// hash.
    std::string resolved_uri;

    bool operator==(const IncludeResolution&) const = default;
};
