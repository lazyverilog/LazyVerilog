#include "filelist.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace {

static std::optional<size_t> find_filelist_slash_comment(std::string_view line) {
    // Filelists use // for comments, but LSP/VS Code users sometimes paste
    // file URIs such as file:///C:/repo/rtl/top.sv.  Treat a double slash that
    // immediately follows ':' as part of a URI/scheme (or a Windows-ish path)
    // rather than as a comment opener.  Ordinary trailing comments still work:
    //
    //   rtl/top.sv // comment       -> comment
    //   file:///C:/repo/top.sv      -> path
    for (size_t pos = line.find("//"); pos != std::string_view::npos;
         pos = line.find("//", pos + 2)) {
        if (pos > 0 && line[pos - 1] == ':')
            continue;
        return pos;
    }
    return std::nullopt;
}

std::string expand_env_vars(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        if (text[i] != '$') {
            out.push_back(text[i++]);
            continue;
        }

        const size_t dollar = i++;
        std::string name;
        if (i < text.size() && text[i] == '{') {
            const size_t begin = ++i;
            while (i < text.size() && text[i] != '}')
                ++i;
            if (i >= text.size()) {
                out.append(text.substr(dollar));
                break;
            }
            name = std::string(text.substr(begin, i - begin));
            ++i;
        } else {
            const size_t begin = i;
            while (i < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (!(std::isalnum(c) || text[i] == '_'))
                    break;
                ++i;
            }
            if (begin == i) {
                out.push_back('$');
                continue;
            }
            name = std::string(text.substr(begin, i - begin));
        }

        if (const char* value = std::getenv(name.c_str()))
            out += value;
        else
            out.append(text.substr(dollar, i - dollar));
    }

    return out;
}

std::filesystem::path resolve_from_filelist_dir(const std::filesystem::path& filelist_dir,
                                                std::string_view text) {
    auto path = std::filesystem::path(path_from_file_uri(expand_env_vars(text)));
    if (path.is_relative())
        path = filelist_dir / path;
    return std::filesystem::absolute(path).lexically_normal();
}

// ── Wildcards ───────────────────────────────────────────────────────────────
//
// A filelist entry may be a shell-style glob, expanded once when the filelist
// is read:
//
//   *      any run of characters within one path segment
//   ?      one character
//   [abc]  one character from the set; ranges ([a-z]) and negation ([!a], [^a])
//   **     a whole segment: zero or more directories
//
// Only the entry as written is a pattern.  The directory of the filelist that
// names it, and the value of an environment variable before the first wildcard,
// are literal: a project checked out under `proj[v2]/` must not turn every
// plain entry into a pattern.
//
// Matches are sorted, so the order a glob contributes does not depend on the
// directory iteration order.  A wildcard does not match a leading `.` unless
// the pattern segment itself starts with one, and `**` does not descend into
// hidden directories (.git, .cache) or through directory symlinks, and never
// walks one directory twice.  On Windows matching ignores case, as the
// filesystem does.  A pattern that matches nothing contributes nothing.
//
// A source entry (plain or `-v`) keeps only Verilog/SystemVerilog sources,
// so `rtl/*` and `rtl/**` do not hand a README to the parser (#212).

enum class GlobKind {
    Source,    ///< a plain or `-v` entry: `.sv` / `.v` files only
    Filelist,  ///< a `-f` entry: any file
    Directory, ///< a `+incdir+` entry
};

/// Names are matched in the filesystem's own encoding (UTF-16 on Windows), so
/// walking past a file whose name has no narrow spelling cannot throw.
using NativeChar = std::filesystem::path::value_type;
using NativeView = std::basic_string_view<NativeChar>;

NativeChar glob_fold(NativeChar c) {
#ifdef _WIN32
    return static_cast<NativeChar>(std::towlower(static_cast<std::wint_t>(c)));
#else
    return c;
#endif
}

/// The extensions a source glob keeps.  The same set an implicit project
/// indexes (`src/implicit_project.cpp`): headers are reached through
/// `include, and compiling one on its own would define its contents twice.
bool is_source_glob_match(const std::filesystem::path& path) {
    auto ext = path.extension().native();
#ifdef _WIN32
    for (auto& c : ext)
        c = glob_fold(c);
#endif
    return ext == std::filesystem::path(".sv").native() ||
           ext == std::filesystem::path(".v").native();
}

/// Match @p c against the bracket expression starting at @p pattern[open]
/// (which is '[').  Returns the index just past the closing ']', or npos when
/// the bracket is unterminated -- in which case '[' is an ordinary character.
template <class Char>
size_t match_glob_bracket(std::basic_string_view<Char> pattern, size_t open, Char c,
                          bool& matched) {
    size_t i = open + 1;
    bool negate = false;
    if (i < pattern.size() && (pattern[i] == Char('!') || pattern[i] == Char('^'))) {
        negate = true;
        ++i;
    }
    const auto in_range = [](Char lo, Char hi, Char ch) {
        if (lo <= ch && ch <= hi)
            return true;
#ifdef _WIN32
        if constexpr (sizeof(Char) > 1) {
            const auto lower = static_cast<Char>(std::towlower(static_cast<std::wint_t>(ch)));
            const auto upper = static_cast<Char>(std::towupper(static_cast<std::wint_t>(ch)));
            return (lo <= lower && lower <= hi) || (lo <= upper && upper <= hi);
        }
#endif
        return false;
    };

    bool hit = false;
    bool first = true;
    while (i < pattern.size()) {
        // A ']' right after '[' (or '[!') is a member, not the end.
        if (pattern[i] == Char(']') && !first) {
            matched = hit != negate;
            return i + 1;
        }
        first = false;
        const Char lo = pattern[i];
        if (i + 2 < pattern.size() && pattern[i + 1] == Char('-') && pattern[i + 2] != Char(']')) {
            hit = hit || in_range(lo, pattern[i + 2], c);
            i += 3;
        } else {
            hit = hit || in_range(lo, lo, c);
            ++i;
        }
    }
    return std::basic_string_view<Char>::npos;
}

template <class Char>
bool is_glob_segment(std::basic_string_view<Char> segment) {
    for (size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] == Char('*') || segment[i] == Char('?'))
            return true;
        bool ignored = false;
        if (segment[i] == Char('[') &&
            match_glob_bracket(segment, i, Char(0), ignored) !=
                std::basic_string_view<Char>::npos)
            return true;
    }
    return false;
}

/// Match one file name against one pattern segment.
bool glob_match_segment(NativeView pattern, NativeView name) {
    constexpr auto npos = NativeView::npos;
    if (!name.empty() && name.front() == NativeChar('.') &&
        (pattern.empty() || pattern.front() != NativeChar('.')))
        return false;

    size_t p = 0;
    size_t n = 0;
    size_t star_p = npos; // pattern index just past the last '*'
    size_t star_n = 0;    // name index that '*' is retried from
    while (n < name.size()) {
        if (p < pattern.size()) {
            const NativeChar pc = pattern[p];
            if (pc == NativeChar('*')) {
                star_p = ++p;
                star_n = n;
                continue;
            }
            if (pc == NativeChar('?')) {
                ++p;
                ++n;
                continue;
            }
            bool consumed = false;
            if (pc == NativeChar('[')) {
                bool matched = false;
                const auto end = match_glob_bracket(pattern, p, name[n], matched);
                if (end != npos) {
                    if (matched) {
                        p = end;
                        ++n;
                        consumed = true;
                    }
                } else if (name[n] == NativeChar('[')) {
                    ++p;
                    ++n;
                    consumed = true;
                }
            } else if (glob_fold(pc) == glob_fold(name[n])) {
                ++p;
                ++n;
                consumed = true;
            }
            if (consumed)
                continue;
        }
        // Mismatch: let the last '*' swallow one more character, or fail.
        if (star_p == npos)
            return false;
        p = star_p;
        n = ++star_n;
    }
    while (p < pattern.size() && pattern[p] == NativeChar('*'))
        ++p;
    return p == pattern.size();
}

bool is_globstar(const std::filesystem::path::string_type& segment) {
    return segment.size() == 2 && segment[0] == NativeChar('*') && segment[1] == NativeChar('*');
}

/// How many directories one `**` descends at most.  Real trees are far
/// shallower; this is only a last guard behind the visited set below.
constexpr size_t kMaxGlobstarDepth = 64;

struct GlobWalk {
    const std::vector<std::filesystem::path::string_type>& segments;
    std::vector<std::filesystem::path>& out;
    /// Directories a `**` has already walked, by (segment index, canonical
    /// path).  Not every runtime reports a link as one -- libc++ on Windows
    /// sees a junction as a plain directory, MinGW's libstdc++ every symlink --
    /// and a link back to an ancestor would otherwise repeat the subtree under
    /// a new spelling at every level, once per link: duplicate sources at
    /// best, an exponential walk at worst.  The same holds for a bind mount.
    std::set<std::pair<size_t, std::filesystem::path>> visited;
};

void glob_walk(GlobWalk& walk, const std::filesystem::path& base, size_t index,
               size_t depth = 0) {
    namespace fs = std::filesystem;
    const auto& segments = walk.segments;
    auto& out = walk.out;
    if (index == segments.size()) {
        out.push_back(base);
        return;
    }
    const auto& segment = segments[index];
    if (is_globstar(segment)) {
        std::error_code canonical_ec;
        auto canonical = fs::canonical(base, canonical_ec);
        if (canonical_ec || !walk.visited.emplace(index, std::move(canonical)).second)
            return;
        // Zero directories ...
        glob_walk(walk, base, index + 1);
        // ... or one more, staying on `**`.  A trailing `**` also names the
        // files it passes; the caller keeps whichever kind it asked for.
        if (depth >= kMaxGlobstarDepth)
            return;
        std::error_code ec;
        for (fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            const auto filename = it->path().filename();
            const auto& name = filename.native();
            if (!name.empty() && name.front() == NativeChar('.'))
                continue;
            // Only a real directory is descended into: not a symlink, and not
            // a Windows junction, which MSVC reports as its own file type.
            std::error_code type_ec;
            if (it->symlink_status(type_ec).type() == fs::file_type::directory)
                glob_walk(walk, it->path(), index, depth + 1);
            else if (index + 1 == segments.size())
                out.push_back(it->path());
        }
        return;
    }
    if (!is_glob_segment(NativeView(segment))) {
        glob_walk(walk, base / segment, index + 1);
        return;
    }
    std::error_code ec;
    for (fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!glob_match_segment(segment, it->path().filename().native()))
            continue;
        // A segment with more to come names a directory.  Asked here, not left
        // to the walk: Windows resolves `file.sv\..` lexically, as if it were one.
        std::error_code type_ec;
        if (index + 1 < segments.size() && !it->is_directory(type_ec))
            continue;
        glob_walk(walk, it->path(), index + 1);
    }
}

bool is_filelist_separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/// Where the written pattern starts: the offset of the first path segment of
/// @p text that holds a wildcard, or npos when it has none.  Environment
/// variable references are not looked into -- their values are literal.
size_t find_first_glob_segment(std::string_view text) {
    // Blank out `$NAME` and `${NAME}` so a name cannot look like a pattern.
    std::string written(text);
    for (size_t i = 0; i < written.size(); ++i) {
        if (written[i] != '$')
            continue;
        size_t end = i + 1;
        if (end < written.size() && written[end] == '{') {
            const auto close = written.find('}', end);
            if (close == std::string::npos)
                break;
            end = close + 1;
        } else {
            while (end < written.size() &&
                   (std::isalnum(static_cast<unsigned char>(written[end])) || written[end] == '_'))
                ++end;
        }
        std::fill(written.begin() + i, written.begin() + end, '_');
        i = end - 1;
    }

    // A Windows long-path prefix (`\\?\C:\...`) holds a '?' that is not a wildcard.
    size_t begin = 0;
    if (written.size() >= 4 && is_filelist_separator(written[0]) &&
        is_filelist_separator(written[1]) && written[2] == '?' && is_filelist_separator(written[3]))
        begin = 4;
    while (begin <= written.size()) {
        size_t end = begin;
        while (end < written.size() && !is_filelist_separator(written[end]))
            ++end;
        if (is_glob_segment(std::string_view(written).substr(begin, end - begin)))
            return begin;
        begin = end + 1;
    }
    return std::string_view::npos;
}

/// Resolve one filelist path entry, expanding wildcards.
///
/// An entry without wildcards resolves to exactly one path, existing or not, so
/// the caller's missing-file warning behaves as it always has.  A wildcard entry
/// resolves to the existing paths of @p kind it matches, sorted; none is fine.
std::vector<std::filesystem::path> resolve_filelist_entry(
    const std::filesystem::path& filelist_dir, std::string_view text, GlobKind kind) {
    namespace fs = std::filesystem;
    const auto glob_at = find_first_glob_segment(text);
    if (glob_at == std::string_view::npos)
        return {resolve_from_filelist_dir(filelist_dir, text)};

    // `rtl/*/` names directories, as in a shell, whatever the entry is for.
    const bool directories_only = is_filelist_separator(text.back());
    const auto is_wanted = [&](const fs::path& path) {
        std::error_code ec;
        if (kind == GlobKind::Directory || directories_only)
            return kind == GlobKind::Directory && fs::is_directory(path, ec);
        if (kind == GlobKind::Source && !is_source_glob_match(path))
            return false;
        return fs::is_regular_file(path, ec);
    };

    // Everything before the first wildcard segment resolves exactly as a plain
    // entry does (environment, file:// URI, relative to the filelist).
    const auto prefix = text.substr(0, glob_at);
    const auto base = prefix.empty() ? fs::absolute(filelist_dir).lexically_normal()
                                     : resolve_from_filelist_dir(filelist_dir, prefix);

    // The rest is walked segment by segment.  Not normalized first: `*/..`
    // has to find a directory for `*` before it can step back out of it.
    std::vector<fs::path::string_type> segments;
    for (const auto& part : fs::path(expand_env_vars(text.substr(glob_at)))) {
        auto segment = part.native();
        if (segment.empty() || segment == fs::path(".").native())
            continue;
        // `a/**/**/b` means `a/**/b`, and walking it as written repeats the
        // whole subtree once per extra `**`.
        if (is_globstar(segment) && !segments.empty() && is_globstar(segments.back()))
            continue;
        segments.push_back(std::move(segment));
    }

    std::vector<fs::path> candidates;
    GlobWalk walk{segments, candidates, {}};
    glob_walk(walk, base, 0);

    std::vector<fs::path> matches;
    for (auto& candidate : candidates) {
        if (!is_wanted(candidate))
            continue;
        auto normal = candidate.lexically_normal();
        // Every consumer keys files by their narrow spelling; a name the
        // narrow encoding cannot hold could not be indexed anyway, and must not
        // abort the rest of the filelist.  Some runtimes throw, others
        // substitute '?', so ask for a round trip.
        try {
            if (fs::path(normal.string()) != normal)
                continue;
        } catch (const std::exception&) {
            continue;
        }
        matches.push_back(std::move(normal));
    }
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

    // A file literally named `top[0].sv` is not matched by the pattern
    // `top[0].sv`; when the pattern finds nothing, take the name as written.
    // Not when a `..` follows the wildcard: `x/*/..` would collapse to an
    // existing `x` without anything matching `*` -- lexically here, and in the
    // Win32 path layer even when asked of the path as written.
    const bool steps_back = std::any_of(segments.begin(), segments.end(), [](const auto& s) {
        return s == fs::path("..").native();
    });
    if (matches.empty() && !steps_back) {
        auto literal = base;
        for (const auto& segment : segments)
            literal /= segment;
        if (is_wanted(literal))
            matches.push_back(literal.lexically_normal());
    }
    return matches;
}

// Backslashes are literal path characters (Windows paths such as
// C:\repo\top.sv), not escapes.  Line continuations are stripped before
// tokenizing; paths containing whitespace must be quoted.
std::vector<std::string> tokenize_filelist_line(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';

    for (const char c : line) {
        if (quote != '\0') {
            if (c == quote)
                quote = '\0';
            else
                current.push_back(c);
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        tokens.push_back(std::move(current));
    return tokens;
}

bool strip_line_continuation(std::string& line) {
    size_t end = line.find_last_not_of(" \t\r");
    if (end == std::string::npos || line[end] != '\\')
        return false;
    line.erase(end);
    return true;
}

bool option_takes_non_source_argument(std::string_view option) {
    static const std::unordered_set<std::string_view> options = {
        "-assert", "-cm",       "-fprofile-dir", "-kdb",    "-l",
        "-libmap", "-Mdir",     "-ntb_opts",     "-o",      "-P",
        "-top",    "-timescale", "-work",         "-y",
    };
    return options.contains(option);
}

struct VcodeLoader {
    VcodeResult result;
    std::unordered_set<std::string> visited_filelists;
    std::unordered_set<std::string> seen_files;
    std::unordered_set<std::string> seen_include_dirs;
};

void warn_missing(const std::filesystem::path& path, const std::filesystem::path& filelist) {
    std::cerr << "[lazyverilog] warning: " << path.string() << " (referenced in "
              << filelist.string() << ") not found on disk\n";
}

void add_file(VcodeLoader& loader, std::string path, uintmax_t size) {
    if (!loader.seen_files.insert(path).second)
        return;
    loader.result.files.push_back(std::move(path));
    loader.result.file_sizes.push_back(size);
}

/// Record @p path as a project source, warning if it is not on disk.
///
/// One metadata call, and only for a path not already recorded.  file_size()
/// answers both questions this needs -- an error means the file is missing or
/// unreadable, which is what the warning is for, and otherwise the size is what
/// the background index queue is ordered by.  Asking exists() here and
/// file_size() again later was two passes over every entry for one fact each,
/// and asking before the dedup charged a filelist that names a file twice (or
/// reaches it through two `-f` includes) once per mention: 51 metadata calls for
/// a list of 50 duplicates of one file.
///
/// The size is a scheduling hint read microseconds before the queue is filled,
/// so nothing depends on it still being current.
void add_file_checked(VcodeLoader& loader, const std::filesystem::path& path,
                      const std::filesystem::path& filelist) {
    auto key = path.string();
    if (loader.seen_files.contains(key))
        return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        warn_missing(path, filelist);
    add_file(loader, std::move(key), ec ? 0 : size);
}

void add_include_dir(VcodeLoader& loader, std::string dir) {
    if (loader.seen_include_dirs.insert(dir).second)
        loader.result.include_dirs.push_back(std::move(dir));
}

void load_vcode_file(const std::filesystem::path& filelist, VcodeLoader& loader) {
    const auto normalized_filelist =
        std::filesystem::absolute(filelist).lexically_normal().string();
    // The visited set is persistent, not a recursion stack: it breaks -f
    // cycles and also skips re-parsing a filelist reachable from two parents.
    if (!loader.visited_filelists.insert(normalized_filelist).second)
        return;

    std::ifstream input(filelist);
    if (!input) {
        std::cerr << "[lazyverilog] warning: filelist " << filelist.string()
                  << " not found or unreadable\n";
        return;
    }

    const auto filelist_dir = filelist.parent_path();
    std::string line;
    std::string logical_line;
    while (std::getline(input, line)) {
        if (strip_line_continuation(line)) {
            logical_line += line;
            logical_line += ' ';
            continue;
        }
        if (!logical_line.empty()) {
            logical_line += line;
            line = std::move(logical_line);
            logical_line.clear();
        }

        // Strip // and # comments
        if (auto pos = find_filelist_slash_comment(line))
            line.erase(*pos);
        if (auto pos = line.find('#'); pos != std::string::npos)
            line.erase(pos);

        auto item = trim_copy(line);
        if (item.empty())
            continue;

        const auto tokens = tokenize_filelist_line(item);
        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            if (token.empty())
                continue;

            // Nested filelists:
            //   -f child.f
            //   -F child.f
            //   -fchild.f
            //   -Fchild.f
            //   -f=child.f
            // Paths are resolved relative to the filelist that contains the -f.
            if (token == "-f" || token == "-F") {
                if (i + 1 < tokens.size()) {
                    for (const auto& nested :
                         resolve_filelist_entry(filelist_dir, tokens[++i], GlobKind::Filelist))
                        load_vcode_file(nested, loader);
                }
                continue;
            }
            if ((token.starts_with("-f") || token.starts_with("-F")) && token.size() > 2) {
                std::string_view nested_text(token);
                nested_text.remove_prefix(2);
                if (nested_text.starts_with("="))
                    nested_text.remove_prefix(1);
                if (!nested_text.empty()) {
                    for (const auto& nested :
                         resolve_filelist_entry(filelist_dir, nested_text, GlobKind::Filelist))
                        load_vcode_file(nested, loader);
                    continue;
                }
            }

            // Recognize simulator-style include-directory entries in the
            // filelist. The directory is not parsed as a source file; it is
            // passed to slang's SourceManager so explicit source files can
            // resolve `include "...".
            //
            // Supported forms:
            //   +incdir+rtl/include
            //   +incdir+/abs/include
            //   +incdir+dir_a+dir_b
            if (token.starts_with("+incdir+")) {
                std::string_view rest(token);
                rest.remove_prefix(std::string_view("+incdir+").size());
                while (!rest.empty()) {
                    const auto plus = rest.find('+');
                    auto dir_text =
                        plus == std::string_view::npos ? rest : rest.substr(0, plus);
                    if (!dir_text.empty()) {
                        for (const auto& dir : resolve_filelist_entry(filelist_dir, dir_text,
                                                                      GlobKind::Directory))
                            add_include_dir(loader, dir.string());
                    }
                    if (plus == std::string_view::npos)
                        break;
                    rest.remove_prefix(plus + 1);
                }
                continue;
            }

            // `-v file.v` names a Verilog library source file.  Index it as a
            // source file because it can contain module/package declarations
            // needed by navigation and lint.
            if (token == "-v") {
                if (i + 1 < tokens.size()) {
                    for (const auto& file :
                         resolve_filelist_entry(filelist_dir, tokens[++i], GlobKind::Source))
                        add_file_checked(loader, file, filelist);
                }
                continue;
            }

            // Skip other compiler options / flags.  Some options have a
            // following non-source argument; consume those explicitly so they
            // are not accidentally treated as source paths.
            if (token.starts_with("-")) {
                if (option_takes_non_source_argument(token) && i + 1 < tokens.size())
                    ++i;
                continue;
            }
            if (token.starts_with("+"))
                continue;

            for (const auto& file : resolve_filelist_entry(filelist_dir, token, GlobKind::Source))
                add_file_checked(loader, file, filelist);
        }
    }
}

} // namespace

std::string resolve_vcode_path(const std::filesystem::path& root, const Config& config) {
    if (config.design.vcode.empty())
        return {};
    auto filelist =
        std::filesystem::path(path_from_file_uri(expand_env_vars(config.design.vcode)));
    if (filelist.is_relative())
        filelist = root / filelist;
    return std::filesystem::absolute(filelist).lexically_normal().string();
}

VcodeResult load_vcode(const std::filesystem::path& root, const Config& config) {
    if (config.design.vcode.empty())
        return {};

    VcodeLoader loader;
    load_vcode_file(std::filesystem::path(resolve_vcode_path(root, config)), loader);
    // Sorted, so two launches over one tree report them in one order; the set
    // they come from is unordered.
    loader.result.filelists.assign(loader.visited_filelists.begin(),
                                   loader.visited_filelists.end());
    std::sort(loader.result.filelists.begin(), loader.result.filelists.end());
    return std::move(loader.result);
}
