#include "analyzer.hpp"
#include "lsp_position.hpp"
#include "cpu_budget.hpp"
#include "dynamic_file_index.hpp"
#include "syntax_index_shared.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/PreprocessorDiags.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/Glob.h>
#include <slang/text/SourceManager.h>
#include <unordered_map>
#include <unordered_set>

namespace {

bool perf_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LAZYVERILOG_TRACE_PERF");
        return value && *value && std::string_view(value) != "0";
    }();
    return enabled;
}

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxRtlTreeDepth = 256;

constexpr unsigned kMaxBackgroundIndexThreads = 8;

// How long the parse queue must stay empty before the parse worker rebuilds the
// live project shard of a buffer it has just reparsed.  Short enough that a user
// who stops typing sees project-wide features catch up immediately, long enough
// that it never runs twice inside one burst of keystrokes.
constexpr auto kLiveShardIdleDelay = std::chrono::milliseconds(150);

// Smallest header an open buffer's parse is willing to see as directives alone.
//
// The projection trades exactness of the buffer's own tree -- the header's
// declarations stop appearing in it, and features answer for them from the
// header's shard instead -- for not re-parsing the header on every keystroke.
// That trade is only worth making where the bulk is actually felt: a header this
// size costs milliseconds per character typed, while a small one costs
// microseconds and is worth keeping exact.
constexpr size_t kDirectivesOnlySeedBytes = 64u << 10;

// Milder than the background compiler's default of 10.  Project index warmup
// gates when cross-file features start answering, so it should yield to
// interactive work without being starved outright.
constexpr int kBackgroundIndexNiceValue = 5;


void log_perf(std::string_view label, Clock::time_point start) {
    if (!perf_trace_enabled())
        return;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    std::cerr << "[lazyverilog][perf] " << label << ": " << elapsed.count() << "us\n";
}

} // namespace

/// Resolve the URI a location belongs to, falling back to the owning document
/// when the location is not backed by a real file (macro internals, memory
/// buffers).
static std::string location_to_uri(const slang::SourceManager& sm, slang::SourceLocation location,
                                   const std::string& fallback_uri) {
    auto uri = uri_from_source_location(sm, location);
    return uri.empty() ? fallback_uri : uri;
}

static int saturating_lsp_int(size_t value) {
    constexpr auto max_int = static_cast<size_t>(std::numeric_limits<int>::max());
    return value > max_int ? std::numeric_limits<int>::max() : static_cast<int>(value);
}


static void cache_document_end_position(DocumentState& state) {
    size_t line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < state.text.size(); ++i) {
        if (state.text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    const size_t col = utf16_units_until_newline(state.text, line_start);
    state.end_line = saturating_lsp_int(line);
    state.end_character = saturating_lsp_int(col);
}

/// Resolve configured include-directory patterns to existing directories, once.
///
/// The parse path used to hand these to SourceManager::addUserDirectories(),
/// which globs the pattern and then runs weakly_canonical() over every match —
/// a stat per path component, per directory.  A SourceManager is built per
/// parse, so that ran again on every keystroke and once per project file during
/// indexing: with a few hundred include directories it dominated the edit path,
/// and on a shared/network filesystem each of those stats is a round trip.
///
/// Resolving here and passing the result as
/// PreprocessorOptions::additionalIncludePaths keeps the same search order —
/// slang tries the including file's own directory first, then these — with no
/// filesystem work left on the edit path.
static std::vector<std::filesystem::path>
resolve_include_dirs(const std::vector<std::string>& dirs) {
    std::vector<std::filesystem::path> resolved;
    resolved.reserve(dirs.size());
    for (const auto& dir : dirs) {
        if (dir.empty())
            continue;
        slang::SmallVector<std::filesystem::path> matches;
        std::error_code ec;
        // The same glob slang applied, so wildcard include paths keep working.
        // A pattern matching nothing is dropped and the error ignored, as
        // before: completion stays best-effort when the user has a stale config
        // path, and missing include diagnostics still surface from the parse.
        slang::svGlob({}, dir, slang::GlobMode::Directories, matches,
                      /*expandEnvVars=*/false, ec);
        resolved.insert(resolved.end(), matches.begin(), matches.end());
    }
    return resolved;
}

static void collect_parse_diagnostics(DocumentState& state, const std::string& fallback_uri) {
    if (!state.tree)
        return;

    // Format diagnostics immediately while the SyntaxTree arena is alive.
    // Do NOT copy slang::Diagnostic objects — their ConstantValue args can
    // contain internal pointers that are not safely copyable.
    const auto& diags = state.tree->diagnostics();
    auto& sm = state.tree->sourceManager();
    slang::DiagnosticEngine engine(sm);
    for (const auto& d : diags) {
        ParseDiagInfo info;
        try {
            auto loc = d.location.valid() ? sm.getFullyExpandedLoc(d.location) : d.location;
            info.uri = location_to_uri(sm, loc, fallback_uri);
            if (loc.valid() && sm.isFileLoc(loc)) {
                size_t ln = sm.getLineNumber(loc);
                info.line = ln > 0 ? (int)ln - 1 : 0;
                info.col = utf16_column(sm, loc);
            }
        } catch (...) {
        }
        auto sev = slang::getDefaultSeverity(d.code);
        if (sev == slang::DiagnosticSeverity::Error || sev == slang::DiagnosticSeverity::Fatal)
            info.severity = 1;
        else if (sev == slang::DiagnosticSeverity::Warning)
            info.severity = 2;
        else
            info.severity = 3;
        try {
            info.message = engine.formatMessage(d);
        } catch (...) {
            info.message = "(diagnostic format error)";
        }
        state.parse_diagnostics.push_back(std::move(info));
    }
}

struct OpenTextOverlay {
    std::string uri;
    std::string path;
    // Keep the immutable open-buffer snapshot alive instead of copying its full
    // text into every overlay vector.  didChange swaps DocumentState instances,
    // so this shared_ptr gives SourceManager a stable view for the duration of
    // the parse without retaining any closed/project ASTs beyond the open-buffer
    // operation that already needed them.
    std::shared_ptr<const DocumentState> state;
};

static void preload_open_text_overlays(slang::SourceManager& sm,
                                       const std::vector<OpenTextOverlay>& overlays,
                                       const std::string& excluded_path = {}) {
    for (const auto& overlay : overlays) {
        if (overlay.path.empty() || overlay.path == excluded_path)
            continue;
        // Seed SourceManager's file cache with unsaved open-buffer text before
        // slang resolves `include directives.  This lets a dependent file such
        // as memory.sv see an unsaved rename in params.svh without polling or
        // reading every project file.  Only already-open buffers are copied.
        if (!overlay.state)
            continue;
        sm.assignText(std::string_view(overlay.path), std::string_view(overlay.state->text));
    }
}

/// Paths that the header text cache must not touch, in either direction.
///
/// The file being parsed belongs here because it must come from disk — a
/// filelist entry can also be a header somewhere else.  Open-buffer paths belong
/// here for two reasons: SourceManager throws if the same path is assigned
/// twice, and unsaved editor text must win over anything cached.
///
/// Excluding overlays from *storing* as well keeps the invariant simple: the
/// cache only ever holds on-disk text.  Caching unsaved text would otherwise
/// survive the buffer being closed, since closing a file that is not itself a
/// filelist entry does not bump the background generation.
static std::unordered_set<std::string_view>
header_cache_excluded_paths(const std::vector<OpenTextOverlay>& open_overlays,
                            const std::string& own_path) {
    std::unordered_set<std::string_view> excluded;
    excluded.reserve(open_overlays.size() + 1);
    excluded.insert(own_path);
    for (const auto& overlay : open_overlays) {
        if (!overlay.path.empty())
            excluded.insert(overlay.path);
    }
    return excluded;
}

/// Seed already-known header text so slang resolves `include from its own cache
/// instead of the filesystem.  See HeaderTextCache for why this is scoped to one
/// indexing burst, and why only headers the burst widely shares are offered.
///
/// The exclusion check is not redundant with store_header_texts() skipping open
/// buffers: a file can be opened *during* a burst, and didOpen does not bump the
/// background generation, so the cache can still hold that path's disk text when
/// the next file in the same burst is parsed.
/// @param seeded  receives every path this assigned, mapped to the digest of
///        the header as first read.  A seeded buffer holds whatever the cache
///        had -- for a widely shared header, its directives alone -- so hashing
///        what slang ends up with would key a shard on a reduction of its own
///        dependency.  The cache carries the real digest for exactly this
///        reason; see HeaderTextCache::Entry::digest.
static void preload_cached_header_texts(slang::SourceManager& sm, HeaderTextCache& cache,
                                        uint64_t generation,
                                        const std::unordered_set<std::string_view>& excluded,
                                        std::unordered_map<std::string, IndexCache::Digest>& seeded) {
    for (const auto& candidate : cache.seed_candidates(generation)) {
        if (!candidate.text || excluded.contains(candidate.path))
            continue;
        sm.assignText(std::string_view(candidate.path), std::string_view(*candidate.text));
        seeded.emplace(candidate.path, candidate.digest);
    }
}

static std::string header_directives_only(std::string_view text);

/// Record how every `include in @p tree resolved, so a later launch can tell
/// whether the same directive would now find something else.
///
/// slang reports the ones that succeeded through the tree's include metadata,
/// and the ones that found nothing only as a diagnostic -- which is the half
/// that matters most here, because an unresolved `include leaves no file behind
/// for the shard key to hash.  See IncludeResolution.
static std::vector<IncludeResolution> collect_include_resolutions(
    slang::syntax::SyntaxTree& tree, const slang::SourceManager& sm,
    const std::string& own_uri) {
    std::vector<IncludeResolution> resolutions;

    // The file a directive is written in, which is the directory slang searches
    // first.  A directive inside an `include`d header belongs to that header.
    const auto directive_origin = [&](slang::SourceLocation loc) {
        const auto expanded = sm.getFullyExpandedLoc(loc);
        const auto& full_path = sm.getFullPath(expanded.buffer());
        return full_path.empty() ? own_uri : uri_from_path(full_path);
    };

    for (const auto& include : tree.getIncludeDirectives()) {
        if (!include.syntax)
            continue;
        std::string resolved;
        if (include.buffer.id.valid()) {
            const auto& full_path = sm.getFullPath(include.buffer.id);
            if (!full_path.empty())
                resolved = uri_from_path(full_path);
        }
        resolutions.push_back(IncludeResolution{
            .from_uri = directive_origin(include.syntax->getFirstToken().location()),
            .spelling = std::string(include.path),
            .is_system = include.isSystem,
            .resolved_uri = std::move(resolved),
        });
    }

    for (const auto& diagnostic : tree.diagnostics()) {
        if (diagnostic.code != slang::diag::CouldNotOpenIncludeFile)
            continue;
        // The formatter renders "'<path>': <reason>"; the path is the first
        // argument, kept as a string by the emitter above.
        if (diagnostic.args.empty())
            continue;
        const auto* spelling = std::get_if<std::string>(&diagnostic.args.front());
        if (!spelling || spelling->empty())
            continue;
        resolutions.push_back(IncludeResolution{
            .from_uri = directive_origin(diagnostic.location),
            .spelling = *spelling,
            // A system include that found nothing is recorded as one: the
            // search it would re-run is a different search.
            .is_system = spelling->front() == '<',
            .resolved_uri = {},
        });
    }

    std::sort(resolutions.begin(), resolutions.end(),
              [](const IncludeResolution& a, const IncludeResolution& b) {
                  return std::tie(a.from_uri, a.spelling, a.is_system) <
                         std::tie(b.from_uri, b.spelling, b.is_system);
              });
    resolutions.erase(std::unique(resolutions.begin(), resolutions.end()), resolutions.end());
    return resolutions;
}

/// The `include`s written in @p uri itself.
///
/// A parse records every directive in its tree, headers included.  A shard is
/// per file, so it carries only its own: a nested header's directives belong to
/// that header's shard, and the preload requires that shard to be a hit before
/// it will reuse anything that included it, so the two compose.
static std::vector<IncludeResolution>
resolutions_written_in(const std::vector<IncludeResolution>& all, const std::string& uri) {
    std::vector<IncludeResolution> own;
    for (const auto& resolution : all) {
        if (resolution.from_uri == uri)
            own.push_back(resolution);
    }
    return own;
}

/// Digest the bytes this parse read, so the shard cache can key a shard on what
/// it was actually built from rather than on a later re-read of the file.
///
/// Only buffers slang loaded itself are digested: the file's own, and every
/// header it pulled in that was not seeded from the burst's text cache.  A
/// seeded buffer is whatever that cache held, which for a widely shared header
/// is its directives alone -- a digest of that would never match the file on
/// disk, and every shard depending on it would miss forever.  The parse that
/// first read such a header in full is the one that records it.
static void record_parsed_digests(
    DocumentState& state, std::string_view own_source,
    const std::unordered_map<std::string, IndexCache::Digest>& seeded) {
    if (!own_source.empty())
        state.parsed_texts.emplace(state.uri, own_source);
    if (!state.source_manager)
        return;
    for (const auto buffer : state.source_manager->getAllBuffers()) {
        const auto& full_path = state.source_manager->getFullPath(buffer);
        if (full_path.empty())
            continue;
        auto dependency_uri = uri_from_path(full_path);
        if (!state.include_dependency_set.contains(dependency_uri))
            continue;
        // A seeded header's digest is the one the cache carries for it, taken
        // when it was read in full; hashing the buffer would hash the
        // projection instead.  Everything else slang read itself, and is left
        // as text for the memo to hash at most once.
        if (const auto it = seeded.find(full_path.string()); it != seeded.end()) {
            state.parsed_digests.emplace(std::move(dependency_uri),
                                         std::make_pair(it->second.lo, it->second.hi));
            continue;
        }
        state.parsed_texts.emplace(std::move(dependency_uri),
                                   state.source_manager->getSourceText(buffer));
    }
}

/// Seed the headers the previous parse of this same buffer `include`d.
///
/// The candidate list is that parse's dependency set rather than everything the
/// cache holds: assignText() copies, so offering every open buffer's headers to
/// every keystroke would trade one read for several copies.  A header that
/// changed on disk was already dropped by the watcher path
/// (refresh_changed_extra_files), so nothing here needs to check the filesystem.
///
/// A header big enough for its bulk to be felt per keystroke is seeded as its
/// directives alone, provided its own shard was built from a parse of it on its
/// own.  See kDirectivesOnlySeedBytes for why size gates this.
static void preload_open_parse_headers(slang::SourceManager& sm, OpenParseHeaderCache& cache,
                                       const std::vector<std::string>& previous_dependencies,
                                       const std::unordered_set<std::string_view>& excluded,
                                       const std::unordered_set<std::string>& standalone_uris) {
    for (const auto& dependency_uri : previous_dependencies) {
        const auto path = normalize_filesystem_path(path_from_file_uri(dependency_uri)).string();
        if (path.empty() || excluded.contains(path))
            continue;
        auto text = cache.get(path);
        if (!text)
            continue;
        if (text->size() >= kDirectivesOnlySeedBytes && standalone_uris.contains(dependency_uri)) {
            if (auto directives = cache.get_directives(path, header_directives_only))
                text = std::move(directives);
        }
        sm.assignText(std::string_view(path), std::string_view(*text));
    }
}

/// Record the headers an open buffer's parse pulled in, so the next keystroke
/// can seed them instead of reading them again.
static void store_open_parse_headers(const slang::SourceManager& sm, const DocumentState& state,
                                     OpenParseHeaderCache& cache,
                                     const std::unordered_set<std::string_view>& excluded) {
    for (const auto buffer : sm.getAllBuffers()) {
        const auto& full_path = sm.getFullPath(buffer);
        if (full_path.empty())
            continue;
        // Normalize before using this as a cache key: preload_open_parse_headers()
        // looks entries up by normalize_filesystem_path(path_from_file_uri(...)),
        // and slang's own resolved path does not go through that normalization.
        // On a filesystem where the project path crosses a symlink or a short
        // (8.3) name the two spellings differ, so storing under the raw path
        // left get() unable to ever find what was just stored.
        const auto path_string = normalize_filesystem_path(full_path).string();
        if (excluded.contains(path_string))
            continue;
        // Same reasoning as store_header_texts(): the dependency set is what
        // separates genuine `include targets from seeded buffers and overlays.
        if (!state.include_dependency_set.contains(uri_from_path(full_path)))
            continue;
        cache.store(path_string, sm.getSourceText(buffer));
    }
}

/// Record the headers this parse pulled in, so sibling files in the same burst
/// can be seeded from memory instead of re-reading them.
static void store_header_texts(const slang::SourceManager& sm, const DocumentState& state,
                               HeaderTextCache& cache, uint64_t generation,
                               const std::unordered_set<std::string_view>& excluded,
                               bool count_as_burst_parse) {
    std::vector<HeaderTextCache::ParsedHeader> headers;
    for (const auto buffer : sm.getAllBuffers()) {
        const auto& full_path = sm.getFullPath(buffer);
        if (full_path.empty())
            continue;
        auto path_string = full_path.string();
        if (excluded.contains(path_string))
            continue;
        // Only genuine `include dependencies of this parse.  Seeded buffers and
        // open-buffer overlays also live in this SourceManager, and the
        // dependency set is what distinguishes them.
        if (!state.include_dependency_set.contains(uri_from_path(full_path)))
            continue;
        headers.emplace_back(std::move(path_string), sm.getSourceText(buffer));
    }
    // Handed over in one call on purpose: this parse and the headers it wanted
    // have to enter the cache together or a concurrent seed_candidates() reads a
    // half-updated popularity ratio.  See HeaderTextCache::record_parse().
    cache.record_parse(generation, headers, count_as_burst_parse);
}

/// Source text of the buffer @p uri was loaded into.
///
/// SyntaxIndex::build() uses the source text for one thing — finding where a
/// module instantiation ends — and it splits that text by line.  Handing a
/// header's build the *including* file's text would resolve those line numbers
/// against the wrong file, so find the header's own buffer.  getAllBuffers() is
/// documented as not thread safe; this is safe only because every background
/// worker owns its SourceManager.
static std::string_view header_source_text(const slang::SourceManager& sm,
                                           const std::string& uri) {
    for (const auto buffer : sm.getAllBuffers()) {
        const auto& full_path = sm.getFullPath(buffer);
        if (full_path.empty())
            continue;
        if (uri_from_path(full_path) == uri)
            return sm.getSourceText(buffer);
    }
    return {};
}

/// The key store_header_texts() files @p uri's text under.
///
/// Taken from the SourceManager rather than derived from the URI so it is the
/// same spelling store_header_texts() used; slang's resolved path does not go
/// through normalize_filesystem_path(), and a key that differs by a symlink or a
/// short name would replace nothing.
static std::string header_cache_key(const slang::SourceManager& sm, const std::string& uri) {
    for (const auto buffer : sm.getAllBuffers()) {
        const auto& full_path = sm.getFullPath(buffer);
        if (!full_path.empty() && uri_from_path(full_path) == uri)
            return full_path.string();
    }
    return {};
}

/// Whether @p line's first thing that is neither whitespace nor a comment is a
/// backtick, updating @p in_block_comment for the line that follows.
///
/// The comment state has to be carried whether or not the answer is already
/// known, or a dropped line that opens a block comment would leave a later
/// `define inside it looking like a directive.
static bool line_starts_directive(std::string_view line, bool& in_block_comment) {
    bool starts = false;
    bool decided = false;
    for (size_t i = 0; i < line.size();) {
        if (in_block_comment) {
            if (line.compare(i, 2, "*/") == 0) {
                in_block_comment = false;
                i += 2;
            } else {
                ++i;
            }
            continue;
        }
        if (line.compare(i, 2, "//") == 0)
            break;
        if (line.compare(i, 2, "/*") == 0) {
            in_block_comment = true;
            i += 2;
            continue;
        }
        if (line[i] == '"') {
            // Skipped rather than scanned: a backtick or a /* inside a string
            // literal is neither a directive nor a comment.
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size())
                    ++i;
                ++i;
            }
            ++i;
            decided = true;
            continue;
        }
        if (!decided && std::isspace(static_cast<unsigned char>(line[i])) == 0) {
            starts = line[i] == '`';
            decided = true;
        }
        ++i;
    }
    return starts;
}

/// @p text with everything but its preprocessor directives blanked out.
///
/// What an includer needs from a header is what the preprocessor does with it.
/// A `define is genuinely per-includer — its body means whatever it expands to
/// where it is used — so every file has to see it again.  A declaration is not:
/// since shard scoping the header's own shard is where its declarations live and
/// what every other file resolves against, so re-reading the header's bulk once
/// per includer buys nothing and costs O(files x header), which on an HPC design
/// is the largest file in the project multiplied by its includer count.
///
/// Dropped lines become empty rather than disappearing, so every directive keeps
/// the line number it is written on: a macro expansion carries the location of
/// its definition, and anything that resolves one back to the header would
/// otherwise report the wrong line.
///
/// A directive inside a block comment stays dropped even though the preprocessor
/// would ignore it either way — keeping the line without its opening /* would
/// leave a stray */ in text that gets `include`d.
static std::string header_directives_only(std::string_view text) {
    std::string projected;
    projected.reserve(text.size() / 8 + 64);

    bool in_block_comment = false;
    bool continuing = false;
    size_t pos = 0;
    while (pos < text.size()) {
        const auto eol = text.find('\n', pos);
        const auto line_end = eol == std::string_view::npos ? text.size() : eol;
        const auto line = text.substr(pos, line_end - pos);

        const bool opened_in_comment = in_block_comment;
        const bool directive = line_starts_directive(line, in_block_comment);
        const bool keep = continuing || (!opened_in_comment && directive);
        if (keep)
            projected.append(line);

        auto body = line;
        while (!body.empty() && (body.back() == '\r' || body.back() == '\n'))
            body.remove_suffix(1);
        continuing = keep && !body.empty() && body.back() == '\\';

        if (eol == std::string_view::npos)
            break;
        projected.push_back('\n');
        pos = eol + 1;
    }
    return projected;
}

struct BuiltHeaderShard {
    std::string uri;
    std::shared_ptr<const SyntaxIndex> index;
    /// Built from a parse of the header by itself, so the shard is the
    /// authoritative record of its declarations — the condition for serving an
    /// includer its directives alone.  See standalone_header_uris_.
    bool stands_alone{false};
};

static std::vector<BuiltHeaderShard>
build_header_shards(const std::vector<std::string>& headers_to_build, const DocumentState& includer,
                    const std::vector<std::string>& defines,
                    const std::vector<std::filesystem::path>& include_dirs,
                    const std::vector<OpenTextOverlay>& open_overlays,
                    HeaderTextCache& header_texts, uint64_t generation,
                    DocumentState* parsed_digest_sink = nullptr);

static std::shared_ptr<DocumentState>
make_file_state_with_options(const std::filesystem::path& path,
                             const std::vector<std::string>& defines,
                             const std::vector<std::filesystem::path>& include_dirs,
                             const std::vector<OpenTextOverlay>& open_overlays = {},
                             bool retain_text = false,
                             HeaderTextCache* header_texts = nullptr,
                             uint64_t generation = 0,
                             bool collect_diagnostics = true,
                             bool restrict_index_to_own_file = false,
                             bool count_as_burst_parse = true) {
    const auto start = Clock::now();
    const auto norm = normalize_filesystem_path(path);
    const std::string norm_string = norm.string();
    const std::string uri = uri_from_path(norm);

    auto sm = make_lsp_source_manager();
    std::unordered_set<std::string_view> header_cache_excluded;
    std::unordered_map<std::string, IndexCache::Digest> seeded_header_paths;
    if (header_texts) {
        header_cache_excluded = header_cache_excluded_paths(open_overlays, norm_string);
        preload_cached_header_texts(*sm, *header_texts, generation, header_cache_excluded,
                                    seeded_header_paths);
    }
    preload_open_text_overlays(*sm, open_overlays, norm_string);
    slang::parsing::PreprocessorOptions ppo;
    ppo.predefines = defines;
    ppo.additionalIncludePaths = include_dirs;
    slang::Bag bag;
    bag.set(ppo);

    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(norm.string(), *sm, bag);
    if (!tree_or_error)
        return nullptr;

    // fromFile already loaded source into SourceManager; read text from there
    // instead of opening the file a second time.  On NFS this avoids an extra
    // open + stat + read per indexed file.
    // The buffer the tree was parsed from, not the one its root node starts in:
    // a file whose first line is an `include has its first token in the header,
    // so the root-node spelling hands back the *header's* text as if it were
    // this file's.  See SyntaxIndex::build(), which scopes on the same identity.
    std::string_view sm_source;
    if (const auto& t = *tree_or_error; t) {
        if (const auto own_buffers = t->getSourceBufferIds(); !own_buffers.empty())
            sm_source = sm->getSourceText(own_buffers.front());
    }

    auto state = std::make_shared<DocumentState>(uri, retain_text ? std::string(sm_source)
                                                                  : std::string{},
                                                 nullptr);
    state->normalized_path = norm.string();
    if (retain_text)
        cache_document_end_position(*state);
    state->source_manager = std::move(sm);
    state->tree = std::move(*tree_or_error);
    state->include_dependencies = collect_include_dependency_uris(*state->source_manager, uri);
    state->include_dependency_set.insert(state->include_dependencies.begin(),
                                         state->include_dependencies.end());
    record_parsed_digests(*state, sm_source, seeded_header_paths);
    if (state->tree)
        state->include_resolutions =
            collect_include_resolutions(*state->tree, *state->source_manager, uri);
    if (header_texts)
        store_header_texts(*state->source_manager, *state, *header_texts, generation,
                           header_cache_excluded, count_as_burst_parse);
    if (state->tree) {
        // A restricted build indexes only this file's own declarations and
        // occurrences; whatever it `include`s becomes one shard per header,
        // built once per indexing burst by background_index_loop().
        state->index = SyntaxIndex::build(
            *state->tree, sm_source, IndexDepth::Declarations,
            restrict_index_to_own_file ? std::string_view(uri) : std::string_view{});
        state->index.include_dependencies = state->include_dependencies;
    }
    // Formatting a diagnostic renders its message and resolves its line, and the
    // background index path throws the result away: only the shard is committed.
    // A project that parses with errors would pay that per file for nothing.
    if (collect_diagnostics)
        collect_parse_diagnostics(*state, uri);
    log_perf("make_file_state_with_options " + uri, start);
    return state;
}

/// Build one shard per claimed header, using @p includer as the parse that
/// pulled them in.  Runs outside map_mutex_: it parses.
///
/// The caller's claim is what keeps two workers off the same header.  @p includer
/// may be an open buffer's snapshot — its text is unsaved, but a header's own
/// shard is built from the header, so an open includer can claim just as well as
/// a closed one.
static std::vector<BuiltHeaderShard>
build_header_shards(const std::vector<std::string>& headers_to_build, const DocumentState& includer,
                    const std::vector<std::string>& defines,
                    const std::vector<std::filesystem::path>& include_dirs,
                    const std::vector<OpenTextOverlay>& open_overlays,
                    HeaderTextCache& header_texts, uint64_t generation,
                    DocumentState* parsed_digest_sink) {
    std::vector<BuiltHeaderShard> built;
    built.reserve(headers_to_build.size());
    // Headers that did not parse standalone, in the order they were claimed.
    // They are sharded together after this loop, from one walk of the
    // includer's tree; see below.
    std::vector<std::string> derived_uris;
    for (const auto& header_uri : headers_to_build) {
        // Parse the header on its own.  Deriving its shard from an includer's
        // tree cannot express "the header's declarations": the restriction
        // meant to do that does not filter, because
        // SourceFileIdResolver::wants_declaration() keeps every declaration
        // when no mentions provider is set.  What landed under the header's
        // URI was therefore a copy of whichever includer claimed it first —
        // its modules and ports, scoped to its module, and none of the
        // header's own declarations when the `include sits at file scope.
        std::shared_ptr<SyntaxIndex> header_index;
        auto header_state = make_file_state_with_options(
            path_from_file_uri(header_uri), defines, include_dirs, open_overlays,
            /*retain_text=*/false, &header_texts, generation,
            /*collect_diagnostics=*/true, /*restrict_index_to_own_file=*/true,
            // Burst bookkeeping, not a file of the project: counting it would
            // make the header look less shared than every file that includes it
            // proves it is.  See HeaderTextCache::record_parse().
            /*count_as_burst_parse=*/false);
        // A header parsed on its own read its own bytes, which is the one
        // reading of it the shard cache can key its shard on: the includer's
        // parse may have been served the burst's directives-only projection
        // instead.  See DocumentState::parsed_digests.
        if (parsed_digest_sink && header_state) {
            for (const auto& [digest_uri, digest] : header_state->parsed_digests)
                parsed_digest_sink->parsed_digests.try_emplace(digest_uri, digest);
            // The text points into header_state's SourceManager, which this
            // shard's DocumentState does not own -- so it is hashed now, while
            // that manager is still alive, rather than carried as a view.
            for (const auto& [text_uri, text] : header_state->parsed_texts) {
                if (parsed_digest_sink->parsed_digests.contains(text_uri))
                    continue;
                const auto digest = IndexCache::digest_source_buffer(text);
                parsed_digest_sink->parsed_digests.try_emplace(text_uri,
                                                               std::make_pair(digest.lo,
                                                                              digest.hi));
            }
        }
        const bool stands_alone =
            header_state && header_state->tree &&
            std::none_of(header_state->parse_diagnostics.begin(),
                         header_state->parse_diagnostics.end(),
                         [](const ParseDiagInfo& diag) { return diag.severity == 1; });
        if (stands_alone) {
            header_index = std::make_shared<SyntaxIndex>(std::move(header_state->index));

            // Its declarations are now recorded, so the rest of the burst
            // only needs its directives.  Serving the projection through the
            // same cache the full text came from is what makes the saving
            // reach the files still queued: at most one file per worker can
            // already be reading the header when this runs, and every file
            // after them is seeded from here.  A header open in the editor
            // was never stored, so it is never projected.
            if (const auto key = header_cache_key(*includer.source_manager, header_uri);
                !key.empty()) {
                header_texts.project(
                    generation, key,
                    header_directives_only(header_source_text(*includer.source_manager,
                                                              header_uri)));
            }
        } else {
            // A header that is a textual fragment — a port list, a module
            // opened in one file and closed in another, a class body a package
            // `include`s — has no tree of its own to index, so its shard has to
            // come from the includer that pulled it in.  Deferred: one walk of
            // that tree serves all of them (see below).
            derived_uris.push_back(header_uri);
        }
        built.push_back(BuiltHeaderShard{
            .uri = header_uri,
            .index = std::move(header_index),
            .stands_alone = stands_alone,
        });
    }

    if (!derived_uris.empty() && includer.tree) {
        // One walk, then split, rather than one walk per header.  Restricting
        // the build to a single header does not make the walk any cheaper — it
        // still visits every node of the includer's tree and only discards what
        // it finds — so N headers cost N x the includer.  A package that
        // `include`s its whole library is exactly that shape: UVM's uvm_pkg.sv
        // pulls in 116 fragments across 86k lines, and re-walking it once per
        // fragment was 11.3 s of a 12.5 s project index.
        //
        // The walk is unrestricted so every fragment's declarations are found
        // in it, and split_by_source_file() then keeps each entry with the file
        // it came from.  That also fixes what the per-header build could not
        // express: with no mentions provider set, its restriction did not
        // filter declarations at all, so each fragment's shard was a copy of
        // the whole includer's — 116 copies of UVM, which is where the start-up
        // memory went.
        //
        // No source text is passed: it feeds only the ';'-scan fallback for an
        // instantiation's end line, which one text cannot serve for many files,
        // and which syntax_end_line0() uses only when the parsed range has no
        // valid end.
        auto derived = SyntaxIndex::build(*includer.tree, {}, IndexDepth::Declarations)
                           .split_by_source_file(derived_uris);
        // Both lists were appended to in the loop above, so the nth shard that
        // did not stand alone is the nth entry of derived_uris.
        size_t next = 0;
        for (auto& shard : built) {
            if (shard.stands_alone)
                continue;
            auto& index = derived[next++];
            index.include_dependencies =
                collect_include_dependency_uris(*includer.source_manager, shard.uri);
            shard.index = std::make_shared<SyntaxIndex>(std::move(index));
        }
    }
    return built;
}

Analyzer::~Analyzer() {
    // With no writer thread, the queue's only other drain point is the end of a
    // burst.  A process that goes away mid-burst would drop everything queued
    // since the last publish, which costs the next launch a reparse of exactly
    // the files this launch just did.  Write them out instead, before the stop
    // flag closes the queue.
    if (shard_writes_need_inline_drain())
        drain_shard_writes_inline();

    // Stopped first: it holds shared_ptrs into the shards the rest of teardown
    // is about to drop, and it takes map_mutex_ to check its generation.
    {
        std::lock_guard<std::mutex> lock(index_cache_write_mutex_);
        index_cache_writer_stop_ = true;
    }
    index_cache_write_cv_.notify_all();
    if (index_cache_writer_.joinable())
        index_cache_writer_.join();

    if (parse_worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(parse_mutex_);
            parse_stop_.store(true);
        }
        parse_cv_.notify_all();
        parse_worker_.join();
    }
    if (!background_indexers_.empty()) {
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            background_stop_.store(true);
        }
        background_cv_.notify_all();
        for (auto& worker : background_indexers_) {
            if (worker.joinable())
                worker.join();
        }
    }
}

std::shared_ptr<DocumentState> Analyzer::make_state(const std::string& uri,
                                                    const std::string& text) const {
    const auto start = Clock::now();
    // Pass URI as name (display label) and stripped filesystem path as path
    // (used by SourceManager::assignText for include resolution relative to
    // the file's directory, not the server CWD).
    std::string path = path_from_file_uri(uri);
    // Fresh SourceManager per document snapshot: avoids "path already assigned"
    // errors when the same file is re-parsed on didChange, and prevents the
    // static singleton from accumulating stale buffers across edits.
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> include_dirs;
    std::vector<OpenTextOverlay> open_overlays;
    std::vector<std::string> previous_dependencies;
    std::unordered_set<std::string> standalone_headers;
    const auto normalized_current_path = normalize_filesystem_path(path).string();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        defines = defines_;
        include_dirs = include_dir_paths_;
        // What this buffer included last time is the candidate set for header
        // seeding below.  On didOpen there is no previous snapshot, so the first
        // parse reads from disk and every keystroke after it does not.
        if (const auto it = docs_.find(uri); it != docs_.end() && it->second)
            previous_dependencies = it->second->include_dependencies;
        // Only the ones this buffer includes: the set is project-wide, and
        // copying all of it per keystroke would grow with the design.
        for (const auto& dependency_uri : previous_dependencies) {
            if (standalone_header_uris_.contains(dependency_uri))
                standalone_headers.insert(dependency_uri);
        }
        open_overlays.reserve(docs_.size());
        for (const auto& [open_uri, open_state] : docs_) {
            if (!open_state || open_uri == uri)
                continue;
            if (open_state->normalized_path == normalized_current_path)
                continue;
            open_overlays.push_back(OpenTextOverlay{
                .uri = open_uri,
                .path = open_state->normalized_path,
                .state = open_state,
            });
        }
    }

    auto sm = make_lsp_source_manager();
    const auto header_cache_excluded =
        header_cache_excluded_paths(open_overlays, normalized_current_path);
    preload_open_parse_headers(*sm, open_parse_header_texts_, previous_dependencies,
                               header_cache_excluded, standalone_headers);
    preload_open_text_overlays(*sm, open_overlays, normalized_current_path);
    slang::parsing::PreprocessorOptions ppo;
    ppo.predefines = defines;
    ppo.additionalIncludePaths = std::move(include_dirs);
    slang::Bag bag;
    bag.set(ppo);
    // Pass the normalized path, not the raw one path_from_file_uri() produced:
    // this SourceManager has disableProximatePaths set, so it resolves a
    // relative `include against this path verbatim, with no canonicalization
    // of its own.  If this were the raw path, an include's resolved full path
    // would carry whatever symlink or short (8.3) name the client's spelling
    // had, which does not match the normalized path preload/store_open_parse_headers()
    // key their cache with — a permanent cache miss on any filesystem where
    // that spelling differs (macOS /tmp, Windows short names).
    auto tree = slang::syntax::SyntaxTree::fromText(
        std::string_view(text), *sm, std::string_view(uri), std::string_view(normalized_current_path),
        bag);
    auto state = std::make_shared<DocumentState>(uri, text, nullptr);
    state->normalized_path = normalized_current_path;
    cache_document_end_position(*state);
    state->source_manager = std::move(sm);
    state->tree = std::move(tree);
    state->include_dependencies = collect_include_dependency_uris(*state->source_manager, uri);
    state->include_dependency_set.insert(state->include_dependencies.begin(),
                                         state->include_dependencies.end());
    store_open_parse_headers(*state->source_manager, *state, open_parse_header_texts_,
                             header_cache_excluded);
    // clangd-style current-file layer:
    //
    // Do not materialize any current-file SyntaxIndex on didOpen/didChange.
    // The live SyntaxTree is the current-file representation.  Features derive
    // narrow AST facts on demand, and project/background files keep the indexed
    // shard representation.
    collect_parse_diagnostics(*state, uri);
    log_perf("make_state " + uri, start);
    return state;
}

void Analyzer::open(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);

    const auto path_string = state->normalized_path;

    bool listed_extra_file = false;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        docs_[uri] = state;
        invalidate_extra_snapshots_locked();
        listed_extra_file = extra_file_set_.contains(path_string);
        parse_committed_cv_.notify_all();
    }

    // Building a dynamic/open-buffer SyntaxIndex may walk the full AST. Do that
    // outside map_mutex_ so a large listed file opened in the editor does not
    // block unrelated request handlers behind global analyzer state.
    if (listed_extra_file) {
        auto index = get_dynamic_index(*state);
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (const auto it = docs_.find(uri); it != docs_.end() && it->second == state)
            update_extra_cache_for_live_state_locked(state, std::move(index));
    }
}

void Analyzer::change(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);

    const auto path_string = state->normalized_path;

    bool listed_extra_file = false;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        docs_[uri] = state;
        invalidate_extra_snapshots_locked();
        listed_extra_file = extra_file_set_.contains(path_string);
        parse_committed_cv_.notify_all();

        if (queue_include_dependents_locked(uri)) {
            ++background_generation_;
            start_background_indexer_locked();
            background_cv_.notify_all();
        }
    }

    // See Analyzer::open(): current-buffer shard building is intentionally
    // outside map_mutex_ to keep the edit path responsive on large RTL files.
    if (listed_extra_file) {
        auto index = get_dynamic_index(*state);
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (const auto it = docs_.find(uri); it != docs_.end() && it->second == state)
            update_extra_cache_for_live_state_locked(state, std::move(index));
    }

}

uint64_t Analyzer::enqueue_parse(const std::string& uri, std::string text) {
    uint64_t version = ++version_counter_;

    std::string path = path_from_file_uri(uri);
    auto state = std::make_shared<DocumentState>(uri, std::move(text), nullptr);
    state->normalized_path = normalize_filesystem_path(path).string();
    cache_document_end_position(*state);
    state->doc_version = version;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        // This placeholder replaces the parsed snapshot before the worker has
        // produced a new one, so carry its `include list forward.  What a file
        // included one keystroke ago is the best available answer until the
        // reparse lands, and leaving it empty would tell make_state() that this
        // buffer includes nothing — which is exactly when its headers should be
        // seeded from cache instead of read again.
        if (const auto it = docs_.find(uri); it != docs_.end() && it->second) {
            state->include_dependencies = it->second->include_dependencies;
            state->include_dependency_set = it->second->include_dependency_set;
            // Keep the last snapshot that actually parsed, so an AST-shaped
            // request arriving in this window has something true to answer
            // from.  Taking the predecessor's own predecessor when it is
            // itself a placeholder keeps the chain exactly one deep, however
            // many keystrokes arrive before a parse commits.
            state->previous_parsed =
                it->second->tree ? it->second : it->second->previous_parsed;
        }
        docs_[uri] = state;
        latest_version_[uri] = version;
        semantic_diagnostics_.erase(uri);
        invalidate_extra_snapshots_locked();
    }
    {
        std::lock_guard<std::mutex> lock(parse_mutex_);
        parse_pending_[uri] = ParseJob{uri, std::move(state), version};
        if (!parse_worker_.joinable())
            parse_worker_ = std::thread([this] { parse_worker_loop(); });
    }
    parse_cv_.notify_one();
    return version;
}

void Analyzer::set_parse_complete_callback(
    std::function<void(const std::string& uri)> cb) {
    parse_complete_cb_ = std::move(cb);
}

void Analyzer::set_parse_paused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(parse_mutex_);
        parse_paused_.store(paused);
    }
    // Taken under parse_mutex_ above so the worker cannot miss the change
    // between testing its predicate and going back to sleep.
    parse_cv_.notify_all();
}

void Analyzer::parse_worker_loop() {
    // Live shards whose owning buffer has been reparsed but whose rebuild is
    // waiting for the typing to stop.  See the deferral note below.
    std::unordered_map<std::string, std::shared_ptr<const DocumentState>> deferred_shards;
    // Reparsed buffers whose includers still have to be told, waiting for the
    // same moment.  See the fanout note below.
    std::unordered_set<std::string> deferred_dependents;

    while (true) {
        ParseJob job;
        bool have_job = false;
        {
            std::unique_lock<std::mutex> lock(parse_mutex_);
            const auto takeable = [&] {
                return parse_stop_.load() ||
                       (!parse_paused_.load() && !parse_pending_.empty());
            };
            if (deferred_shards.empty() && deferred_dependents.empty()) {
                parse_cv_.wait(lock, takeable);
            } else {
                parse_cv_.wait_for(lock, kLiveShardIdleDelay, takeable);
            }
            if (parse_stop_.load())
                break;
            if (!parse_paused_.load() && !parse_pending_.empty()) {
                auto it = parse_pending_.begin();
                job = std::move(it->second);
                parse_pending_.erase(it);
                have_job = true;
            }
        }

        if (!have_job) {
            // The queue stayed empty for kLiveShardIdleDelay, so the user has
            // stopped typing and the deferred shards are worth building.
            for (auto& [shard_uri, shard_state] : deferred_shards) {
                auto index = get_dynamic_index(*shard_state);
                std::lock_guard<std::mutex> lock(map_mutex_);
                if (const auto it = docs_.find(shard_uri);
                    it != docs_.end() && it->second == shard_state)
                    update_extra_cache_for_live_state_locked(shard_state, std::move(index));
            }
            deferred_shards.clear();

            // Tell the includers, once per burst.
            //
            // Editing a header included by hundreds of files used to queue all
            // of them on every keystroke, and each fanout bumped the background
            // generation.  That generation is what scopes HeaderTextCache and
            // the directives-only projection built from it, so every character
            // typed threw both away and made the resulting storm re-read and
            // re-preprocess the whole header once per includer — the
            // O(files x header) blowup the projection exists to prevent, paid
            // per keystroke and on the same CPU the buffer's own parse needs.
            //
            // The scan is not cheap either: it walks every project shard's
            // dependency list under map_mutex_, which every request handler
            // also needs.
            //
            // Deferring costs the includers a staleness window of one idle
            // delay after the last keystroke, the same trade already accepted
            // for the live shard above.
            if (!deferred_dependents.empty()) {
                std::lock_guard<std::mutex> lock(map_mutex_);
                bool queued = false;
                for (const auto& dependent_uri : deferred_dependents) {
                    if (queue_include_dependents_locked(dependent_uri))
                        queued = true;
                }
                if (queued) {
                    ++background_generation_;
                    start_background_indexer_locked();
                    background_cv_.notify_all();
                }
                deferred_dependents.clear();
            }
            continue;
        }

        auto state = make_state(job.uri, job.pending->text); // outside all locks
        state->doc_version = job.version;

        // One keystroke produces two snapshots of the same text: the text-only
        // placeholder enqueue_parse() installed, and this one.  Folds are
        // derived from the text and from nothing else, so the answer cannot
        // differ between them -- but the slot that holds it lives on the
        // snapshot, so without this the identical whole-file computation is
        // paid twice per edit.  Measured on a 57 890-line buffer before the
        // fold passes were made linear: 254 ms in the reparse window and 248 ms
        // again once the parse landed, against 5 ms for a warm slot.
        //
        // Share the slot rather than copy the result out of it.  The editor
        // asks from the notification itself, so the request thread is usually
        // still computing on the placeholder when this runs -- a copy taken
        // here finds it empty and both computations happen anyway.  Sharing
        // lets whichever finishes first answer for both.
        //
        // The text check is not defensive about `make_state()`, which is handed
        // exactly `job.pending->text`; it states the invariant the share relies
        // on.  Nothing has published this state yet, so the assignment needs no
        // synchronization of its own.
        if (state->text == job.pending->text)
            state->share_folding_cache_with(*job.pending);

        bool committed = false;
        bool listed_extra = false;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            auto it = latest_version_.find(job.uri);
            if (it != latest_version_.end() && it->second == job.version) {
                docs_[job.uri] = state;
                invalidate_extra_snapshots_locked();
                listed_extra = extra_file_set_.contains(state->normalized_path);

                committed = true;
                // Wake any handler parked in get_parsed_state() for this file.
                // Broadcast: several requests can be waiting on the same
                // notification's reparse.
                parse_committed_cv_.notify_all();
            }
            // else: stale, discard
        }

        if (committed) {
            // Defer the shard rebuild until the typing stops.
            //
            // Only project-wide features read this shard, so it is allowed to
            // trail the buffer by a moment.  Building it walks everything the
            // file `include`s, which with one large shared header is the most
            // expensive thing on the edit path — and every keystroke throws the
            // previous result away.  Holding it until the parse queue has been
            // empty for kLiveShardIdleDelay makes a burst cost one rebuild
            // instead of one per character.  The cost of that is a shard which
            // trails the buffer by up to one delay after the last keystroke;
            // nothing that answers for the current file reads it.
            if (listed_extra)
                deferred_shards[job.uri] = state;
            deferred_dependents.insert(job.uri);
            if (parse_complete_cb_)
                parse_complete_cb_(job.uri);
        }
    }
}

void Analyzer::close(const std::string& uri) {
    // A file open as a buffer is excluded from the open-parse header cache, so
    // whatever was cached for it predates the editing session.  Closing it makes
    // it cache-eligible again; drop the pre-session text rather than serve it.
    open_parse_header_texts_.invalidate(
        normalize_filesystem_path(path_from_file_uri(uri)).string());
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        docs_.erase(uri);
        latest_version_.erase(uri);
        semantic_diagnostics_.erase(uri);
        invalidate_extra_snapshots_locked();
        // A closed document never gets its parse, so release anyone waiting on
        // one instead of making them sit out the timeout.
        parse_committed_cv_.notify_all();
        // If the closed file is also in the filelist cache, replace its live shard
        // with a disk-backed parse in the background.  Keeping this asynchronous is
        // important for large buffers: closing a split should not synchronously
        // parse an include-heavy RTL file on the UI path.
        if (const auto it = extra_cache_.find(uri); it != extra_cache_.end()) {
            queue_background_file_locked(it->second.path, /*front=*/false);
            start_background_indexer_locked();
            background_cv_.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lock(parse_mutex_);
        parse_pending_.erase(uri);
    }
}

std::vector<std::string> Analyzer::extra_files() const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    return extra_files_;
}

std::vector<std::shared_ptr<const DocumentState>> Analyzer::project_file_states_sync() const {
    const auto start = Clock::now();

    std::vector<std::string> paths;
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> include_dirs;
    std::vector<OpenTextOverlay> open_overlays;
    std::unordered_map<std::string, std::shared_ptr<const DocumentState>> live_by_path;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        paths = extra_files_;
        defines = defines_;
        include_dirs = include_dir_paths_;

        open_overlays.reserve(docs_.size());
        live_by_path.reserve(docs_.size());
        for (const auto& [open_uri, open_state] : docs_) {
            if (!open_state)
                continue;
            open_overlays.push_back(OpenTextOverlay{
                .uri = open_uri,
                .path = open_state->normalized_path,
                .state = open_state,
            });
            live_by_path.emplace(open_state->normalized_path, open_state);
        }
    }

    std::vector<std::shared_ptr<const DocumentState>> states;
    states.reserve(paths.size());
    std::unordered_set<std::string> seen_paths;
    seen_paths.reserve(paths.size());

    for (const auto& raw_path : paths) {
        const auto normalized_path = normalize_filesystem_path(raw_path).string();
        if (!seen_paths.insert(normalized_path).second)
            continue;

        if (const auto live = live_by_path.find(normalized_path); live != live_by_path.end()) {
            states.push_back(live->second);
            continue;
        }

        // Parse into a short-lived DocumentState so :LintAll can run the same
        // AST-based lint rules as an open buffer, then discard the AST after the
        // executeCommand response is built.  This deliberately does not update
        // extra_cache_ or publish a ProjectIndexSnapshot; :LintAll is a manual
        // diagnostics command, not a hidden reindex operation.
        auto state = make_file_state_with_options(normalized_path, defines, include_dirs,
                                                  open_overlays, true);
        if (state)
            states.push_back(std::move(state));
    }

    log_perf("project_file_states_sync files=" + std::to_string(states.size()), start);
    return states;
}

std::shared_ptr<const DocumentState> Analyzer::get_state(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return nullptr;
    return it->second;
}

std::shared_ptr<const DocumentState>
Analyzer::get_parsed_state(const std::string& uri, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(map_mutex_);
    const auto current = [&]() -> std::shared_ptr<const DocumentState> {
        const auto it = docs_.find(uri);
        return it == docs_.end() ? nullptr : it->second;
    };

    auto state = current();
    if (!state || state->tree)
        return state;

    // A reparse is in flight.  Wait for it rather than answer from a snapshot
    // the user has already typed past -- but only for as long as the request
    // thread can afford, since it answers one request at a time.
    parse_committed_cv_.wait_for(lock, timeout, [&] {
        const auto latest = current();
        return !latest || latest->tree;
    });

    state = current();
    if (!state || state->tree)
        return state;

    // Still no tree: the user is typing faster than this file parses, or the
    // parse failed.  One keystroke stale beats nothing at all.
    return state->previous_parsed ? state->previous_parsed : state;
}

struct IdentifierSpan {
    std::string text;
    int start_col{0};
    int end_col{0};
};

// Extract identifier at (0-based line, 0-based col) from source text.
static std::optional<IdentifierSpan> extract_ident_span(std::string_view src, int line, int col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < src.size() && cur < line) {
        if (src[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur < line)
        return std::nullopt;

    size_t ls = pos;
    size_t le = src.find('\n', pos);
    if (le == std::string_view::npos)
        le = src.size();

    if (col < 0 || (size_t)col >= le - ls)
        return std::nullopt;
    size_t ip = ls + col;

    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    if (!is_id(src[ip]))
        return std::nullopt;

    size_t start = ip;
    while (start > ls && is_id(src[start - 1]))
        --start;
    size_t end = ip;
    while (end < le && is_id(src[end]))
        ++end;

    return IdentifierSpan{std::string(src.substr(start, end - start)), (int)(start - ls),
                          (int)(end - ls)};
}

static bool same_location(const Location& lhs, const Location& rhs) {
    return lhs.uri == rhs.uri && lhs.line == rhs.line && lhs.col == rhs.col;
}

static std::string extract_ident(std::string_view src, int line, int col) {
    auto span = extract_ident_span(src, line, col);
    return span ? span->text : std::string{};
}

static bool is_backtick_identifier(std::string_view src, int line, int ident_start_col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < src.size() && cur < line) {
        if (src[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur < line || ident_start_col <= 0)
        return false;

    size_t line_start = pos;
    size_t line_end = src.find('\n', pos);
    if (line_end == std::string_view::npos)
        line_end = src.size();
    const size_t backtick = line_start + (size_t)ident_start_col - 1;
    return backtick < line_end && src[backtick] == '`';
}

static bool is_define_identifier(std::string_view src, int line, int ident_start_col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < src.size() && cur < line) {
        if (src[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur < line || ident_start_col <= 0)
        return false;

    const size_t line_start = pos;
    const size_t ident_start = line_start + (size_t)ident_start_col;
    if (ident_start > src.size())
        return false;

    std::string_view prefix = src.substr(line_start, ident_start - line_start);
    auto first = prefix.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return false;
    prefix.remove_prefix(first);
    return prefix.starts_with("`define") &&
           (prefix.size() == 7 || std::isspace((unsigned char)prefix[7]));
}

static int to_lsp_line(int one_based_line) { return one_based_line > 0 ? one_based_line - 1 : 0; }

static std::optional<Location>
find_module_definition(const SyntaxIndex& index, const std::string& uri, const std::string& name) {
    auto it = index.module_by_name.find(name);
    if (it == index.module_by_name.end() || it->second >= index.modules.size())
        return std::nullopt;

    const auto& module = index.modules[it->second];
    const auto actual_uri = index.source_uri(module.file_id);
    const int line = to_lsp_line(module.line);
    return Location{actual_uri.empty() ? uri : actual_uri, line, module.col, line,
                    module.col + (int)utf16_length(module.name)};
}

static const ModuleEntry* find_module_entry(const SyntaxIndex& index, const std::string& name) {
    auto it = index.module_by_name.find(name);
    if (it == index.module_by_name.end() || it->second >= index.modules.size())
        return nullptr;
    return &index.modules[it->second];
}

static const PortEntry* find_port_entry(const ModuleEntry& module, const std::string& name) {
    auto it = module.port_by_name.find(name);
    if (it == module.port_by_name.end() || it->second >= module.ports.size())
        return nullptr;
    return &module.ports[it->second];
}

static Location location_from_token_actual_uri(const slang::SourceManager& sm,
                                               const std::string& fallback_uri,
                                               const slang::parsing::Token& token);

static std::optional<Location> find_port_definition(const SyntaxIndex& index,
                                                    const std::string& uri,
                                                    const std::string& module_name,
                                                    const std::string& port_name) {
    const auto* module = find_module_entry(index, module_name);
    if (!module)
        return std::nullopt;
    const auto* port = find_port_entry(*module, port_name);
    if (!port)
        return std::nullopt;

    const auto actual_uri = index.source_uri(port->file_id);
    const int line = to_lsp_line(port->line);
    return Location{actual_uri.empty() ? uri : actual_uri, line, port->col, line,
                    port->col + (int)utf16_length(port->name)};
}

// Class name that `owner::alias` names, for a `typedef` declared inside a class.
//
// Keyed by owner rather than by bare alias name: `typedef_by_name` holds one
// entry per name, and in a UVM project every class declares its own `type_id`,
// so a bare lookup answers with whichever class was indexed first.
static std::optional<std::string> scoped_typedef_base_type(const SyntaxIndex& index,
                                                           const std::string& owner,
                                                           const std::string& alias) {
    if (owner.empty() || alias.empty())
        return std::nullopt;
    const auto it = index.package_type_by_scoped_name.find(package_scoped_key(owner, alias));
    if (it == index.package_type_by_scoped_name.end() || it->second >= index.typedefs.size())
        return std::nullopt;
    auto name = canonical_type_name_from_text(index.typedefs[it->second].resolved);
    if (name.empty())
        return std::nullopt;
    return name;
}

// Resolve `package_name::member` through the package-scoped lookup maps.
//
// Three O(1) map probes, no linear scan: this runs on the request path for every
// qualified identifier, and CLAUDE.md forbids whole-project walks there.
static std::optional<Location> find_package_member(const SyntaxIndex& index,
                                                   const std::string& uri,
                                                   const std::string& package_name,
                                                   const std::string& member_name) {
    if (package_name.empty() || member_name.empty())
        return std::nullopt;
    if (!index.package_names.count(package_name))
        return std::nullopt;

    const auto key = package_scoped_key(package_name, member_name);
    auto make_loc = [&](SourceFileID file_id, int line, int col) {
        const auto actual_uri = index.source_uri(file_id);
        const int lsp_line = to_lsp_line(line);
        return Location{actual_uri.empty() ? uri : actual_uri, lsp_line, col, lsp_line,
                        col + (int)utf16_length(member_name)};
    };

    if (auto it = index.package_value_by_scoped_name.find(key);
        it != index.package_value_by_scoped_name.end() && it->second < index.values.size()) {
        const auto& value = index.values[it->second];
        return make_loc(value.file_id, value.line, value.col);
    }
    if (auto it = index.package_type_by_scoped_name.find(key);
        it != index.package_type_by_scoped_name.end() && it->second < index.typedefs.size()) {
        const auto& td = index.typedefs[it->second];
        return make_loc(td.file_id, td.line, td.col);
    }
    if (auto it = index.package_class_by_scoped_name.find(key);
        it != index.package_class_by_scoped_name.end() && it->second < index.classes.size()) {
        const auto& cls = index.classes[it->second];
        return make_loc(cls.file_id, cls.line, cls.col);
    }

    // Enum members live under their typedef entry rather than in the value
    // table, so `pkg::IDLE` needs this extra step.
    for (const auto& td : index.typedefs) {
        if (td.parent_scope != package_name)
            continue;
        for (const auto& member : td.enum_members) {
            if (member.name == member_name)
                return make_loc(member.file_id, member.line, member.col);
        }
    }

    return std::nullopt;
}

static std::optional<std::string> symbol_id_for_index_location(const SyntaxIndex& index,
                                                               const Location& loc,
                                                               bool allow_name_fallback = false) {
    auto acceptable = [&](const ReferenceEntry& ref) {
        return ref.symbol_id && (allow_name_fallback || !ref.symbol_debug.starts_with("name:"));
    };

    // Keep SyntaxIndex compact for initial project warm-up: the index owns only
    // the raw reference vector, not auxiliary location / SymbolID maps.  This
    // lookup is used only to recover the clicked definition's semantic-ish
    // SymbolID, so a linear scan is acceptable and restores the older memory /
    // startup-time tradeoff: startup is faster, references can spend more time.
    const int reference_line = loc.line + 1;
    const auto file_it = index.source_file_ids.find(loc.uri);
    const SourceFileID requested_file_id =
        file_it == index.source_file_ids.end() ? kInvalidSourceFileID : file_it->second;

    size_t best_index = index.references.size();
    for (size_t i = 0; i < index.references.size(); ++i) {
        const auto& ref = index.references[i];
        if (!acceptable(ref) || ref.line != reference_line || ref.col != loc.col)
            continue;

        // Invalid FileID entries historically meant "owning shard URI" and
        // matched by line/column only.  Preserve that behavior while preferring
        // exact file-id matches when a shard contains included headers.
        if (ref.file_id != kInvalidSourceFileID && ref.file_id != requested_file_id)
            continue;

        best_index = i;
        break;
    }

    if (best_index < index.references.size())
        return index.references[best_index].symbol_debug;
    return std::nullopt;
}

static std::optional<Location>
find_module_definition_in_tree(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                               const std::string& name) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& name;
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, const std::string& name)
            : sm(sm), uri(uri), name(name) {}

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            if (result)
                return;
            if (node.header->name.valueText() == name)
                result = location_from_token_actual_uri(sm, uri, node.header->name);
            if (!result)
                visitDefault(node);
        }
    };

    Visitor visitor(tree.sourceManager(), uri, name);
    tree.root().visit(visitor);
    return visitor.result;
}

static std::optional<Location>
find_port_definition_in_tree(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                             const std::string& module_name, const std::string& port_name) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& module_name;
        const std::string& port_name;
        bool in_target_module{false};
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri,
                const std::string& module_name, const std::string& port_name)
            : sm(sm), uri(uri), module_name(module_name), port_name(port_name) {}

        void maybe_set(const slang::parsing::Token& token) {
            if (!result && token && token.valueText() == port_name)
                result = location_from_token_actual_uri(sm, uri, token);
        }

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            if (result)
                return;
            const bool was = in_target_module;
            in_target_module = node.header->name.valueText() == module_name;
            if (in_target_module)
                visitDefault(node);
            in_target_module = was;
        }

        void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
            if (in_target_module && node.declarator)
                maybe_set(node.declarator->name);
            if (!result)
                visitDefault(node);
        }

        void handle(const slang::syntax::ExplicitAnsiPortSyntax& node) {
            if (in_target_module)
                maybe_set(node.name);
            if (!result)
                visitDefault(node);
        }

        void handle(const slang::syntax::PortDeclarationSyntax& node) {
            if (!in_target_module)
                return;
            for (const auto* declarator : node.declarators) {
                if (declarator)
                    maybe_set(declarator->name);
                if (result)
                    return;
            }
            visitDefault(node);
        }

        // `.WIDTH(16)` in an instantiation names a parameter of the child's
        // `#(...)` list.  DefinitionTargetKind::NamedParameter resolves through
        // this same walker, so without a handler here it only ever matched
        // ports and a same-file parameter resolved to nothing at all.
        //
        // Deliberately scoped to ParameterPortListSyntax rather than to
        // ParameterDeclarationSyntax at large: a body `localparam` is a
        // ParameterDeclarationStatementSyntax and never reaches here, so it
        // cannot be claimed by a `#(...)` override that merely shares its name.
        void handle(const slang::syntax::ParameterPortListSyntax& node) {
            if (!in_target_module)
                return;
            for (const auto* declaration : node.declarations) {
                if (!declaration)
                    continue;
                if (const auto* value =
                        declaration->as_if<slang::syntax::ParameterDeclarationSyntax>()) {
                    for (const auto* declarator : value->declarators) {
                        if (declarator)
                            maybe_set(declarator->name);
                        if (result)
                            return;
                    }
                } else if (const auto* type_param =
                               declaration
                                   ->as_if<slang::syntax::TypeParameterDeclarationSyntax>()) {
                    // `parameter type T = logic [7:0]` is overridden by name the
                    // same way a value parameter is.
                    for (const auto* declarator : type_param->declarators) {
                        if (declarator)
                            maybe_set(declarator->name);
                        if (result)
                            return;
                    }
                }
            }
        }
    };

    Visitor visitor(tree.sourceManager(), uri, module_name, port_name);
    tree.root().visit(visitor);
    return visitor.result;
}

static Location location_from_token(const slang::SourceManager& sm, const std::string& uri,
                                    const slang::parsing::Token& token) {
    // A macro argument -- or a literal written inside the macro body itself,
    // e.g. `type_id` in `uvm_object_utils_begin` -- has its own location point
    // into the expansion buffer, which has no lines for getColumnNumber() to
    // walk back through -- it answers 0, and the column comes out negative.
    // Report where the user actually typed it.
    const auto location = sm.isMacroLoc(token.location())
                              ? sm.getFullyOriginalLoc(token.location())
                              : token.location();
    const int line = to_lsp_line((int)sm.getLineNumber(location));
    const int col = utf16_column(sm, location);
    return Location{uri, line, col, line,
                    col + (int)utf16_length(token.valueText())};
}

static Location location_from_token_actual_uri(const slang::SourceManager& sm,
                                               const std::string& fallback_uri,
                                               const slang::parsing::Token& token) {
    // The URI must come from the same resolved location that
    // location_from_token() uses for line/col below -- deriving it from the
    // raw (unresolved) token location instead mixes getFullyExpandedLoc()'s
    // buffer with getFullyOriginalLoc()'s line/col, which point at different
    // files for a token written inside a macro body (e.g. `type_id` in
    // `uvm_object_utils_begin`).
    const auto location = sm.isMacroLoc(token.location())
                              ? sm.getFullyOriginalLoc(token.location())
                              : token.location();
    auto loc = location_from_token(sm, location_to_uri(sm, location, fallback_uri), token);
    return loc;
}

static bool macro_has_user_source_location(const slang::SourceManager& sm,
                                           const slang::parsing::Token& name) {
    // slang built-in macros are represented with SourceLocation::NoLocation.
    // Returning an LSP definition for those tokens forces us to invent a file
    // and line, which is why `SV_COV_ERROR previously jumped to line 1 of the
    // current buffer.  Treat no-location macros as invisible to user-facing
    // LSP features.
    return name && name.location().valid() && sm.isFileLoc(name.location());
}

static std::string render_hover_dimensions(
    const slang::SourceManager& sm,
    const slang::syntax::SyntaxList<slang::syntax::VariableDimensionSyntax>& dimensions) {
    std::string text;
    for (const auto* dimension : dimensions) {
        if (!dimension)
            continue;

        const auto rendered = render_declaration_type_text(sm, *dimension);
        if (rendered.empty())
            continue;

        if (syntax_needs_space_between_fragments(text, rendered))
            text += ' ';
        text += rendered;
    }
    return text;
}

static std::string append_hover_suffix(std::string base, const std::string& suffix) {
    if (suffix.empty())
        return base;
    base += (base.empty() ? "" : " ") + suffix;
    return base;
}

static std::string format_ports_doc(const std::string& ports_text) {
    auto text = trim_copy(ports_text);
    if (text.size() < 2 || text.front() != '(' || text.back() != ')')
        return text;

    auto inner = trim_copy(text.substr(1, text.size() - 2));
    if (inner.empty())
        return "()";

    std::vector<std::string> ports;
    size_t start = 0;
    while (start <= inner.size()) {
        const size_t comma = inner.find(',', start);
        if (comma == std::string::npos) {
            ports.push_back(trim_copy(inner.substr(start)));
            break;
        }
        ports.push_back(trim_copy(inner.substr(start, comma - start)));
        start = comma + 1;
    }
    if (ports.size() <= 1)
        return "(" + inner + ")";

    std::string out = "(\n";
    for (size_t i = 0; i < ports.size(); ++i) {
        out += "    " + ports[i];
        if (i + 1 != ports.size())
            out += ",\n";
    }
    out += "\n)";
    return out;
}

static std::string module_doc_from_entry(const ModuleEntry& module) {
    if (module.ports.empty())
        return {};

    size_t max_dir = 0;
    size_t max_type = 0;
    for (const auto& port : module.ports) {
        max_dir = std::max(max_dir, port.direction.size());
        max_type = std::max(max_type, port.type.size());
    }

    std::string doc = "```\nmodule " + module.name;
    for (const auto& port : module.ports) {
        doc += "\n  " + port.direction;
        doc += std::string(max_dir - port.direction.size(), ' ');
        doc += "  " + port.type;
        doc += std::string(max_type - port.type.size(), ' ');
        doc += "  " + port.name;
    }
    doc += "\n```";
    return doc;
}

static std::optional<Location> find_macro_definition(const slang::syntax::SyntaxTree& tree,
                                                     const std::string& uri,
                                                     const std::string& name) {
    const auto& sm = tree.sourceManager();

    for (const auto* macro : tree.getDefinedMacros()) {
        if (!macro || macro->name.valueText() != name)
            continue;
        if (!macro_has_user_source_location(sm, macro->name))
            continue;
        return location_from_token_actual_uri(sm, uri, macro->name);
    }
    return std::nullopt;
}

static std::optional<SymbolInfo> find_macro_info(const slang::syntax::SyntaxTree& tree,
                                                 const std::string& uri, const std::string& name) {
    const auto& sm = tree.sourceManager();

    for (const auto* macro : tree.getDefinedMacros()) {
        if (!macro || macro->name.valueText() != name)
            continue;
        if (!macro_has_user_source_location(sm, macro->name))
            continue;

        std::string body;
        for (const auto& token : macro->body)
            body += token.toString();
        body = trim_copy(body);

        auto loc = location_from_token_actual_uri(sm, uri, macro->name);
        return SymbolInfo{.name = name,
                          .kind = "macro",
                          .detail = body.empty() ? "(empty)" : body,
                          .line = loc.line,
                          .col = loc.col};
    }
    return std::nullopt;
}

static std::optional<Location>
find_subroutine_argument_definition(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                                    const std::string& subroutine_name,
                                    const std::string& argument_name) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& subroutine_name;
        const std::string& argument_name;
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri,
                const std::string& subroutine_name, const std::string& argument_name)
            : sm(sm), uri(uri), subroutine_name(subroutine_name), argument_name(argument_name) {}

        void handle(const slang::syntax::FunctionDeclarationSyntax& node) {
            const auto* identifier =
                node.prototype->name->as_if<slang::syntax::IdentifierNameSyntax>();
            if (!identifier || identifier->identifier.valueText() != subroutine_name)
                return;

            if (!node.prototype->portList)
                return;

            for (const auto* port_base : node.prototype->portList->ports) {
                const auto* port =
                    port_base ? port_base->as_if<slang::syntax::FunctionPortSyntax>() : nullptr;
                if (!port)
                    continue;
                const auto& token = port->declarator->name;
                if (token.valueText() == argument_name) {
                    result = location_from_token_actual_uri(sm, uri, token);
                    return;
                }
            }
        }
    };

    Visitor visitor(tree.sourceManager(), uri, subroutine_name, argument_name);
    tree.root().visit(visitor);
    return visitor.result;
}

struct GenericDefinitionVisitor : public slang::syntax::SyntaxVisitor<GenericDefinitionVisitor> {
    const slang::SourceManager& sm;
    const std::string& uri;
    const std::string& name;
    const std::string& preferred_module;
    const std::string& preferred_package;
    const std::vector<ImportEntry>& visible_imports;
    int use_line_one_based{0};
    std::string current_module;
    std::string current_package;
    int aggregate_type_depth{0};
    std::optional<Location> first_result;
    std::optional<Location> scoped_result;
    // How many enclosing lexical scopes each recorded candidate sat in.  A
    // declaration inside a generate block shadows a module-level one of the same
    // name, and the module-level declaration is visited first, so "first match
    // wins" would answer with the shadowed signal.
    int first_result_depth{-1};
    int scoped_result_depth{-1};

    GenericDefinitionVisitor(const slang::SourceManager& sm, const std::string& uri,
                             const std::string& name, const std::string& preferred_module,
                             const std::string& preferred_package,
                             const std::vector<ImportEntry>& visible_imports,
                             int use_line_one_based)
        : sm(sm), uri(uri), name(name), preferred_module(preferred_module),
          preferred_package(preferred_package), visible_imports(visible_imports),
          use_line_one_based(use_line_one_based) {}

    std::optional<Location> result() const { return scoped_result ? scoped_result : first_result; }

    bool package_member_visible(std::string_view package_name, std::string_view symbol_name) const {
        if (package_name.empty())
            return true;

        // A symbol declared inside a package is visible without an import only
        // while resolving another identifier from that same package body.  It is
        // not injected into modules that merely `include the package text:
        //
        //   `include "params.svh"       // contains: package cpu_pkg; task add_number; ...
        //   module memory_top;
        //       add_number();           // must not jump to cpu_pkg::add_number
        //   endmodule
        //
        // SystemVerilog package members are reached by qualification
        // (cpu_pkg::add_number) or import (cpu_pkg::* / cpu_pkg::add_number).
        if (preferred_package == package_name)
            return true;

        for (const auto& import : visible_imports) {
            if (import.package_name != package_name)
                continue;
            if (!import.parent_scope.empty() && import.parent_scope != preferred_module)
                continue;
            if (import.start_line > 0 && use_line_one_based < import.start_line)
                continue;
            if (import.end_line > 0 && use_line_one_based > import.end_line)
                continue;
            if (import.wildcard || import.symbol_name == symbol_name)
                return true;
        }
        return false;
    }

    // Lexical declaration scopes (begin/end blocks, loop headers) that enclose
    // the declarations being visited.  A pair of zero lines means "no lexical
    // restriction" — used for scopes that live in another file, where the
    // cursor's line number is not comparable.
    std::vector<std::pair<int, int>> scope_stack;

    void push_scope(slang::SourceRange range) {
        if (!range.start().valid() || !range.end().valid() ||
            uri_from_source_location(sm, range.start()) != uri) {
            scope_stack.emplace_back(0, 0);
            return;
        }
        scope_stack.emplace_back((int)sm.getLineNumber(range.start()),
                                 (int)sm.getLineNumber(range.end()));
    }

    // A declaration inside a block or loop header is only visible to uses
    // lexically inside that same block:
    //
    //     for (int i = 0; i < 10; i++) begin ... end
    //     for (int i = 0; i < 10; i++) begin o_data[i] = 1; end
    //                                               ^ the second `i`, not the first
    /// Enclosing scopes that really restrict visibility, i.e. how deeply
    /// shadowed a declaration found here is.
    int scope_depth() const {
        int depth = 0;
        for (const auto& [start_line, end_line] : scope_stack)
            if (start_line > 0 || end_line > 0)
                ++depth;
        return depth;
    }

    bool cursor_in_innermost_scope() const {
        if (scope_stack.empty())
            return true;
        const auto [start_line, end_line] = scope_stack.back();
        if (start_line > 0 && use_line_one_based < start_line)
            return false;
        if (end_line > 0 && use_line_one_based > end_line)
            return false;
        return true;
    }

    void maybe_set(slang::parsing::Token token, bool scope_sensitive = true) {
        if (!token || token.valueText() != name)
            return;
        if (!package_member_visible(current_package, token.valueText()))
            return;
        if (scope_sensitive && !cursor_in_innermost_scope())
            return;

        auto loc = location_from_token_actual_uri(sm, uri, token);
        const int depth = scope_sensitive ? scope_depth() : 0;
        if (!first_result || depth > first_result_depth) {
            first_result = loc;
            first_result_depth = depth;
        }
        if (scope_sensitive && !preferred_module.empty() && current_module == preferred_module &&
            (!scoped_result || depth > scoped_result_depth)) {
            scoped_result = loc;
            scoped_result_depth = depth;
        }
    }

    void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
        maybe_set(node.header->name, false);

        auto previous_module = current_module;
        auto previous_package = current_package;
        const bool is_package = node.kind == slang::syntax::SyntaxKind::PackageDeclaration;
        current_module = std::string(node.header->name.valueText());
        if (is_package)
            current_package = current_module;
        visitDefault(node);
        current_module = std::move(previous_module);
        current_package = std::move(previous_package);
    }

    void handle(const slang::syntax::ClassDeclarationSyntax& node) {
        // Class names are ordinary type identifiers at use sites:
        //
        //     packet_cfg cfg;
        //     ^^^^^^^^^^ should jump to: class packet_cfg;
        //
        // The token fallback in definition_target_at() can identify the use
        // token, but the definition search still needs to expose the class
        // declaration name as a generic definition candidate.
        maybe_set(node.name, false);
        visitDefault(node);
    }

    void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
        maybe_set(node.declarator->name);
    }

    void handle(const slang::syntax::ExplicitAnsiPortSyntax& node) { maybe_set(node.name); }

    void handle(const slang::syntax::StructUnionTypeSyntax& node) {
        // Fields declared inside a struct / union are not visible as ordinary
        // unqualified identifiers in the surrounding module or package scope.
        //
        // Example:
        //
        //     typedef struct packed {
        //         logic valid;      // field, only reachable as obj.valid
        //     } fifo_entry_t;
        //     logic valid;          // module variable
        //     assign valid = entry.valid;
        //
        // A blanket DeclaratorSyntax handler used to treat both `valid`
        // declarations as scoped module definitions because included header
        // text is parsed under the including module.  Since the field appears
        // first, go-to-definition on the LHS `valid` jumped to the aggregate
        // field.  Keep aggregate fields out of the generic lookup path; member
        // access (`entry.valid`) is resolved by the dedicated ClassMember path
        // via find_typedef_field_definition().
        ++aggregate_type_depth;
        visitDefault(node);
        --aggregate_type_depth;
    }

    void handle(const slang::syntax::DeclaratorSyntax& node) {
        if (aggregate_type_depth > 0)
            return;
        maybe_set(node.name);
    }

    // A genvar is declared through an IdentifierNameSyntax list, not a
    // DeclaratorSyntax, so the handler above never sees it and the name
    // resolved to nothing at all -- taking references and rename down with it,
    // since both start from the definition.
    void handle(const slang::syntax::GenvarDeclarationSyntax& node) {
        for (const auto* ident : node.identifiers)
            if (ident)
                maybe_set(ident->identifier);
    }

    // The inline form `for (genvar i = 0; ...)` declares `i` on the loop header
    // itself; the separate-declaration form leaves `genvar` empty and is
    // covered by the handler above.
    void handle(const slang::syntax::LoopGenerateSyntax& node) {
        if (node.genvar)
            maybe_set(node.identifier);
        visitDefault(node);
    }

    // `parameter type data_t = logic [7:0]` declares `data_t` through a
    // TypeAssignmentSyntax, not a DeclaratorSyntax, so the handler above never
    // sees it.  Like a module or class name it is a type identifier at its use
    // sites, so it is not scope-sensitive.
    void handle(const slang::syntax::TypeAssignmentSyntax& node) { maybe_set(node.name, false); }

    // `extern function void bump(int amount);` is a declaration in its own
    // right; without it the cursor on a prototype name resolves to nothing, and
    // references/rename — which both start from the definition — return empty.
    void handle(const slang::syntax::ClassMethodPrototypeSyntax& node) {
        if (node.prototype && node.prototype->name)
            if (const auto* identifier =
                    node.prototype->name->as_if<slang::syntax::IdentifierNameSyntax>())
                maybe_set(identifier->identifier, false);
        visitDefault(node);
    }

    void handle(const slang::syntax::FunctionDeclarationSyntax& node) {
        if (const auto* identifier =
                node.prototype->name->as_if<slang::syntax::IdentifierNameSyntax>())
            maybe_set(identifier->identifier);

        // Subroutine formals and locals are not visible as unqualified names in
        // the surrounding scope, and included subroutine text is parsed under
        // the including module, so descending unconditionally lets a formal
        // (e.g. `input i_data`) shadow a same-named module declaration.  Only
        // expose the body's declarations when the cursor is lexically inside
        // this subroutine.
        const auto range = node.sourceRange();
        if (!range.start().valid() || !range.end().valid())
            return;
        if (uri_from_source_location(sm, range.start()) != uri)
            return;
        const int start_line = (int)sm.getLineNumber(range.start());
        const int end_line = (int)sm.getLineNumber(range.end());
        if (use_line_one_based < start_line || use_line_one_based > end_line)
            return;

        // A subroutine body is a scope in exactly the way a begin/end block is:
        //
        //     logic [7:0] count;                                     // module
        //     function automatic f(input logic [7:0] count);         // shadows it
        //         tmp = count + 1;                                   // means the formal
        //
        // Without pushing a scope here, formals and locals are recorded at the
        // same depth as module declarations, the module one is visited first,
        // and `depth > first_result_depth` then refuses to replace it.
        push_scope(range);
        visitDefault(node);
        scope_stack.pop_back();
    }

    void handle(const slang::syntax::TypedefDeclarationSyntax& node) {
        maybe_set(node.name);
        visitDefault(node);
    }

    void handle(const slang::syntax::BlockStatementSyntax& node) {
        push_scope(node.sourceRange());
        visitDefault(node);
        scope_stack.pop_back();
    }

    void handle(const slang::syntax::ForLoopStatementSyntax& node) {
        push_scope(node.sourceRange());
        visitDefault(node);
        scope_stack.pop_back();
    }

    void handle(const slang::syntax::ForeachLoopStatementSyntax& node) {
        push_scope(node.sourceRange());
        visitDefault(node);
        scope_stack.pop_back();
    }

    // A generate block owns its declarations exactly the way a begin/end block
    // does: `logic [7:0] dout;` inside `for ... begin : g_lanes` is a different
    // signal from a module-level `dout`.
    void handle(const slang::syntax::GenerateBlockSyntax& node) {
        push_scope(node.sourceRange());
        visitDefault(node);
        scope_stack.pop_back();
    }
};

// Index-based generic definition lookup.  Replaces the full AST walk for
// extra-file searches so closed project files are also searched and open
// files no longer pay a per-request AST traversal cost.
static std::optional<Location> find_generic_definition_from_index(
    const SyntaxIndex& index, const std::string& uri, const std::string& name,
    const std::string& preferred_module, const std::string& preferred_package,
    const std::vector<ImportEntry>& visible_imports, int use_line_one_based) {

    // Returns true when a symbol declared inside `parent_scope` (a package) is
    // reachable at the cursor position given the current file's import list.
    auto pkg_visible = [&](std::string_view parent_scope, std::string_view sym_name) -> bool {
        if (parent_scope.empty() || !index.package_names.count(std::string(parent_scope)))
            return true; // not in a package — module/interface scope, always visible
        if (preferred_package == parent_scope)
            return true; // cursor is inside the same package
        for (const auto& imp : visible_imports) {
            if (imp.package_name != parent_scope)
                continue;
            if (!imp.parent_scope.empty() && imp.parent_scope != preferred_module)
                continue;
            if (imp.start_line > 0 && use_line_one_based < imp.start_line)
                continue;
            if (imp.end_line > 0 && use_line_one_based > imp.end_line)
                continue;
            if (imp.wildcard || imp.symbol_name == sym_name)
                return true;
        }
        return false;
    };

    auto make_loc = [&](SourceFileID file_id, int line, int col) -> Location {
        const auto actual_uri = index.source_uri(file_id);
        const int lsp_line = to_lsp_line(line);
        return Location{actual_uri.empty() ? uri : actual_uri, lsp_line, col, lsp_line,
                        col + (int)utf16_length(name)};
    };

    // Modules and packages — scope-insensitive, always visible (mirrors
    // GenericDefinitionVisitor::handle(ModuleDeclarationSyntax) with scope_sensitive=false).
    if (auto it = index.module_by_name.find(name); it != index.module_by_name.end() &&
                                                    it->second < index.modules.size())
        return make_loc(index.modules[it->second].file_id, index.modules[it->second].line,
                        index.modules[it->second].col);

    // Classes — scope-insensitive (mirrors handle(ClassDeclarationSyntax) scope_sensitive=false).
    if (auto it = index.class_by_name.find(name); it != index.class_by_name.end() &&
                                                   it->second < index.classes.size())
        return make_loc(index.classes[it->second].file_id, index.classes[it->second].line,
                        index.classes[it->second].col);

    // Typedefs — O(1) lookup with package visibility check.
    if (auto it = index.typedef_by_name.find(name); it != index.typedef_by_name.end() &&
                                                     it->second < index.typedefs.size()) {
        const auto& td = index.typedefs[it->second];
        if (pkg_visible(td.parent_scope, name))
            return make_loc(td.file_id, td.line, td.col);
    }

    // Enum members are ordinary named constants at use sites.  They live under
    // their typedef entry in SyntaxIndex rather than in the flat value table.
    for (const auto& td : index.typedefs) {
        if (!pkg_visible(td.parent_scope, name))
            continue;
        for (const auto& member : td.enum_members) {
            if (member.name == name)
                return make_loc(member.file_id, member.line, member.col);
        }
    }

    // Returns false for a symbol that belongs to some *other* module's scope.
    //
    // A parameter, port, or variable declared in module `foo` is not visible
    // inside module `bar`; reaching it requires hierarchical or port
    // connection, not a bare identifier.  Without this check an unresolvable
    // name in the current file silently resolves to any same-named declaration
    // in any project file — e.g. `logic [DEPTH-1:0]` in a module with no DEPTH
    // jumping into an unrelated module's `#(parameter int DEPTH = 16)`.
    //
    // An empty parent_scope is compilation-unit scope and stays visible, as do
    // package members, which pkg_visible() gates on imports instead.
    auto module_scope_visible = [&](std::string_view parent_scope) -> bool {
        if (parent_scope.empty() || parent_scope == preferred_module)
            return true;
        const std::string scope(parent_scope);
        if (index.package_names.count(scope))
            return true; // package member — pkg_visible() owns this decision
        return !index.module_by_name.count(scope);
    };

    // Values: variables, nets, functions, tasks, parameters, ports.
    // Linear scan but over a flat POD-like vector — far cheaper than an AST
    // visitor walk.  Prefer a scoped hit (same module as cursor) over a generic
    // first hit to mirror GenericDefinitionVisitor's scoped_result priority.
    std::optional<Location> first_hit;
    std::optional<Location> scoped_hit;
    for (const auto& v : index.values) {
        if (v.name != name)
            continue;
        if (!pkg_visible(v.parent_scope, name))
            continue;
        if (!module_scope_visible(v.parent_scope))
            continue;
        auto loc = make_loc(v.file_id, v.line, v.col);
        if (!first_hit)
            first_hit = loc;
        if (!preferred_module.empty() && v.parent_scope == preferred_module && !scoped_hit)
            scoped_hit = loc;
    }
    return scoped_hit ? scoped_hit : first_hit;
}

static std::optional<Location> find_generic_definition(const slang::syntax::SyntaxTree& tree,
                                                       const std::string& uri,
                                                       const std::string& name,
                                                       const std::string& preferred_module,
                                                       const std::string& preferred_package,
                                                       const std::vector<ImportEntry>& visible_imports,
                                                       int use_line_one_based) {
    GenericDefinitionVisitor visitor(tree.sourceManager(), uri, name, preferred_module,
                                     preferred_package, visible_imports, use_line_one_based);
    tree.root().visit(visitor);
    return visitor.result();
}

static std::optional<std::string> class_type_for_object_reference(const SyntaxIndex& index,
                                                                  std::string_view module_name,
                                                                  std::string_view object_name,
                                                                  int use_line_one_based) {
    if (module_name.empty() || object_name.empty())
        return std::nullopt;

    std::optional<std::string> result;
    for (const auto& value : index.values) {
        if (value.parent_scope != module_name || value.name != object_name ||
            !is_module_value_kind(value.kind))
            continue;

        // Block-local values carry a lexical visibility range.  Module-level
        // values have a zero range and are visible throughout the module.  If
        // multiple declarations with the same name are visible, prefer the
        // later one because it is the innermost/latest declaration encountered
        // by the structural index walk.
        if (value.scope_start_line > 0 && use_line_one_based < value.scope_start_line)
            continue;
        if (value.scope_end_line > 0 && use_line_one_based > value.scope_end_line)
            continue;

        const auto type = canonical_type_name_from_text(value.type);
        if (!type.empty())
            result = type;
    }
    return result;
}

/// Declared type of `object_name`, exactly as written.
///
/// class_type_for_object_reference() canonicalises to the trailing identifier,
/// which is right for `pkg::packet_t` but wrong for an interface port: the type
/// text is `AXI_BUS.Slave`, whose trailing identifier is the modport.
static std::optional<std::string> declared_type_text_for_object_reference(
    const SyntaxIndex& index, std::string_view module_name, std::string_view object_name,
    int use_line_one_based) {
    if (module_name.empty() || object_name.empty())
        return std::nullopt;

    std::optional<std::string> result;
    for (const auto& value : index.values) {
        if (value.parent_scope != module_name || value.name != object_name ||
            !is_module_value_kind(value.kind))
            continue;
        if (value.scope_start_line > 0 && use_line_one_based < value.scope_start_line)
            continue;
        if (value.scope_end_line > 0 && use_line_one_based > value.scope_end_line)
            continue;
        if (!value.type.empty())
            result = value.type;
    }
    return result;
}

/// Interface half of a declared-type text: `AXI_BUS.Slave` -> `AXI_BUS`, and
/// `virtual bus_if #(.W_ADDR(8))` -> `bus_if`, so a virtual interface handle
/// reaches its members like the interface instance it points at.
static std::string interface_name_from_type_text(std::string_view type_text) {
    return base_type_identifier(type_text);
}

/// A signal, modport or parameter declared inside interface @p interface_name.
///
/// Everything needed is already in the shard — `interface_names` says which
/// modules are interfaces, `ModuleEntry::modports` carries each modport's
/// location, and the interface's signals are ordinary ports and values — so
/// this is lookup only, with no extra indexing work.
static std::optional<Location> find_interface_member_definition(const SyntaxIndex& index,
                                                                const std::string& uri,
                                                                std::string_view interface_name,
                                                                std::string_view member_name) {
    if (interface_name.empty() || member_name.empty())
        return std::nullopt;
    if (!index.interface_names.contains(std::string(interface_name)))
        return std::nullopt;

    const auto it = index.module_by_name.find(std::string(interface_name));
    if (it == index.module_by_name.end() || it->second >= index.modules.size())
        return std::nullopt;
    const auto& iface = index.modules[it->second];

    const auto locate = [&](SourceFileID file_id, int line, int col, size_t name_size)
        -> std::optional<Location> {
        if (line <= 0)
            return std::nullopt;
        const auto actual_uri = index.source_uri(file_id);
        const int lsp_line = to_lsp_line(line);
        return Location{actual_uri.empty() ? uri : actual_uri, lsp_line, col, lsp_line,
                        col + (int)name_size};
    };

    for (const auto& modport : iface.modports) {
        if (modport.name == member_name)
            return locate(modport.file_id, modport.line, modport.col, utf16_length(modport.name));
    }
    for (const auto& port : iface.ports) {
        if (port.name == member_name)
            return locate(port.file_id, port.line, port.col, utf16_length(port.name));
    }
    for (const auto& value : index.values) {
        if (value.parent_scope != interface_name || value.name != member_name)
            continue;
        return locate(value.file_id, value.line, value.col, utf16_length(value.name));
    }
    return std::nullopt;
}

/// Instantiation named @p instance_name inside @p module_name.
///
/// `u_leaf.state_q` resolves `state_q`, but a cursor on `u_leaf` itself is a
/// plain identifier that no declarator matches, so it used to resolve to
/// nothing.  InstanceEntry already records where the instance is written.
static std::optional<Location> find_instance_definition(const SyntaxIndex& index,
                                                        const std::string& uri,
                                                        std::string_view module_name,
                                                        std::string_view instance_name) {
    if (instance_name.empty())
        return std::nullopt;
    for (const auto& inst : index.instances) {
        if (inst.instance_name != instance_name)
            continue;
        if (!module_name.empty() && !inst.parent_module.empty() &&
            inst.parent_module != module_name)
            continue;
        if (inst.line <= 0)
            continue;
        const auto actual_uri = index.source_uri(inst.file_id);
        const int lsp_line = to_lsp_line(inst.line);
        // InstanceEntry keeps the instantiation line but no column for the name,
        // so anchor at the start of the line the instance is written on.
        return Location{actual_uri.empty() ? uri : actual_uri, lsp_line, 0, lsp_line, 0};
    }
    return std::nullopt;
}

/// Declaration of @p member_name inside the generate block labelled @p label.
///
/// `gen_stall_mem.rf_rd_a_hz` names a signal declared inside `begin :
/// gen_stall_mem`.  A generate label is not a value, so the receiver had no
/// type and the member lookup never started.  The block and the reference are
/// in the same file by construction, so this reads the current AST rather than
/// asking the index to carry a per-declaration block name.
static std::optional<Location> find_generate_block_member_in_tree(
    const slang::syntax::SyntaxTree& tree, const std::string& uri, std::string_view label,
    std::string_view member_name) {
    if (label.empty() || member_name.empty())
        return std::nullopt;

    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        std::string_view label;
        std::string_view member_name;
        int depth{0};
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, std::string_view label,
                std::string_view member_name)
            : sm(sm), uri(uri), label(label), member_name(member_name) {}

        void handle(const slang::syntax::GenerateBlockSyntax& node) {
            const auto* name_clause = node.beginName ? node.beginName : node.endName;
            const bool matches = name_clause && name_clause->name.valueText() == label;
            if (matches)
                ++depth;
            visitDefault(node);
            if (matches)
                --depth;
        }

        void handle(const slang::syntax::DeclaratorSyntax& node) {
            if (result || depth == 0 || !node.name || node.name.valueText() != member_name)
                return;
            result = location_from_token_actual_uri(sm, uri, node.name);
        }
    };

    Visitor visitor(tree.sourceManager(), uri, label, member_name);
    tree.root().visit(visitor);
    return visitor.result;
}

/// True when this file declares a generate block labelled @p label.
///
/// Used to tell a hierarchical address (`g_lane[0].acc`) from a handle whose
/// type simply could not be resolved: for the former, falling back to a
/// same-named signal of the enclosing scope answers with a different object.
static bool generate_block_label_exists_in_tree(const slang::syntax::SyntaxTree& tree,
                                                std::string_view label) {
    if (label.empty())
        return false;

    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        std::string_view label;
        bool found{false};

        explicit Visitor(std::string_view label) : label(label) {}

        void handle(const slang::syntax::GenerateBlockSyntax& node) {
            const auto* name_clause = node.beginName ? node.beginName : node.endName;
            if (name_clause && name_clause->name.valueText() == label)
                found = true;
            if (!found)
                visitDefault(node);
        }
    };

    Visitor visitor(label);
    tree.root().visit(visitor);
    return visitor.found;
}

static std::pair<int, int> source_range_lines_one_based(const slang::SourceManager& sm,
                                                        slang::SourceRange range) {
    if (!range.start().valid() || !range.end().valid())
        return {0, 0};
    const auto start = sm.getLineNumber(range.start());
    const auto end = sm.getLineNumber(range.end());
    return {start > 0 ? (int)start : 0, end > 0 ? (int)end : 0};
}

static std::optional<std::string> class_type_for_object_reference_in_tree(
    const slang::syntax::SyntaxTree& tree, const std::string& uri, std::string_view module_name,
    std::string_view object_name, int use_line_one_based) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        std::string_view module_name;
        std::string_view object_name;
        int use_line_one_based;
        std::string current_module;
        std::vector<std::pair<int, int>> scope_stack;
        std::optional<std::string> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri,
                std::string_view module_name, std::string_view object_name,
                int use_line_one_based)
            : sm(sm), uri(uri), module_name(module_name), object_name(object_name),
              use_line_one_based(use_line_one_based) {}

        void maybe_set(const slang::syntax::DataTypeSyntax& type,
                       const slang::syntax::SeparatedSyntaxList<slang::syntax::DeclaratorSyntax>& declarators) {
            if (current_module != module_name)
                return;
            const auto [scope_start, scope_end] =
                scope_stack.empty() ? std::pair<int, int>{0, 0} : scope_stack.back();
            if (scope_start > 0 && use_line_one_based < scope_start)
                return;
            if (scope_end > 0 && use_line_one_based > scope_end)
                return;
            for (const auto* decl : declarators) {
                if (!decl || decl->name.valueText() != object_name)
                    continue;
                const auto decl_uri = uri_from_source_location(sm, decl->name.location());
                if (!decl_uri.empty() && decl_uri != uri)
                    continue;
                const auto type_name = canonical_type_name_from_text(render_syntax_node_text(sm, type));
                if (!type_name.empty())
                    result = type_name;
            }
        }

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            const auto previous_module = current_module;
            current_module = std::string(node.header->name.valueText());
            scope_stack.push_back(source_range_lines_one_based(sm, node.sourceRange()));
            visitDefault(node);
            scope_stack.pop_back();
            current_module = previous_module;
        }

        // A class body is a declaration scope too, so `p.foo()` inside a method
        // has to find `p` among the class properties and method locals.
        void handle(const slang::syntax::ClassDeclarationSyntax& node) {
            const auto previous_module = current_module;
            current_module = std::string(node.name.valueText());
            scope_stack.push_back(source_range_lines_one_based(sm, node.sourceRange()));
            visitDefault(node);
            scope_stack.pop_back();
            current_module = previous_module;
        }

        // `task run_phase(uvm_phase phase);` — the receiver is an argument.
        void handle(const slang::syntax::FunctionPortSyntax& node) {
            if (current_module != module_name || !node.dataType || !node.declarator)
                return;
            if (node.declarator->name.valueText() != object_name)
                return;
            const auto decl_uri = uri_from_source_location(sm, node.declarator->name.location());
            if (!decl_uri.empty() && decl_uri != uri)
                return;
            const auto type_name =
                canonical_type_name_from_text(render_syntax_node_text(sm, *node.dataType));
            if (!type_name.empty())
                result = type_name;
        }

        void handle(const slang::syntax::BlockStatementSyntax& node) {
            scope_stack.push_back(source_range_lines_one_based(sm, node.sourceRange()));
            visitDefault(node);
            scope_stack.pop_back();
        }

        void handle(const slang::syntax::LocalVariableDeclarationSyntax& node) {
            maybe_set(*node.type, node.declarators);
            visitDefault(node);
        }

        void handle(const slang::syntax::DataDeclarationSyntax& node) {
            maybe_set(*node.type, node.declarators);
            visitDefault(node);
        }
    };

    Visitor visitor(tree.sourceManager(), uri, module_name, object_name, use_line_one_based);
    tree.root().visit(visitor);
    return visitor.result;
}

/// Declared type of `type_name.field_name` when `type_name` is a typedef'd
/// struct or union.  The struct-shaped counterpart of
/// find_class_field_type_in_hierarchy(); aggregates have no base type to walk.
static std::optional<std::string> find_typedef_field_type(const SyntaxIndex& index,
                                                          std::string_view type_name,
                                                          std::string_view field_name) {
    if (type_name.empty() || field_name.empty())
        return std::nullopt;

    for (const auto& td : index.typedefs) {
        const std::string scoped_name =
            td.parent_scope.empty() ? td.name : td.parent_scope + "::" + td.name;
        if (td.name != type_name && scoped_name != type_name)
            continue;
        for (const auto& field : td.fields) {
            if (field.name != field_name || field.type.empty())
                continue;
            auto type = canonical_type_name_from_text(field.type);
            if (!type.empty())
                return type;
        }
    }
    return std::nullopt;
}

/// The type an alias stands for, or nothing when @p type_name is not an alias.
///
/// `parameter type T_BEAT = corner_pkg::outer_t` is indexed as a typedef whose
/// `resolved` text names the default type but which carries no fields of its
/// own; a plain `typedef outer_t beat_t;` has the same shape.  Members are
/// declared on the type the alias points at, so member lookup has to take that
/// hop.  A typedef that owns fields is an aggregate and ends the chain.
///
/// The *default* is what an alias resolves to: an instantiation-time parameter
/// override is not visible from the file being edited, which is the same choice
/// the rest of the parameter handling already makes.
static std::optional<std::string> typedef_alias_target(const SyntaxIndex& index,
                                                       std::string_view type_name) {
    if (type_name.empty())
        return std::nullopt;
    for (const auto& td : index.typedefs) {
        if (td.name != type_name || !td.fields.empty() || td.resolved.empty())
            continue;
        auto target = canonical_type_name_from_text(td.resolved);
        if (!target.empty() && target != type_name)
            return target;
    }

    // A `parameter type` is recorded as a parameter whose type text is
    // literally "type" and whose default value is the type it stands for —
    // as a header parameter port, or as a value for a body parameter.
    const auto from_default = [&](std::string_view default_value) -> std::optional<std::string> {
        auto target = canonical_type_name_from_text(default_value);
        if (target.empty() || target == type_name)
            return std::nullopt;
        return target;
    };
    for (const auto& module : index.modules) {
        for (const auto& port : module.ports) {
            if (port.name != type_name || port.type != "type" || port.default_value.empty())
                continue;
            if (auto target = from_default(port.default_value))
                return target;
        }
    }
    for (const auto& value : index.values) {
        if (value.name != type_name || value.type != "type" || value.default_value.empty())
            continue;
        if (auto target = from_default(value.default_value))
            return target;
    }
    return std::nullopt;
}

static std::optional<Location> find_typedef_field_definition(const SyntaxIndex& index,
                                                            const std::string& uri,
                                                            std::string_view type_name,
                                                            std::string_view field_name) {
    if (type_name.empty() || field_name.empty())
        return std::nullopt;

    for (const auto& td : index.typedefs) {
        const std::string scoped_name =
            td.parent_scope.empty() ? td.name : td.parent_scope + "::" + td.name;
        if (td.name != type_name && scoped_name != type_name)
            continue;
        for (const auto& field : td.fields) {
            if (field.name != field_name || field.line <= 0)
                continue;
            const std::string actual_uri = index.source_uri(field.file_id);
            const int line = to_lsp_line(field.line);
            return Location{actual_uri.empty() ? uri : actual_uri, line, field.col, line,
                            field.col + (int)utf16_length(field.name)};
        }
    }
    return std::nullopt;
}

static std::optional<Location> find_aggregate_field_declaration_at(const SyntaxIndex& index,
                                                                   const std::string& uri,
                                                                   std::string_view field_name,
                                                                   int lsp_line,
                                                                   int lsp_col) {
    if (field_name.empty())
        return std::nullopt;

    auto field_location_if_clicked = [&](const FieldEntry& field) -> std::optional<Location> {
        if (field.name != field_name || field.line <= 0 || field.col != lsp_col)
            return std::nullopt;

        const int field_lsp_line = to_lsp_line(field.line);
        if (field_lsp_line != lsp_line)
            return std::nullopt;

        const std::string actual_uri = index.source_uri(field.file_id);
        const std::string resolved_uri = actual_uri.empty() ? uri : actual_uri;
        if (resolved_uri != uri)
            return std::nullopt;

        return Location{resolved_uri, field_lsp_line, field.col, field_lsp_line,
                        field.col + (int)utf16_length(field.name)};
    };

    // Generic unqualified lookup intentionally ignores aggregate fields, but
    // requests that start on the declaration token itself still need a stable
    // definition location.  Hover and references both ask definition_of() first
    // even for declarations:
    //
    //     typedef struct { logic addr; } packet_t;
    //                            ^ hover / references here
    //
    // Without this exact-location path, excluding fields from generic lookup
    // would make declaration-origin requests look unresolved.
    for (const auto& td : index.typedefs) {
        for (const auto& field : td.fields) {
            if (auto loc = field_location_if_clicked(field))
                return loc;
        }
    }
    for (const auto& cls : index.classes) {
        for (const auto& field : cls.fields) {
            if (auto loc = field_location_if_clicked(field))
                return loc;
        }
    }
    return std::nullopt;
}

static std::optional<Location> find_class_method_definition(const SyntaxIndex& index,
                                                            const std::string& uri,
                                                            std::string_view class_name,
                                                            std::string_view method_name) {
    if (class_name.empty() || method_name.empty())
        return std::nullopt;

    for (const auto& cls : index.classes) {
        const std::string class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        if (class_scope != class_name && cls.name != class_name)
            continue;
        for (const auto& method : cls.methods) {
            if (method.name != method_name || method.line <= 0)
                continue;
            const std::string actual_uri = index.source_uri(method.file_id);
            const int line = to_lsp_line(method.line);
            return Location{actual_uri.empty() ? uri : actual_uri, line, method.col, line,
                            method.col + (int)utf16_length(method.name)};
        }
    }
    return std::nullopt;
}

static std::optional<Location> find_class_member_definition(const SyntaxIndex& index,
                                                            const std::string& uri,
                                                            std::string_view class_name,
                                                            std::string_view member_name) {
    if (class_name.empty() || member_name.empty())
        return std::nullopt;

    for (const auto& cls : index.classes) {
        const std::string class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        if (class_scope != class_name && cls.name != class_name)
            continue;

        // Class properties and class methods share the same member-access
        // syntax at the use site (`obj.member`).  Closed files are represented
        // only by SyntaxIndex shards, so both member families must be resolved
        // from compact index facts here; otherwise hover falls through to the
        // generic "symbol" fallback even though the definition location is known.
        for (const auto& field : cls.fields) {
            if (field.name != member_name || field.line <= 0)
                continue;
            const std::string actual_uri = index.source_uri(field.file_id);
            const int line = to_lsp_line(field.line);
            return Location{actual_uri.empty() ? uri : actual_uri, line, field.col, line,
                            field.col + (int)utf16_length(field.name)};
        }
        for (const auto& method : cls.methods) {
            if (method.name != member_name || method.line <= 0)
                continue;
            const std::string actual_uri = index.source_uri(method.file_id);
            const int line = to_lsp_line(method.line);
            return Location{actual_uri.empty() ? uri : actual_uri, line, method.col, line,
                            method.col + (int)utf16_length(method.name)};
        }

        // A class-scoped typedef (`my_item::type_id`) is a member too, but it
        // lives in the typedef table rather than on the ClassEntry.
        for (const auto& td : index.typedefs) {
            if (td.name != member_name || td.line <= 0 || td.parent_scope != cls.name)
                continue;
            const std::string actual_uri = index.source_uri(td.file_id);
            const int line = to_lsp_line(td.line);
            return Location{actual_uri.empty() ? uri : actual_uri, line, td.col, line,
                            td.col + (int)utf16_length(td.name)};
        }
    }
    return std::nullopt;
}

// One SyntaxIndex shard plus the URI to fall back on when the shard carries no
// source file of its own.  Class hierarchies routinely span shards — a child in
// the current file extending a base in a closed project file — so member lookup
// has to be able to hop between them.
struct ClassLookupShard {
    const SyntaxIndex* index;
    const std::string* uri;
};

static const ClassEntry* find_class_entry(const SyntaxIndex& index, std::string_view class_name) {
    auto it = index.class_by_name.find(std::string(class_name));
    if (it == index.class_by_name.end() || it->second >= index.classes.size())
        return nullptr;
    return &index.classes[it->second];
}

// Resolve `member_name` on `class_name`, then on its `extends` ancestors.
//
// SystemVerilog inherits properties and methods, so `child.depth` and a bare
// `depth` inside a child class body both have to reach `pkt_base::depth`.  Each
// hop re-searches every shard because the base class is frequently declared in
// a different file than the child.
static std::optional<Location> find_class_member_in_hierarchy(
    std::span<const ClassLookupShard> shards, std::string class_name,
    std::string_view member_name) {
    std::unordered_set<std::string> visited;
    while (!class_name.empty() && visited.insert(class_name).second) {
        for (const auto& shard : shards) {
            if (auto loc = find_class_member_definition(*shard.index, *shard.uri, class_name,
                                                        member_name))
                return loc;
        }

        std::string base;
        for (const auto& shard : shards) {
            const auto* cls = find_class_entry(*shard.index, class_name);
            if (cls && !cls->base_class.empty()) {
                base = base_class_lookup_name(cls->base_class);
                break;
            }
        }
        class_name = std::move(base);
    }
    return std::nullopt;
}

// Whether `derived` reaches `base` through its `extends` chain.
//
// Each hop re-searches every shard for the same reason the member lookup above
// does: a base class is usually declared in a different file than the class
// that extends it.
static bool class_derives_from(std::span<const ClassLookupShard> shards, std::string derived,
                               std::string_view base) {
    std::unordered_set<std::string> visited;
    while (!derived.empty() && visited.insert(derived).second) {
        if (derived == base)
            return true;

        std::string next;
        for (const auto& shard : shards) {
            const auto* cls = find_class_entry(*shard.index, derived);
            if (cls && !cls->base_class.empty()) {
                next = base_class_lookup_name(cls->base_class);
                break;
            }
        }
        derived = std::move(next);
    }
    return false;
}

/// Class that @p class_name extends, searched across shards.
///
/// `super.member` names a member of the base class specifically.  Letting it
/// resolve against the derived class instead is only invisible while the derived
/// class does not redeclare the member -- `super.new()` always does.
static std::optional<std::string> find_class_base_name(std::span<const ClassLookupShard> shards,
                                                       std::string_view class_name) {
    if (class_name.empty())
        return std::nullopt;
    for (const auto& shard : shards) {
        const auto* cls = find_class_entry(*shard.index, class_name);
        if (cls && !cls->base_class.empty())
            return base_class_lookup_name(cls->base_class);
    }
    return std::nullopt;
}

// Declared type of `field_name` on `class_name` or any ancestor, searched
// across shards.  The receiver of a member access is often itself an inherited
// field, e.g. `seq_item_port` declared by `uvm_driver` and used from a subclass.
static std::optional<std::string> find_class_field_type_in_hierarchy(
    std::span<const ClassLookupShard> shards, std::string class_name,
    std::string_view field_name) {
    std::unordered_set<std::string> visited;
    while (!class_name.empty() && visited.insert(class_name).second) {
        std::string base;
        for (const auto& shard : shards) {
            const auto* cls = find_class_entry(*shard.index, class_name);
            if (!cls)
                continue;
            for (const auto& field : cls->fields) {
                if (field.name != field_name || field.type.empty())
                    continue;
                const auto type = canonical_type_name_from_text(field.type);
                if (!type.empty())
                    return type;
            }
            if (base.empty() && !cls->base_class.empty())
                base = base_class_lookup_name(cls->base_class);
        }
        class_name = std::move(base);
    }
    return std::nullopt;
}

// Declaration of `member_name` written inside `class_name`'s body in this tree.
//
// `Class::member` is not only a static method call: it also reaches a nested
// typedef, which is how the utils macros publish `type_id`.  Those typedefs are
// produced by macro expansion, so the answer is wherever the token really came
// from, which is what location_from_token_actual_uri() reports.
static std::optional<Location> find_class_scoped_declaration_in_tree(
    const slang::syntax::SyntaxTree& tree, const std::string& uri, std::string_view class_name,
    std::string_view member_name) {
    if (class_name.empty() || member_name.empty())
        return std::nullopt;
    using namespace slang::syntax;

    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        std::string_view class_name;
        std::string_view member_name;
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, std::string_view class_name,
                std::string_view member_name)
            : sm(sm), uri(uri), class_name(class_name), member_name(member_name) {}

        void accept(const slang::parsing::Token& token) {
            if (!result && token && token.valueText() == member_name)
                result = location_from_token_actual_uri(sm, uri, token);
        }

        void handle(const ClassDeclarationSyntax& node) {
            if (std::string_view(node.name.valueText()) != class_name) {
                visitDefault(node);
                return;
            }
            for (const auto* item : node.items) {
                if (!item)
                    continue;
                if (const auto* td = item->as_if<TypedefDeclarationSyntax>()) {
                    accept(td->name);
                } else if (const auto* prop = item->as_if<ClassPropertyDeclarationSyntax>()) {
                    if (const auto* nested = prop->declaration->as_if<TypedefDeclarationSyntax>())
                        accept(nested->name);
                    else if (const auto* data = prop->declaration->as_if<DataDeclarationSyntax>()) {
                        for (const auto* decl : data->declarators)
                            if (decl)
                                accept(decl->name);
                    }
                } else if (const auto* method = item->as_if<ClassMethodDeclarationSyntax>()) {
                    if (const auto* id =
                            method->declaration->prototype->name->as_if<IdentifierNameSyntax>())
                        accept(id->identifier);
                } else if (const auto* proto = item->as_if<ClassMethodPrototypeSyntax>()) {
                    if (const auto* id = proto->prototype->name->as_if<IdentifierNameSyntax>())
                        accept(id->identifier);
                }
            }
        }
    } visitor(tree.sourceManager(), uri, class_name, member_name);

    tree.root().visit(visitor);
    return visitor.result;
}

static bool token_at_location(const slang::SourceManager& sm, const slang::parsing::Token& token,
                              const Location& location) {
    if (!token || !token.location().valid())
        return false;
    const auto token_location = location_from_token_actual_uri(sm, location.uri, token);
    return token_location.uri == location.uri && token_location.line == location.line &&
           token_location.col == location.col;
}

static std::string token_text(const slang::parsing::Token& token) {
    return token ? std::string(token.valueText()) : std::string{};
}

static std::string name_text(const slang::syntax::NameSyntax& name) {
    if (const auto* identifier = name.as_if<slang::syntax::IdentifierNameSyntax>())
        return token_text(identifier->identifier);
    return trim_copy(name.toString());
}

static std::optional<SymbolInfo>
symbol_info_from_definition(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                            const std::string& name, const Location& definition,
                            const SyntaxIndex* prebuilt_index = nullptr,
                            const std::string& owner = {}) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& name;
        const Location& definition;
        const SyntaxIndex& index;
        // Class named to the left of `::`, empty when the cursor was unqualified.
        const std::string& owner;
        std::vector<std::string> enclosing_class;
        std::optional<SymbolInfo> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, const std::string& name,
                const Location& definition, const SyntaxIndex& index, const std::string& owner)
            : sm(sm), uri(uri), name(name), definition(definition), index(index), owner(owner) {}

        void set_from_token(const slang::parsing::Token& token, std::string kind,
                            std::string detail) {
            if (result || token.valueText() != name || !token_at_location(sm, token, definition))
                return;
            result = SymbolInfo{.name = name,
                                .kind = std::move(kind),
                                .detail = std::move(detail),
                                .line = definition.line,
                                .col = definition.col};
        }

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            if (result || node.header->name.valueText() != name ||
                !token_at_location(sm, node.header->name, definition)) {
                visitDefault(node);
                return;
            }

            std::string doc;
            for (const auto& module : index.modules) {
                if (module.name == name) {
                    doc = module_doc_from_entry(module);
                    break;
                }
            }

            // ModuleDeclarationSyntax also covers interface, package, and
            // program declarations; reporting all of them as "module" mislabels
            // every interface in a testbench.
            const char* declaration_kind =
                node.kind == slang::syntax::SyntaxKind::InterfaceDeclaration ? "interface"
                : node.kind == slang::syntax::SyntaxKind::PackageDeclaration ? "package"
                : node.kind == slang::syntax::SyntaxKind::ProgramDeclaration ? "program"
                                                                             : "module";

            result = SymbolInfo{.name = name,
                                .kind = declaration_kind,
                                .detail = declaration_kind,
                                .doc = std::move(doc),
                                .line = definition.line,
                                .col = definition.col};
        }

        void handle(const slang::syntax::ClassDeclarationSyntax& node) {
            if (result || node.name.valueText() != name ||
                !token_at_location(sm, node.name, definition)) {
                enclosing_class.emplace_back(node.name.valueText());
                visitDefault(node);
                enclosing_class.pop_back();
                return;
            }

            result = SymbolInfo{.name = name,
                                .kind = "class",
                                .detail = "class",
                                .line = definition.line,
                                .col = definition.col};
        }

        void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
            if (!node.declarator)
                return;
            std::string detail;
            if (node.header) {
                if (const auto* variable =
                        node.header->as_if<slang::syntax::VariablePortHeaderSyntax>()) {
                    detail = token_text(variable->direction);
                    auto type = render_declaration_type_text(sm, *variable->dataType);
                    if (!type.empty())
                        detail += (detail.empty() ? "" : " ") + type;
                } else if (const auto* net =
                               node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                    detail = token_text(net->direction);
                    auto type = render_declaration_type_text(sm, *net->dataType);
                    if (!type.empty())
                        detail += (detail.empty() ? "" : " ") + type;
                }
            }

            // ANSI ports have the same split-type shape as ordinary
            // declarations: the header owns the direction and shared packed
            // type, while the declarator owns any unpacked dimensions.
            //
            //     module m(input logic [1:0] i_data [7:0]);
            //              ^^^^^^^^^^^^^^^^^ shared header type
            //                                      ^^^^^ declarator dimension
            //
            // Hover should show the full object type, not just the header type.
            detail = append_hover_suffix(detail,
                                         render_hover_dimensions(sm, node.declarator->dimensions));
            set_from_token(node.declarator->name, "port", detail);
        }

        void handle(const slang::syntax::ExplicitAnsiPortSyntax& node) {
            set_from_token(node.name, "port", token_text(node.direction));
        }

        void handle(const slang::syntax::PortDeclarationSyntax& node) {
            std::string detail;
            if (const auto* variable =
                    node.header->as_if<slang::syntax::VariablePortHeaderSyntax>()) {
                detail = token_text(variable->direction);
                auto type = render_declaration_type_text(sm, *variable->dataType);
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            } else if (const auto* net = node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                detail = token_text(net->direction);
                auto type = render_declaration_type_text(sm, *net->dataType);
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            }
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                // Non-ANSI port declarations can also place unpacked
                // dimensions on individual declarators:
                //
                //     input logic [1:0] a [7:0], b [3:0];
                //
                // The header detail is shared, so copy it before appending each
                // declarator's dimensions.
                auto port_detail = detail;
                port_detail =
                    append_hover_suffix(port_detail,
                                        render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "port", port_detail);
            }
        }

        void handle(const slang::syntax::DataDeclarationSyntax& node) {
            // A data declaration's syntactic data type is shared by every
            // declarator in the declaration:
            //
            //     logic [7:0] a, b [4];
            //     ^^^^^^^^^^^ shared DataDeclarationSyntax::type
            //
            // SystemVerilog also allows each declarator to carry unpacked
            // dimensions.  Those dimensions are part of the declared object's
            // full type even though they are syntactically attached to the
            // declarator instead of the shared data type.  Hover should
            // therefore show:
            //
            //     a -> logic [7:0]
            //     b -> logic [7:0] [4]
            //
            // rather than the previous empty detail string, which made a
            // variable hover display only "**name** — *variable*".
            auto base_type = render_declaration_type_text(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto detail = base_type;
                detail = append_hover_suffix(detail,
                                             render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "variable", detail);
            }
        }

        // Hover on a genvar: without these the token resolved to a location but
        // no declaration matched it, so hover degraded to the bare
        // "**gi** — *symbol*" fallback.
        void handle(const slang::syntax::GenvarDeclarationSyntax& node) {
            for (const auto* ident : node.identifiers)
                if (ident)
                    set_from_token(ident->identifier, "genvar", "genvar");
        }

        void handle(const slang::syntax::LoopGenerateSyntax& node) {
            if (node.genvar)
                set_from_token(node.identifier, "genvar", "genvar");
            visitDefault(node);
        }

        void handle(const slang::syntax::NetDeclarationSyntax& node) {
            auto detail = token_text(node.netType);
            auto type = render_declaration_type_text(sm, *node.type);
            if (!type.empty())
                detail += (detail.empty() ? "" : " ") + type;
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto net_detail = detail;
                net_detail = append_hover_suffix(net_detail,
                                                 render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "net", net_detail);
            }
        }

        void handle(const slang::syntax::StructUnionMemberSyntax& node) {
            // Struct / union fields are not DataDeclarationSyntax nodes.  They
            // have their own StructUnionMemberSyntax shape:
            //
            //     typedef struct {
            //         logic [7:0] addr;
            //         logic       valid;
            //     } packet_t;
            //
            // Generic definition lookup can correctly land on the field's
            // DeclaratorSyntax, but hover must still reconstruct the field's
            // declaration type from StructUnionMemberSyntax::type plus any
            // declarator-local unpacked dimensions.
            //
            // Rendered expanded: a struct declared inside a shared header's
            // macro body would otherwise report every field's type as the macro
            // invocation the user wrote, which is not a type at all.
            auto base_type = render_syntax_node_text_expanded(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto detail = base_type;
                detail = append_hover_suffix(detail,
                                             render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "field", detail);
            }
        }

        void handle(const slang::syntax::LocalVariableDeclarationSyntax& node) {
            // Local declarations use a different syntax node from module/class
            // data declarations, but the hover policy is the same: show the
            // variable kind plus the declared data type.  Keep this in the
            // definition-to-symbol layer instead of the hover renderer so hover
            // formatting remains a pure presentation step over SymbolInfo.
            auto base_type = render_declaration_type_text(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto detail = base_type;
                detail = append_hover_suffix(detail,
                                             render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "variable", detail);
            }
        }

        void handle(const slang::syntax::ParameterDeclarationSyntax& node) {
            auto detail = render_declaration_type_text(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;
                auto param_detail = detail;
                if (declarator->initializer)
                    param_detail += (param_detail.empty() ? "" : " ") + std::string("= ") +
                                    trim_copy(declarator->initializer->expr->toString());
                set_from_token(declarator->name, "parameter", param_detail);
            }
        }

        void handle(const slang::syntax::FunctionPortSyntax& node) {
            auto detail = token_text(node.direction);
            if (node.dataType) {
                auto type = render_declaration_type_text(sm, *node.dataType);
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            }
            detail = append_hover_suffix(detail,
                                         render_hover_dimensions(sm, node.declarator->dimensions));
            set_from_token(node.declarator->name, "argument", detail);
        }

        void handle(const slang::syntax::FunctionDeclarationSyntax& node) {
            if (const auto* identifier =
                    node.prototype->name->as_if<slang::syntax::IdentifierNameSyntax>()) {
                auto kind =
                    node.kind == slang::syntax::SyntaxKind::TaskDeclaration ? "task" : "function";
                std::string doc;
                std::string detail(kind);
                if (node.kind == slang::syntax::SyntaxKind::TaskDeclaration) {
                    auto ports =
                        node.prototype->portList ? node.prototype->portList->toString() : "";
                    doc = "```\ntask " + name + format_ports_doc(ports) + "\n```";
                } else {
                    auto return_type = render_syntax_node_text(sm, *node.prototype->returnType);
                    auto ports =
                        node.prototype->portList ? node.prototype->portList->toString() : "";
                    doc = "```\nfunction " + return_type + " " + name + format_ports_doc(ports) +
                          "\n```";
                }
                if (identifier->identifier.valueText() == name &&
                    token_at_location(sm, identifier->identifier, definition)) {
                    result = SymbolInfo{.name = name,
                                        .kind = kind,
                                        .detail = detail,
                                        .doc = std::move(doc),
                                        .line = definition.line,
                                        .col = definition.col};
                }
            }
            if (!result)
                visitDefault(node);
        }

        void handle(const slang::syntax::TypedefDeclarationSyntax& node) {
            // A macro that declares a member expands once per class, and every
            // expansion reports the macro body's own location -- UVM's factory
            // macros give each registered class its own `type_id` this way.
            // Location alone therefore cannot tell them apart, so when the
            // cursor named an owner, only that owner's copy may answer.
            if (owner.empty() || (!enclosing_class.empty() && enclosing_class.back() == owner))
                set_from_token(node.name, "typedef", render_syntax_node_text(sm, *node.type));

            // An enum member's useful hover fact is the enum it belongs to, and
            // that name lives on the typedef, not on the member.  Without this
            // a member declared in the file being edited hovered as a bare
            // "symbol" with no body at all.
            if (!result) {
                if (const auto* enum_type =
                        node.type->as_if<slang::syntax::EnumTypeSyntax>()) {
                    const std::string enum_name(node.name.valueText());
                    for (const auto* member : enum_type->members) {
                        if (member)
                            set_from_token(member->name, "enum_member", enum_name);
                    }
                }
            }
            if (!result)
                visitDefault(node);
        }
    };

    // Hover/symbol info is a request-local AST query.  Older code built a full
    // SyntaxIndex here when the caller did not provide one, solely to enrich a
    // module hover document.  That recreated the exact anti-pattern we are
    // removing: a point query should not silently index the whole current file.
    SyntaxIndex empty_index;
    const auto& index = prebuilt_index ? *prebuilt_index : empty_index;
    Visitor visitor(tree.sourceManager(), uri, name, definition, index, owner);
    tree.root().visit(visitor);
    return visitor.result;
}

static slang::SourceRange visible_range_for_token(const slang::SourceManager& sm,
                                                  const slang::parsing::Token& token) {
    // Macro *argument* tokens are text the user typed at the call site, so they
    // occupy their own original range there.  Only tokens that come from the
    // macro body have no place of their own in the source and must fall back to
    // the whole invocation.
    if (sm.isMacroArgLoc(token.location())) {
        const auto start = sm.getFullyOriginalLoc(token.location());
        return slang::SourceRange(start, start + token.rawText().length());
    }
    if (sm.isMacroLoc(token.location()))
        return sm.getExpansionRange(token.location());
    return token.range();
}

static bool contains_position(const slang::SourceManager& sm, slang::SourceRange range, int line,
                              int col) {
    if (!range.start().valid() || !range.end().valid())
        return false;

    // Compared against a request position, which the client measures in UTF-16.
    const int start_line = to_lsp_line((int)sm.getLineNumber(range.start()));
    const int start_col = utf16_column(sm, range.start());
    const int end_line = to_lsp_line((int)sm.getLineNumber(range.end()));
    const int end_col = utf16_column(sm, range.end());

    if (line < start_line || line > end_line)
        return false;
    if (line == start_line && col < start_col)
        return false;
    if (line == end_line && col >= end_col)
        return false;
    return true;
}

static bool range_starts_in_uri(const slang::SourceManager& sm, slang::SourceRange range,
                                const std::string& uri) {
    if (!range.start().valid())
        return false;
    return location_to_uri(sm, range.start(), uri) == uri;
}

static bool contains_position_in_uri(const slang::SourceManager& sm, slang::SourceRange range,
                                     const std::string& uri, int line, int col) {
    return range_starts_in_uri(sm, range, uri) && contains_position(sm, range, line, col);
}

static bool token_contains_position_in_uri(const slang::SourceManager& sm,
                                           const slang::parsing::Token& token,
                                           const std::string& uri, int line, int col) {
    return token && token.location().valid() &&
           contains_position_in_uri(sm, visible_range_for_token(sm, token), uri, line, col);
}

enum class DefinitionTargetKind {
    None,
    Instance,
    NamedPort,
    NamedParameter,
    NamedArgument,
    ClassMember,
    Macro,
    PackageMember,
    Generic,
};

struct DefinitionTarget {
    DefinitionTargetKind kind{DefinitionTargetKind::None};
    std::string name;
    std::string module_name;
    std::string subroutine_name;
    std::string object_name;
    // Whole dotted receiver written before `name`, outermost first.
    //
    //     csr_addr.csr_decode.priv_lvl   cursor on priv_lvl
    //     -> {"csr_addr", "csr_decode"}
    //
    // `object_name` is the innermost element and stays the answer for the common
    // one-hop case.  The rest is what a chain deeper than one dot needs: the
    // innermost element alone is a field name, not something declared in the
    // enclosing module, so resolution has to start from the outermost receiver
    // and follow each field's type in turn.
    std::vector<std::string> object_path;
    // Left-hand side of a `pkg::name` qualification, verbatim.  The visitor only
    // has the syntax tree, so it cannot know whether this really names a
    // package; definition_of_state() decides that against the index.
    std::string package_qualifier;
    // Owner of `package_qualifier` when the qualifier is itself scoped, as in
    // `my_item::type_id::create`: "my_item" here, "type_id" above.  A type alias
    // is only identifiable together with the class that declares it -- in a UVM
    // project every class declares its own `type_id`.
    std::string qualifier_scope;
    std::string scope_module;
    std::string scope_package;
    // Innermost class body the cursor sits in, empty outside class scope.
    // Unqualified names there may be inherited members.
    std::string scope_class;
};

struct DefinitionTargetVisitor : public slang::syntax::SyntaxVisitor<DefinitionTargetVisitor> {
    const slang::SourceManager& sm;
    const std::string& uri;
    int line;
    int col;
    DefinitionTarget target;
    // A macro body token whose expansion covers the cursor.  Held aside rather
    // than accepted outright: the same expansion also covers the arguments the
    // user typed, and an identifier there is the better answer.  Body tokens are
    // visited first, so the walk has to continue past them to find out.
    DefinitionTarget macro_target;
    std::string current_module;
    std::string current_package;
    std::string current_class;

    DefinitionTargetVisitor(const slang::SourceManager& sm, const std::string& uri, int line,
                            int col)
        : sm(sm), uri(uri), line(line), col(col) {}

    bool found() const { return target.kind != DefinitionTargetKind::None; }

    /// Whether @p token may claim the cursor as a definite target.
    ///
    /// visitToken() already defers macro-body tokens: their expansion range is
    /// the whole invocation, which also covers the arguments the user typed, so
    /// an identifier written in those arguments is the better answer and the
    /// walk has to continue past the body token to find it.  The typed node
    /// handlers need the same rule.  Without it the type name in a macro body
    /// such as `` `define COPY(ARG) function void f(peer_t rhs); ARG = rhs.ARG;
    /// endfunction `` claims a cursor sitting on `cfg` in `` `COPY(cfg) `` and
    /// resolves to peer_t instead of the user's own declaration.
    bool token_claims_cursor(const slang::parsing::Token& token) const {
        if (!token_contains_position_in_uri(sm, token, uri, line, col))
            return false;
        return !(sm.isMacroLoc(token.location()) && !sm.isMacroArgLoc(token.location()));
    }

    /// Whole dotted receiver written before @p token, outermost first.
    ///
    /// Scanning back only one word stops at the second dot of `a.b.c`, which is
    /// why a chain deeper than one hop used to resolve to nothing.
    std::vector<std::string> receiver_chain_before_member_dot(
        const slang::parsing::Token& token) const {
        if (!token || !token.location().valid())
            return {};
        // Ask the question of the text the user actually typed; see the note in
        // object_before_member_dot() about macro arguments.
        auto loc = token.location();
        if (sm.isMacroArgLoc(loc))
            loc = sm.getFullyOriginalLoc(loc);
        if (!loc.valid())
            return {};
        const auto source = sm.getSourceText(loc.buffer());
        size_t i = loc.offset();
        if (i > source.size())
            return {};

        // Bounded: a receiver deeper than this is not something the compact
        // index can follow anyway, and the cap keeps the scan off the hot path.
        constexpr size_t kMaxReceiverChain = 8;
        std::vector<std::string> chain;
        while (chain.size() < kMaxReceiverChain) {
            while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
                --i;
            if (i == 0 || source[i - 1] != '.')
                break;
            --i;
            while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
                --i;
            const size_t end = i;
            while (i > 0 && syntax_fragment_edge_is_wordlike(source[i - 1]))
                --i;
            if (i == end)
                break;
            chain.emplace_back(source.substr(i, end - i));
        }
        std::reverse(chain.begin(), chain.end());
        return chain;
    }

    std::string object_before_member_dot(const slang::parsing::Token& token) const {
        if (!token || !token.location().valid())
            return {};
        // Ask the question of the text the user actually typed.  A macro
        // argument is expanded into the macro's body, so scanning backwards from
        // the expansion buffer sees whatever the body put in front of it: in
        // `` `define COPY(ARG) ARG = rhs.ARG; ``, the second ARG is preceded by
        // "rhs." there, and `` `COPY(cfg) `` would be read as `rhs.cfg` even
        // though the user wrote a bare `cfg`.  At the call site the same token is
        // preceded by "(", so no member access is inferred.
        auto loc = token.location();
        if (sm.isMacroArgLoc(loc))
            loc = sm.getFullyOriginalLoc(loc);
        if (!loc.valid())
            return {};
        const auto source = sm.getSourceText(loc.buffer());
        size_t i = loc.offset();
        if (i > source.size())
            return {};
        while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
            --i;
        if (i == 0 || source[i - 1] != '.')
            return {};
        --i;
        while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
            --i;
        const size_t end = i;
        while (i > 0 && syntax_fragment_edge_is_wordlike(source[i - 1]))
            --i;
        if (i == end)
            return {};
        return std::string(source.substr(i, end - i));
    }

    void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
        if (token_claims_cursor(node.header->name)) {
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(node.header->name.valueText());
            target.scope_module = current_module;
            target.scope_package = current_package;
            return;
        }

        auto previous_module = current_module;
        auto previous_package = current_package;
        const bool is_package = node.kind == slang::syntax::SyntaxKind::PackageDeclaration;
        current_module = std::string(node.header->name.valueText());
        if (is_package)
            current_package = current_module;
        visitDefault(node);
        current_module = std::move(previous_module);
        current_package = std::move(previous_package);
    }

    // Tracked so an unqualified name inside a class body can be resolved
    // against that class and the classes it extends.
    void handle(const slang::syntax::ClassDeclarationSyntax& node) {
        auto previous_class = current_class;
        current_class = std::string(node.name.valueText());
        visitDefault(node);
        current_class = std::move(previous_class);
    }

    void handle(const slang::syntax::HierarchyInstantiationSyntax& node) {
        if (token_claims_cursor(node.type)) {
            target.kind = DefinitionTargetKind::Instance;
            target.name = std::string(node.type.valueText());
            target.module_name = target.name;
            target.scope_module = current_module;
            target.scope_package = current_package;
            return;
        }

        const std::string module_name(node.type.valueText());
        if (node.parameters) {
            for (const auto* parameter : node.parameters->parameters) {
                const auto* named =
                    parameter ? parameter->as_if<slang::syntax::NamedParamAssignmentSyntax>()
                              : nullptr;
                if (!named)
                    continue;
                if (token_claims_cursor(named->name)) {
                    target.kind = DefinitionTargetKind::NamedParameter;
                    target.name = std::string(named->name.valueText());
                    target.module_name = module_name;
                    target.scope_module = current_module;
                    target.scope_package = current_package;
                    return;
                }
            }
        }

        for (const auto* instance : node.instances) {
            if (!instance)
                continue;
            if (instance->decl &&
                token_claims_cursor(instance->decl->name)) {
                target.kind = DefinitionTargetKind::Instance;
                target.name = std::string(instance->decl->name.valueText());
                target.module_name = module_name;
                target.scope_module = current_module;
                target.scope_package = current_package;
                return;
            }

            for (const auto* connection : instance->connections) {
                if (!connection)
                    continue;
                const auto* named = connection->as_if<slang::syntax::NamedPortConnectionSyntax>();
                if (named && token_claims_cursor(named->name)) {
                    target.kind = DefinitionTargetKind::NamedPort;
                    target.name = std::string(named->name.valueText());
                    target.module_name = module_name;
                    target.scope_module = current_module;
                    target.scope_package = current_package;
                    return;
                }
            }
        }
        visitDefault(node);
    }

    void handle(const slang::syntax::InvocationExpressionSyntax& node) {
        const auto* callee = node.left->as_if<slang::syntax::IdentifierNameSyntax>();
        if (!callee || !node.arguments) {
            visitDefault(node);
            return;
        }

        const std::string subroutine_name(callee->identifier.valueText());
        for (const auto* argument : node.arguments->parameters) {
            if (!argument)
                continue;
            const auto* named = argument->as_if<slang::syntax::NamedArgumentSyntax>();
            if (!named)
                continue;
            if (token_claims_cursor(named->name)) {
                target.kind = DefinitionTargetKind::NamedArgument;
                target.name = std::string(named->name.valueText());
                target.subroutine_name = subroutine_name;
                target.scope_module = current_module;
                target.scope_package = current_package;
                return;
            }
        }

        visitDefault(node);
    }

    // `pkg::WIDTH`, `pkg::byte_t`, `pkg::my_class`.
    //
    // Without this handler the qualifier is lost: NamedTypeSyntax's as_if to
    // IdentifierNameSyntax fails for a scoped name, so the cursor falls through
    // to visitToken() and is reported as a bare Generic identifier, which then
    // happily resolves to a same-named local declaration.
    //
    // Only the right-hand identifier becomes a PackageMember.  A cursor on the
    // left half is a reference to the package itself and already resolves, so it
    // must keep going through the default walk.
    void handle(const slang::syntax::ScopedNameSyntax& node) {
        // slang uses ScopedNameSyntax for both `pkg::name` and dotted
        // hierarchical/member names such as `fifo_entry.valid`.  Only the `::`
        // form is a package qualification; the dotted form belongs to the
        // member-access path and must not be intercepted here.
        if (!node.left || !node.right ||
            node.separator.kind != slang::parsing::TokenKind::DoubleColon) {
            // …with one exception: a dotted name whose receiver carries a
            // select (`g_lane[0].acc`) parses as ScopedNameSyntax, not as a
            // MemberAccessExpressionSyntax, so nothing below records the
            // receiver.  The cursor then reached visitToken() as a bare
            // identifier and resolved to whatever same-named signal the
            // enclosing module happened to declare — a different object.
            //
            // Deliberately narrow: only `receiver[i].member`, the shape the
            // deeper hierarchical walk cannot start from.  Longer paths
            // (`u_dut.u_leaf.sig`) already resolve through the instance and
            // hierarchy machinery, and claiming them here would take that away.
            if (node.left && node.right &&
                node.separator.kind == slang::parsing::TokenKind::Dot &&
                node.left->kind == slang::syntax::SyntaxKind::IdentifierSelectName) {
                if (const auto* right =
                        node.right->as_if<slang::syntax::IdentifierNameSyntax>()) {
                    if (token_claims_cursor(right->identifier)) {
                        std::vector<std::string> chain;
                        collect_dotted_name_chain(node.left, chain);
                        if (chain.size() == 1) {
                            target.kind = DefinitionTargetKind::ClassMember;
                            target.name = std::string(right->identifier.valueText());
                            target.object_path = std::move(chain);
                            target.object_name = target.object_path.back();
                            target.scope_module = current_module;
                            target.scope_package = current_package;
                            return;
                        }
                    }
                }
            }
            visitDefault(node);
            return;
        }

        // The qualifier is usually a bare IdentifierNameSyntax (`pkg::name`), but
        // a parameterized class qualifier (`uvm_config_db #(T)::get`) parses as
        // ClassNameSyntax instead — same identifier, plus a parameter list this
        // lookup doesn't need. Accept either so a parameterized-class static
        // method call isn't left to fall through to an unscoped bare lookup.
        const auto* right = node.right->as_if<slang::syntax::IdentifierNameSyntax>();
        slang::parsing::Token left_identifier;
        std::string left_owner;
        if (const auto* left_id = node.left->as_if<slang::syntax::IdentifierNameSyntax>())
            left_identifier = left_id->identifier;
        else if (const auto* left_class = node.left->as_if<slang::syntax::ClassNameSyntax>())
            left_identifier = left_class->identifier;
        else if (const auto* left_scoped = node.left->as_if<slang::syntax::ScopedNameSyntax>()) {
            // Two hops: `my_item::type_id::create`.  The qualifier that matters
            // for `create` is `type_id`, but that alias is only identifiable
            // together with the class that declares it, so carry `my_item` too.
            //
            // Without this the whole expression fell through to the unqualified
            // walk, which resolves `create` against the enclosing class -- and in
            // UVM that class always has one, generated by the same factory macro
            // that declares `type_id`.
            if (left_scoped->separator.kind == slang::parsing::TokenKind::DoubleColon &&
                left_scoped->left && left_scoped->right) {
                if (const auto* alias =
                        left_scoped->right->as_if<slang::syntax::IdentifierNameSyntax>())
                    left_identifier = alias->identifier;
                if (const auto* owner =
                        left_scoped->left->as_if<slang::syntax::IdentifierNameSyntax>())
                    left_owner = std::string(owner->identifier.valueText());
                else if (const auto* owner_class =
                             left_scoped->left->as_if<slang::syntax::ClassNameSyntax>())
                    left_owner = std::string(owner_class->identifier.valueText());
            }
        }

        if (left_identifier && right &&
            token_claims_cursor(right->identifier)) {
            target.kind = DefinitionTargetKind::PackageMember;
            target.name = std::string(right->identifier.valueText());
            target.package_qualifier = std::string(left_identifier.valueText());
            target.qualifier_scope = std::move(left_owner);
            target.scope_module = current_module;
            target.scope_package = current_package;
            return;
        }

        visitDefault(node);
    }

    // `import pkg::NAME;` — a PackageImportItemSyntax carries bare tokens rather
    // than a ScopedNameSyntax, so it needs its own handler.  This is the case
    // where a package-qualified name would otherwise resolve to a local
    // declaration of the same name.
    void handle(const slang::syntax::PackageImportItemSyntax& node) {
        if (token_claims_cursor(node.item)) {
            target.kind = DefinitionTargetKind::PackageMember;
            target.name = std::string(node.item.valueText());
            target.package_qualifier = std::string(node.package.valueText());
            target.scope_module = current_module;
            target.scope_package = current_package;
            return;
        }
        visitDefault(node);
    }

    /// Receiver identifiers of a member-access expression, outermost first.
    ///
    /// An element select is transparent here: every copy of a generate loop
    /// shares one declaration, so `gen[0].u.sig` resolves like `gen.u.sig`.
    static void collect_receiver_chain(const slang::syntax::ExpressionSyntax* expr,
                                       std::vector<std::string>& out) {
        if (!expr || out.size() > 8)
            return;
        if (const auto* ident = expr->as_if<slang::syntax::IdentifierNameSyntax>()) {
            out.emplace_back(ident->identifier.valueText());
            return;
        }
        if (const auto* member = expr->as_if<slang::syntax::MemberAccessExpressionSyntax>()) {
            collect_receiver_chain(member->left, out);
            out.emplace_back(member->name.valueText());
            return;
        }
        if (const auto* select = expr->as_if<slang::syntax::ElementSelectExpressionSyntax>())
            collect_receiver_chain(select->left, out);
    }

    /// Receiver names of a dotted *name* (as opposed to a member-access
    /// expression): `g_lane[0].u_lane` -> {"g_lane", "u_lane"}.  Selects are
    /// dropped: every copy of a generate loop shares one declaration, so the
    /// index only says which block, never which iteration.
    static void collect_dotted_name_chain(const slang::syntax::NameSyntax* name,
                                          std::vector<std::string>& out) {
        if (!name)
            return;
        if (const auto* ident = name->as_if<slang::syntax::IdentifierNameSyntax>()) {
            out.emplace_back(ident->identifier.valueText());
            return;
        }
        if (const auto* select = name->as_if<slang::syntax::IdentifierSelectNameSyntax>()) {
            out.emplace_back(select->identifier.valueText());
            return;
        }
        if (const auto* scoped = name->as_if<slang::syntax::ScopedNameSyntax>()) {
            if (scoped->separator.kind != slang::parsing::TokenKind::Dot)
                return;
            collect_dotted_name_chain(scoped->left, out);
            collect_dotted_name_chain(scoped->right, out);
        }
    }

    void handle(const slang::syntax::MemberAccessExpressionSyntax& node) {
        if (token_claims_cursor(node.name)) {
            target.kind = DefinitionTargetKind::ClassMember;
            target.name = std::string(node.name.valueText());
            collect_receiver_chain(node.left, target.object_path);
            target.object_name =
                target.object_path.empty() ? std::string{} : target.object_path.back();
            target.scope_module = current_module;
            target.scope_package = current_package;
            return;
        }
        visitDefault(node);
    }

    void handle(const slang::syntax::NamedTypeSyntax& node) {
        const auto* identifier = node.name->as_if<slang::syntax::IdentifierNameSyntax>();
        if (identifier &&
            token_claims_cursor(identifier->identifier)) {
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(identifier->identifier.valueText());
            target.scope_module = current_module;
            target.scope_package = current_package;
            return;
        }
        visitDefault(node);
    }

    void visitToken(slang::parsing::Token token) {
        if (found() || !token || !token.location().valid())
            return;

        if (sm.isMacroLoc(token.location()) && !sm.isMacroArgLoc(token.location())) {
            if (macro_target.kind != DefinitionTargetKind::None ||
                !contains_position_in_uri(sm, sm.getExpansionRange(token.location()), uri, line,
                                          col))
                return;

            auto macro_name = sm.getMacroName(token.location());
            if (!macro_name.empty()) {
                macro_target.kind = DefinitionTargetKind::Macro;
                macro_target.name = std::string(macro_name);
                macro_target.scope_module = current_module;
                macro_target.scope_package = current_package;
            }
            return;
        }

        // `new` is lexed as a keyword, not an identifier, so `super.new(...)`
        // never reached the member-access branch and resolved to nothing --
        // while `super.arm(...)` right next to it worked.  The constructor is
        // indexed like any other method; only this classification was missing.
        const bool is_member_name_token =
            token.kind == slang::parsing::TokenKind::Identifier ||
            token.kind == slang::parsing::TokenKind::NewKeyword;
        if (is_member_name_token &&
            token_contains_position_in_uri(sm, token, uri, line, col)) {
            auto object_path = receiver_chain_before_member_dot(token);
            if (!object_path.empty()) {
                target.kind = DefinitionTargetKind::ClassMember;
                target.name = std::string(token.valueText());
                target.object_name = object_path.back();
                target.object_path = std::move(object_path);
                target.scope_module = current_module;
                target.scope_package = current_package;
                // The receiver of `obj.member` inside a class body is looked up
                // in that class, so the enclosing class has to travel with the
                // target just as it does for unqualified names.
                target.scope_class = current_class;
                return;
            }
            // A bare `new` is an object creation, not a name the user can be
            // asking about; only `receiver.new` is a member reference.
            if (token.kind == slang::parsing::TokenKind::NewKeyword)
                return;
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(token.valueText());
            target.scope_module = current_module;
            target.scope_package = current_package;
            target.scope_class = current_class;
        }
    }
};

static DefinitionTarget definition_target_at(const slang::syntax::SyntaxTree& tree,
                                             const std::string& uri, int line, int col) {
    DefinitionTargetVisitor visitor(tree.sourceManager(), uri, line, col);
    tree.root().visit(visitor);
    return visitor.found() ? visitor.target : visitor.macro_target;
}

static bool index_entry_location_matches(const SyntaxIndex& idx, SourceFileID file_id,
                                         int line_one_based, int col_zero_based,
                                         const Location& definition) {
    if (line_one_based <= 0)
        return false;

    // `Location` is LSP-shaped (0-based line) while SyntaxIndex entries keep
    // 1-based source lines.  Match both the normalized URI and token column so
    // same-spelled symbols in a closed file do not borrow wrong hover metadata.
    // If a shard cannot map the SourceFileID back to a URI, keep the comparison
    // permissive for legacy/in-memory shards and rely on line/column.
    const auto actual_uri = idx.source_uri(file_id);
    if (!actual_uri.empty() && actual_uri != definition.uri)
        return false;
    return to_lsp_line(line_one_based) == definition.line && col_zero_based == definition.col;
}

static SymbolInfo symbol_info_for_value_entry(const ValueEntry& value, const std::string& name,
                                             const Location& definition) {
    std::string doc;
    if ((value.kind == "function" || value.kind == "task") && !value.signature.empty())
        doc = value.signature;

    const bool is_subroutine = value.kind == "function" || value.kind == "task";
    std::string detail = is_subroutine ? value.kind : value.type;
    // Parameters render as `int = 8`, matching how PortEntry parameters are
    // already rendered in symbol_info_from_index().
    if (!is_subroutine && !value.default_value.empty() &&
        (value.kind == "parameter" || value.kind == "localparam"))
        detail += " = " + value.default_value;

    return SymbolInfo{.name = name,
                      .kind = value.kind.empty() ? "variable" : value.kind,
                      .detail = std::move(detail),
                      .doc = std::move(doc),
                      .line = definition.line,
                      .col = definition.col};
}

static SymbolInfo symbol_info_for_typedef_entry(const TypedefEntry& td, const std::string& name,
                                               const Location& definition) {
    std::string detail;
    if (td.is_enum) {
        detail = "enum " + td.resolved;
        if (!td.enum_members.empty()) {
            detail += " {";
            for (size_t i = 0; i < td.enum_members.size(); ++i) {
                if (i)
                    detail += ", ";
                detail += td.enum_members[i].name;
            }
            detail += "}";
        }
    } else if (td.is_struct) {
        // SyntaxIndex currently stores both struct and union typedef members in
        // the same compact shape.  Keep the historical display string here; the
        // important closed-file hover behavior is to surface fields instead of
        // falling back to the bare "symbol" kind.
        detail = "struct packed";
        if (!td.fields.empty()) {
            detail += " {";
            for (const auto& f : td.fields)
                detail += " " + f.type + " " + f.name + ";";
            detail += " }";
        }
    } else {
        detail = td.resolved;
    }

    return SymbolInfo{.name = name,
                      .kind = "typedef",
                      .detail = detail,
                      .line = definition.line,
                      .col = definition.col};
}

static std::optional<SymbolInfo> symbol_info_from_index(const SyntaxIndex& idx,
                                                        const DefinitionTarget& target,
                                                        const Location& definition) {
    // A member spelled once per owner cannot be identified by location alone
    // when every owner's copy expands from the same macro body: UVM's factory
    // macros give each registered class its own `type_id`, and all of them
    // report the macro's own line.  The location scan below would hand back
    // whichever class was indexed first.  When the cursor named the owner
    // (`lv_full_item::type_id`), resolve under that owner instead.
    if (!target.package_qualifier.empty() && !target.name.empty()) {
        const auto key = package_scoped_key(target.package_qualifier, target.name);
        if (auto it = idx.package_type_by_scoped_name.find(key);
            it != idx.package_type_by_scoped_name.end() && it->second < idx.typedefs.size()) {
            const auto& td = idx.typedefs[it->second];
            return symbol_info_for_typedef_entry(td, td.name, definition);
        }
    }

    // Otherwise try an exact definition-location lookup.  This is the most
    // robust path for closed project files because the clicked token may be
    // classified only as a generic identifier, while `definition_of_state()`
    // has already resolved the exact declaration in a SyntaxIndex shard.
    for (const auto& module : idx.modules) {
        if (index_entry_location_matches(idx, module.file_id, module.line, module.col, definition))
            return SymbolInfo{.name = module.name,
                              .kind = "module",
                              .detail = "module",
                              .doc = module_doc_from_entry(module),
                              .line = definition.line,
                              .col = definition.col};
        for (const auto& port : module.ports) {
            if (!index_entry_location_matches(idx, port.file_id, port.line, port.col, definition))
                continue;
            const bool is_parameter =
                port.direction == "parameter" || port.direction == "localparam";
            std::string detail =
                is_parameter ? port.type : (port.decl_type.empty() ? port.type : port.decl_type);
            if (is_parameter && !port.default_value.empty())
                detail += " = " + port.default_value;
            return SymbolInfo{.name = port.name,
                              .kind = is_parameter ? "parameter" : "port",
                              .detail = std::move(detail),
                              .line = definition.line,
                              .col = definition.col};
        }
    }
    for (const auto& value : idx.values) {
        if (index_entry_location_matches(idx, value.file_id, value.line, value.col, definition))
            return symbol_info_for_value_entry(value, value.name, definition);
    }
    for (const auto& td : idx.typedefs) {
        if (index_entry_location_matches(idx, td.file_id, td.line, td.col, definition))
            return symbol_info_for_typedef_entry(td, td.name, definition);
        for (const auto& field : td.fields) {
            // A struct or union member is a field, not a free-standing variable.
            if (index_entry_location_matches(idx, field.file_id, field.line, field.col, definition))
                return SymbolInfo{.name = field.name,
                                  .kind = "field",
                                  .detail = field.type,
                                  .line = definition.line,
                                  .col = definition.col};
        }
        for (const auto& member : td.enum_members) {
            // Naming the enum it belongs to is what makes this hover useful;
            // "enum member" repeated the kind label and said nothing else.
            if (index_entry_location_matches(idx, member.file_id, member.line, member.col, definition))
                return SymbolInfo{.name = member.name,
                                  .kind = "enum_member",
                                  .detail = td.name.empty() ? std::string("enum member") : td.name,
                                  .line = definition.line,
                                  .col = definition.col};
        }
    }
    for (const auto& cls : idx.classes) {
        if (index_entry_location_matches(idx, cls.file_id, cls.line, cls.col, definition))
            return SymbolInfo{.name = cls.name,
                              .kind = "class",
                              .detail = "class",
                              .line = definition.line,
                              .col = definition.col};
        for (const auto& field : cls.fields) {
            if (index_entry_location_matches(idx, field.file_id, field.line, field.col, definition))
                return SymbolInfo{.name = field.name,
                                  .kind = "variable",
                                  .detail = field.type,
                                  .line = definition.line,
                                  .col = definition.col};
        }
        for (const auto& method : cls.methods) {
            if (index_entry_location_matches(idx, method.file_id, method.line, method.col, definition))
                return SymbolInfo{.name = method.name,
                                  .kind = method.is_task ? "task" : "function",
                                  .detail = method.is_task ? "task" : "function",
                                  .doc = method.is_task
                                             ? ("```\ntask " + method.name + "()\n```")
                                             : ("```\nfunction " + method.return_type + " " +
                                                method.name + "()\n```"),
                                  .line = definition.line,
                                  .col = definition.col};
        }
    }

    if (target.kind == DefinitionTargetKind::Instance) {
        auto it = idx.module_by_name.find(target.module_name);
        if (it == idx.module_by_name.end())
            return std::nullopt;
        return SymbolInfo{.name = target.module_name,
                          .kind = "module",
                          .detail = "module",
                          .doc = module_doc_from_entry(idx.modules[it->second]),
                          .line = definition.line,
                          .col = definition.col};
    }
    if (target.kind == DefinitionTargetKind::NamedPort) {
        auto mit = idx.module_by_name.find(target.module_name);
        if (mit == idx.module_by_name.end())
            return std::nullopt;
        const auto& mod = idx.modules[mit->second];
        auto pit = mod.port_by_name.find(target.name);
        if (pit == mod.port_by_name.end())
            return std::nullopt;
        const auto& port = mod.ports[pit->second];
        return SymbolInfo{.name = target.name,
                          .kind = "port",
                          .detail = port.decl_type.empty() ? port.type : port.decl_type,
                          .line = definition.line,
                          .col = definition.col};
    }
    if (target.kind == DefinitionTargetKind::NamedParameter) {
        auto mit = idx.module_by_name.find(target.module_name);
        if (mit == idx.module_by_name.end())
            return std::nullopt;
        const auto& mod = idx.modules[mit->second];
        auto pit = mod.port_by_name.find(target.name);
        if (pit == mod.port_by_name.end())
            return std::nullopt;
        const auto& port = mod.ports[pit->second];
        std::string detail = port.type;
        if (!port.default_value.empty())
            detail += " = " + port.default_value;
        return SymbolInfo{.name = target.name,
                          .kind = "parameter",
                          .detail = detail,
                          .line = definition.line,
                          .col = definition.col};
    }
    if (target.kind == DefinitionTargetKind::ClassMember) {
        for (const auto& cls : idx.classes) {
            for (const auto& f : cls.fields) {
                if (f.name == target.name)
                    return SymbolInfo{.name = target.name,
                                      .kind = "variable",
                                      .detail = f.type,
                                      .line = definition.line,
                                      .col = definition.col};
            }
            for (const auto& m : cls.methods) {
                if (m.name == target.name)
                    return SymbolInfo{
                        .name = target.name,
                        .kind = m.is_task ? "task" : "function",
                        .detail = m.is_task ? "task" : "function",
                        .doc = m.is_task
                                   ? ("```\ntask " + m.name + "()\n```")
                                   : ("```\nfunction " + m.return_type + " " + m.name + "()\n```"),
                        .line = definition.line,
                        .col = definition.col};
            }
        }
        return std::nullopt;
    }
    if (target.kind == DefinitionTargetKind::Generic) {
        // function/task first
        for (const auto& v : idx.values) {
            if (v.name != target.name)
                continue;
            if (v.kind != "function" && v.kind != "task")
                continue;
            std::string doc;
            if (!v.signature.empty())
                doc = v.signature;
            return SymbolInfo{.name = target.name,
                              .kind = v.kind,
                              .detail = v.kind,
                              .doc = std::move(doc),
                              .line = definition.line,
                              .col = definition.col};
        }
        // typedef/enum/struct
        auto tit = idx.typedef_by_name.find(target.name);
        if (tit != idx.typedef_by_name.end()) {
            const auto& td = idx.typedefs[tit->second];
            std::string detail;
            if (td.is_enum) {
                detail = "enum " + td.resolved;
                if (!td.enum_members.empty()) {
                    detail += " {";
                    for (size_t i = 0; i < td.enum_members.size(); ++i) {
                        if (i)
                            detail += ", ";
                        detail += td.enum_members[i].name;
                    }
                    detail += "}";
                }
            } else if (td.is_struct) {
                detail = "struct packed";
                if (!td.fields.empty()) {
                    detail += " {";
                    for (const auto& f : td.fields) {
                        detail += " " + f.type + " " + f.name + ";";
                    }
                    detail += " }";
                }
            } else {
                detail = td.resolved;
            }
            return SymbolInfo{.name = target.name,
                              .kind = "typedef",
                              .detail = detail,
                              .line = definition.line,
                              .col = definition.col};
        }
        // variable/net/parameter/localparam
        for (const auto& v : idx.values) {
            if (v.name == target.name) {
                return SymbolInfo{.name = target.name,
                                  .kind = v.kind.empty() ? "variable" : v.kind,
                                  .detail = v.type,
                                  .line = definition.line,
                                  .col = definition.col};
            }
        }
    }
    return std::nullopt;
}

static std::optional<Location> include_target_at(const DocumentState& state, int line);

/// Type text of a declaration read back from its own source line.
///
/// A closed file's shard deliberately does not render the type of declarations
/// inside a generate block — doing that for every such declaration in a project
/// is the entire cost of indexing them, and they are indexed so a hierarchical
/// path can *find* them.  Hover needs the type for exactly one declaration, so
/// it reads the one line that holds it.
static std::string declaration_type_from_source_line(const std::string& path, int line0, int col0,
                                                     std::string_view name) {
    if (path.empty() || line0 < 0 || col0 <= 0)
        return {};
    std::ifstream input(path);
    if (!input)
        return {};
    std::string text;
    for (int i = 0; i <= line0; ++i) {
        if (!std::getline(input, text))
            return {};
    }
    if ((size_t)col0 > text.size())
        return {};
    // Confirm the name really starts here before trusting the prefix: a stale
    // shard would otherwise turn arbitrary source text into a "type".
    if (text.compare(col0, name.size(), name) != 0)
        return {};

    std::string prefix = trim_copy(text.substr(0, col0));
    // `logic [7:0] a, b;` — hovering `b` leaves `logic [7:0] a,` as the prefix.
    // Drop the earlier declarators so the type alone remains.
    while (!prefix.empty() && prefix.back() == ',') {
        prefix.pop_back();
        while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back())))
            prefix.pop_back();
        while (!prefix.empty() && prefix.back() == ']') {
            const auto open = prefix.rfind('[');
            if (open == std::string::npos)
                return {};
            prefix.erase(open);
            while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back())))
                prefix.pop_back();
        }
        while (!prefix.empty() && syntax_fragment_edge_is_wordlike(prefix.back()))
            prefix.pop_back();
        prefix = trim_copy(std::move(prefix));
    }
    return prefix;
}

std::optional<SymbolInfo> Analyzer::symbol_at(const std::string& uri, int line, int col) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    auto ident = extract_ident(state->text, line, col);
    if (ident.empty())
        return std::nullopt;

    auto target = definition_target_at(*state->tree, uri, line, col);
    auto extra_files = extra_file_snapshot_ptr();

    if (target.kind == DefinitionTargetKind::Macro) {
        if (auto info = find_macro_info(*state->tree, uri, target.name))
            return info;
        for (const auto& extra : *extra_files) {
            if (extra.uri == uri)
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto info = find_macro_info(*extra.state->tree, extra.uri, target.name))
                return info;
        }
        return SymbolInfo{
            .name = target.name, .kind = "macro", .detail = "(empty)", .line = line, .col = col};
    }

    // Reuse the extra-file snapshot already collected for hover.  Calling
    // definition_of() here would collect the same snapshot again, and for open
    // filelist entries that can mean repeating live-buffer index work during a
    // single user-visible hover request.
    auto definition = definition_of_state(*state, uri, line, col, *extra_files, &uri);
    // Miss path only, mirroring definition_of(): a hierarchical path such as
    // `u_dut.g_lane[0].acc` is resolved by the walk below, not by the scope
    // lookups above, and without this hover stays empty on a token that
    // go-to-definition answers.
    if (!definition)
        definition = hierarchical_definition(*state, uri, line, col);
    if (definition) {
        std::string name = target.name.empty() ? ident : target.name;

        // The current document's SyntaxTree may contain tokens whose actual
        // source URI is an included file, for example:
        //
        //     // memory_top.sv
        //     `include "params.svh"   // defines typedef enum ... state_t;
        //     state_t state;
        //
        // `definition_of_state()` correctly reports the typedef location as
        // params.svh, but that declaration is still inside this live SyntaxTree.
        // Restricting the rich AST hover path to `definition->uri == uri` made
        // included-file typedefs, parameters, variables, and class members fall
        // through to the bare "symbol" fallback whenever the include file was
        // not also listed as an extra/project file.  Always try the current tree
        // first; token_at_location() matches exact URI/line/column, so this does
        // not steal metadata for definitions that only exist in a different
        // shard.
        if (auto info = symbol_info_from_definition(*state->tree, uri, name, *definition, nullptr,
                                                    target.package_qualifier))
            return info;

        if (definition->uri != uri) {
            for (const auto& extra : *extra_files) {
                if (extra.uri != definition->uri || !extra.state || !extra.state->tree)
                    continue;
                if (auto info = symbol_info_from_definition(*extra.state->tree, extra.uri, name,
                                                            *definition, &extra.index_ref()))
                    return info;
            }
        }

        for (const auto& extra : *extra_files) {
            if (extra.uri != definition->uri)
                continue;
            if (auto info = symbol_info_from_index(extra.index_ref(), target, *definition)) {
                // A generate-block declaration is indexed without its type, so
                // recover it from the declaration's own line — one line read,
                // and only when hover would otherwise show a bare name.
                if (info->detail.empty())
                    info->detail = declaration_type_from_source_line(
                        extra.path, definition->line, definition->col, info->name);
                return info;
            }
            break;
        }

        // A project file's shard also indexes everything it `include`s, and the
        // included header is usually not itself a filelist entry -- UVM lists
        // uvm_pkg.sv, which includes every uvm_*.svh.  Matching shards by URI
        // alone therefore finds nothing for such declarations and hover degrades
        // to a bare name.  Fall back to scanning the shards; the entries carry
        // their own file_id, so the location match stays exact.
        for (const auto& extra : *extra_files) {
            if (extra.uri == definition->uri)
                continue; // already tried above
            if (auto info = symbol_info_from_index(extra.index_ref(), target, *definition))
                return info;
        }
        return SymbolInfo{
            .name = name, .kind = "symbol", .line = definition->line, .col = definition->col};
    }

    // `` `include "path" `` — the cursor sits inside a string literal, so
    // nothing above resolved a symbol.  Report the file the directive actually
    // pulled in, using the same already-resolved relation goto-definition uses.
    if (auto included = include_target_at(*state, line)) {
        const std::filesystem::path path = path_from_file_uri(included->uri);
        return SymbolInfo{.name = path.filename().string(),
                          .kind = "include",
                          .detail = path.string(),
                          .line = line,
                          .col = col};
    }

    return SymbolInfo{.name = ident, .kind = "unknown", .line = line, .col = col};
}

std::optional<IdentifierAtPosition> Analyzer::identifier_at(const std::string& uri, int line,
                                                            int col) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        int line;
        int col;
        std::optional<IdentifierAtPosition> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, int line, int col)
            : sm(sm), uri(uri), line(line), col(col) {}

        void visitToken(slang::parsing::Token token) {
            if (result || !token || token.kind != slang::parsing::TokenKind::Identifier)
                return;
            // A macro body token is preprocessor output with no position of its
            // own in the file, so the cursor can never really be on it.  Its
            // visible range is the whole invocation, which would otherwise make
            // the first identifier the body happens to contain answer for every
            // column of the call -- naming that instead of what the user wrote.
            // Arguments are typed at the call site and are hit normally.
            if (sm.isMacroLoc(token.location()) && !sm.isMacroArgLoc(token.location()))
                return;
            if (!token_contains_position_in_uri(sm, token, uri, line, col))
                return;

            const auto start = visible_range_for_token(sm, token).start();
            const int token_line = to_lsp_line((int)sm.getLineNumber(start));
            const int token_col = utf16_column(sm, start);
            const std::string name(token.valueText());
            result = IdentifierAtPosition{
                .name = name,
                .line = token_line,
                .col = token_col,
                .end_col = token_col + (int)utf16_length(name),
            };
        }
    };

    Visitor visitor(state->tree->sourceManager(), uri, line, col);
    state->tree->root().visit(visitor);
    if (visitor.result)
        return visitor.result;

    // A macro invocation leaves no Identifier token at the user's position --
    // the preprocessor consumed the backtick and the name.  Read the name back
    // out of the source so `FOO, and a `define of it, stay renameable.
    if (auto ident = extract_ident_span(state->text, line, col);
        ident && (is_backtick_identifier(state->text, line, ident->start_col) ||
                  is_define_identifier(state->text, line, ident->start_col)))
        return IdentifierAtPosition{.name = ident->text,
                                    .line = line,
                                    .col = ident->start_col,
                                    .end_col = ident->end_col};
    return std::nullopt;
}

static std::optional<Location> include_target_at(const DocumentState& state, int line) {
    // The path in `` `include "..." `` is a string literal, so the identifier
    // lookup that drives every other definition target cannot see it.  Nothing
    // needs to be re-resolved though: slang already found the file while
    // parsing, and each included buffer records the source location of the
    // directive that pulled it in.  Mapping the cursor line through that
    // relation avoids re-running an include-path search and touches no
    // filesystem.
    if (!state.source_manager)
        return std::nullopt;
    const auto& sm = *state.source_manager;

    // Anchor on this document's own buffer.  Other open buffers are injected
    // into the same SourceManager through assignText(), so without this check
    // an overlay's includes would be attributed to this file.
    const std::string normalized_uri = uri_from_path(state.normalized_path);
    slang::BufferID owning_buffer;
    for (auto buffer : sm.getAllBuffers()) {
        const auto& full_path = sm.getFullPath(buffer);
        if (!full_path.empty() && uri_from_path(full_path) == normalized_uri) {
            owning_buffer = buffer;
            break;
        }
    }
    if (!owning_buffer.valid())
        return std::nullopt;

    for (auto buffer : sm.getAllBuffers()) {
        // Only directives written in this file.  A nested include's directive
        // lives in the header that spells it, not here, so comparing the
        // immediate parent is what keeps the cursor line meaningful.
        const auto loc = sm.getIncludedFrom(buffer);
        if (!loc.valid() || loc.buffer() != owning_buffer)
            continue;
        if (static_cast<int>(sm.getLineNumber(loc)) - 1 != line)
            continue;

        const auto& full_path = sm.getFullPath(buffer);
        if (full_path.empty())
            continue;
        return Location{uri_from_path(full_path), 0, 0, 0, 0};
    }
    return std::nullopt;
}

// ── Hierarchical references (tb.u_dut.u_sub.sig) ─────────────────────────────
//
// The index already knows every instance and every module's signals; what was
// missing is the walk between them.  Segments are resolved one at a time
// against published index snapshots — no reparse, no project-wide merge — and
// an unresolved segment ends the walk with no answer rather than a same-named
// guess from an unrelated module.

/// Dotted path the cursor sits in, e.g. {"tb","u_dut","g_lanes","u_sub","stage"}
/// for a cursor anywhere in `tb.u_dut.g_lanes[0].u_sub.stage`.  Array indices are
/// dropped; the clicked segment is the last one returned.
static std::vector<std::string> hierarchical_path_at(const std::string& text, int line, int col,
                                                     const IdentifierAtPosition& ident) {
    size_t line_start = 0;
    for (int i = 0; i < line; ++i) {
        line_start = text.find('\n', line_start);
        if (line_start == std::string::npos)
            return {};
        ++line_start;
    }
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos)
        line_end = text.size();
    const std::string_view src(text.data() + line_start, line_end - line_start);
    (void)col;

    const auto is_word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };

    std::vector<std::string> segments{ident.name};
    size_t i = (size_t)ident.col;
    while (i > 0) {
        size_t j = i;
        while (j > 0 && std::isspace(static_cast<unsigned char>(src[j - 1])))
            --j;
        if (j == 0 || src[j - 1] != '.')
            break;
        --j; // consume the dot
        while (j > 0 && std::isspace(static_cast<unsigned char>(src[j - 1])))
            --j;
        // An optional array index on the preceding segment: g_lanes[0].
        if (j > 0 && src[j - 1] == ']') {
            const size_t close = j - 1;
            size_t open = close;
            while (open > 0 && src[open - 1] != '[')
                --open;
            if (open == 0)
                break;
            j = open - 1;
            while (j > 0 && std::isspace(static_cast<unsigned char>(src[j - 1])))
                --j;
        }
        const size_t word_end = j;
        while (j > 0 && is_word(src[j - 1]))
            --j;
        if (j == word_end)
            break;
        segments.insert(segments.begin(), std::string(src.substr(j, word_end - j)));
        i = j;
    }
    return segments;
}

/// A module declaration found in an index, together with the index that holds it.
struct HierarchyModule {
    const SyntaxIndex* index{nullptr};
    std::string shard_uri;
    const ModuleEntry* entry{nullptr};
};

static std::optional<Location> hierarchy_location(const SyntaxIndex& index,
                                                  const std::string& shard_uri,
                                                  SourceFileID file_id, int line_one_based,
                                                  int col, int name_length) {
    if (line_one_based <= 0)
        return std::nullopt;
    const auto file_uri = index.source_uri(file_id);
    const int lsp_line = to_lsp_line(line_one_based);
    return Location{file_uri.empty() ? shard_uri : file_uri, lsp_line, col, lsp_line,
                    col + name_length};
}

std::optional<Location> Analyzer::hierarchical_definition(const DocumentState& state,
                                                          const std::string& uri, int line,
                                                          int col) const {
    auto ident = identifier_at(uri, line, col);
    if (!ident || ident->name.empty())
        return std::nullopt;
    auto segments = hierarchical_path_at(state.text, line, col, *ident);
    if (segments.size() < 2)
        return std::nullopt;

    // Snapshots this walk resolves against: open buffers first (an open file is
    // authoritative for its own declarations), then the published project index.
    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> open_states;
    for_each_state([&](const std::string& state_uri,
                       const std::shared_ptr<const DocumentState>& doc) {
        if (doc && doc->tree)
            open_states.emplace_back(state_uri, doc);
    });
    auto project = project_index_snapshot();

    const auto find_module = [&](const std::string& name) -> std::optional<HierarchyModule> {
        for (const auto& [state_uri, doc] : open_states) {
            const auto& index = get_structural_index(*doc);
            const auto it = index.module_by_name.find(name);
            if (it != index.module_by_name.end())
                return HierarchyModule{&index, state_uri, &index.modules[it->second]};
        }
        if (project) {
            const auto it = project->module_by_name.find(name);
            if (it != project->module_by_name.end() && it->second.shard) {
                const auto& index = *it->second.shard;
                // The shard's URI is the file it was built from; entries carry
                // their own file_id for `include`d declarations.
                std::string shard_uri;
                for (const auto& shard : project->shards)
                    if (shard.index == it->second.shard) {
                        shard_uri = shard.uri;
                        break;
                    }
                return HierarchyModule{&index, shard_uri, &index.modules[it->second.module_index]};
            }
        }
        return std::nullopt;
    };

    const auto find_instance = [&](const HierarchyModule& owner,
                                   const std::string& name) -> const InstanceEntry* {
        for (const auto& inst : owner.index->instances)
            if (inst.parent_module == owner.entry->name && inst.instance_name == name)
                return &inst;
        return nullptr;
    };

    // First segment: the enclosing module itself, an instance in it, or a module
    // named directly.
    std::optional<HierarchyModule> current;
    size_t next = 1;
    if (auto module = find_module(segments[0])) {
        current = std::move(module);
    } else {
        // `u_dut.sig` written inside the testbench: resolve the instance against
        // whichever module in this document declares it.
        const auto& own_index = get_structural_index(state);
        for (const auto& inst : own_index.instances) {
            if (inst.instance_name != segments[0])
                continue;
            current = find_module(inst.module_name);
            break;
        }
        if (!current && segments.size() > 2) {
            // `g_lanes[0].u_leaf.sig`: the first segment is a generate block
            // label.  A generate block is not a module and not an instance, and
            // instances written inside one are filed under the enclosing module,
            // so the label can be skipped — but only once the next segment is
            // confirmed to be an instance, so an unknown name cannot silently
            // disappear.  Same rule the middle of the walk already applies.
            for (const auto& inst : own_index.instances) {
                if (inst.instance_name != segments[1])
                    continue;
                current = find_module(inst.module_name);
                next = 2;
                break;
            }
        }
        if (!current)
            return std::nullopt;
    }

    // Declaration inside a named generate block of the module reached so far.
    const auto find_generate_member = [&](const HierarchyModule& owner, std::string_view label,
                                          std::string_view name) -> const ValueEntry* {
        for (const auto& value : owner.index->values) {
            if (value.name != name || value.generate_label != label ||
                value.parent_scope != owner.entry->name)
                continue;
            return &value;
        }
        return nullptr;
    };

    for (; next + 1 < segments.size(); ++next) {
        const auto* inst = find_instance(*current, segments[next]);
        if (!inst) {
            // A named generate block sits in the path but is not an instance —
            // the index records instances inside a generate under the enclosing
            // module.  Skip the segment only when the next one really is an
            // instance of the module reached so far, so an unknown name cannot
            // silently disappear.
            if (next + 2 < segments.size() && find_instance(*current, segments[next + 1]))
                continue;
            // `dut.g_lane[0].acc`: the block is the last hop and the leaf is one
            // of its declarations, which carries the label in the index.
            if (next + 2 == segments.size()) {
                if (const auto* value =
                        find_generate_member(*current, segments[next], segments.back()))
                    return hierarchy_location(*current->index, current->shard_uri, value->file_id,
                                              value->line, value->col,
                                              (int)segments.back().size());
            }
            return std::nullopt;
        }
        auto target = find_module(inst->module_name);
        if (!target)
            return std::nullopt;
        current = std::move(target);
    }

    // Last segment: a port, a value or an instance of the module reached.
    const auto& leaf = segments.back();
    const auto name_length = (int)leaf.size();
    for (const auto& port : current->entry->ports)
        if (port.name == leaf)
            return hierarchy_location(*current->index, current->shard_uri, port.file_id, port.line,
                                      port.col, name_length);
    for (const auto& value : current->index->values)
        if (value.name == leaf && value.parent_scope == current->entry->name)
            return hierarchy_location(*current->index, current->shard_uri, value.file_id,
                                      value.line, value.col, name_length);
    if (const auto* inst = find_instance(*current, leaf))
        return hierarchy_location(*current->index, current->shard_uri, inst->file_id, inst->line,
                                  0, name_length);
    return std::nullopt;
}

std::optional<Location> Analyzer::definition_of(const std::string& uri, int line, int col) const {
    const auto start = Clock::now();
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    auto extra = extra_file_snapshot_ptr();
    // Skip the current document during extra-file iteration to avoid searching
    // it twice.  The snapshot itself is shared and immutable, so this remains
    // O(1) instead of copying and erase/removing a potentially large filelist
    // vector on every goto-definition request.
    auto result = definition_of_state(*state, uri, line, col, *extra, &uri);
    // Miss path only: an `include directive resolves no identifier, and every
    // successful definition keeps its current cost.
    if (!result)
        result = include_target_at(*state, line);
    // Miss path only: a hierarchical path costs nothing until every other
    // resolution has already failed.
    if (!result)
        result = hierarchical_definition(*state, uri, line, col);
    log_perf("definition_of " + uri + ":" + std::to_string(line) + ":" + std::to_string(col),
             start);
    return result;
}

std::optional<Location>
Analyzer::definition_of_state(const DocumentState& state, const std::string& uri, int line, int col,
                              std::span<const ExtraFileInfo> extra_files,
                              const std::string* skip_extra_uri) const {
    if (!state.tree)
        return std::nullopt;

    auto skip_extra = [&](const ExtraFileInfo& extra) {
        return skip_extra_uri && extra.uri == *skip_extra_uri;
    };

    auto target = definition_target_at(*state.tree, uri, line, col);

    if (target.kind == DefinitionTargetKind::None || target.name.empty()) {
        auto ident = extract_ident_span(state.text, line, col);
        if (!ident || (!is_backtick_identifier(state.text, line, ident->start_col) &&
                       !is_define_identifier(state.text, line, ident->start_col)))
            return std::nullopt;

        if (auto loc = find_macro_definition(*state.tree, uri, ident->text))
            return loc;
        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto loc = find_macro_definition(*extra.state->tree, extra.uri, ident->text))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::Macro) {
        if (auto loc = find_macro_definition(*state.tree, uri, target.name))
            return loc;
        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto loc = find_macro_definition(*extra.state->tree, extra.uri, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::NamedPort) {
        if (auto loc = find_port_definition_in_tree(*state.tree, uri, target.module_name,
                                                    target.name))
            return loc;

        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (auto loc =
                    find_port_definition(extra.index_ref(), extra.uri, target.module_name, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::NamedParameter) {
        if (auto loc = find_port_definition_in_tree(*state.tree, uri, target.module_name,
                                                    target.name))
            return loc;

        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (auto loc =
                    find_port_definition(extra.index_ref(), extra.uri, target.module_name, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::NamedArgument) {
        if (auto loc = find_subroutine_argument_definition(*state.tree, uri, target.subroutine_name,
                                                           target.name))
            return loc;

        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto loc = find_subroutine_argument_definition(*extra.state->tree, extra.uri,
                                                               target.subroutine_name, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::Instance) {
        if (auto loc = find_module_definition_in_tree(*state.tree, uri, target.module_name))
            return loc;
        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (auto loc = find_module_definition(extra.index_ref(), extra.uri, target.module_name))
                return loc;
        }
        return std::nullopt;
    }

    const int use_line_one_based = line + 1;
    const auto& current_index = get_structural_index(state);

    // Every shard a class hierarchy could be spread across, current file first.
    auto class_lookup_shards = [&] {
        std::vector<ClassLookupShard> shards;
        shards.reserve(extra_files.size() + 1);
        shards.push_back(ClassLookupShard{&current_index, &uri});
        for (const auto& extra : extra_files) {
            if (!skip_extra(extra))
                shards.push_back(ClassLookupShard{&extra.index_ref(), &extra.uri});
        }
        return shards;
    };

    if (target.kind == DefinitionTargetKind::PackageMember) {
        // `my_item::type_id::create` — the qualifier is a type alias, so the
        // member lives in the type it names, not under the alias.  Substitute
        // before any lookup below; nothing is keyed by the alias name.
        std::string qualifier = target.package_qualifier;
        if (!target.qualifier_scope.empty()) {
            auto aliased =
                scoped_typedef_base_type(current_index, target.qualifier_scope, qualifier);
            for (const auto& extra : extra_files) {
                if (aliased)
                    break;
                if (skip_extra(extra))
                    continue;
                aliased =
                    scoped_typedef_base_type(extra.index_ref(), target.qualifier_scope, qualifier);
            }
            if (aliased)
                qualifier = *aliased;
        }

        if (auto loc = find_package_member(current_index, uri, qualifier, target.name))
            return loc;
        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (auto loc = find_package_member(extra.index_ref(), extra.uri, qualifier,
                                                target.name))
                return loc;
        }

        // The qualifier may name a class rather than a package: `my_item::type_id`,
        // `my_class::static_method`.  Resolve inside that class only — this is
        // still a qualified lookup, so it must not fall back to unrelated
        // same-named declarations.
        if (state.tree) {
            if (auto loc =
                    find_class_scoped_declaration_in_tree(*state.tree, uri, qualifier, target.name))
                return loc;
        }
        if (auto loc = find_class_member_in_hierarchy(class_lookup_shards(), qualifier,
                                                      target.name))
            return loc;

        // Deliberately no generic fallback.  A qualified name that the package
        // does not export must report "no definition" rather than silently
        // resolving to a same-named local or other-module declaration.
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::Generic) {
        if (auto loc =
                find_aggregate_field_declaration_at(current_index, uri, target.name, line, col))
            return loc;
    }

    if (target.kind == DefinitionTargetKind::ClassMember) {
        // Resolve simple object member calls by first identifying the object's
        // declared object type in the current lexical module, then looking up
        // the requested member inside that class/aggregate.  This intentionally runs
        // before the generic definition fallback so:
        //
        //   Packet p;
        //   p.req_data();
        //
        // jumps to `class Packet::req_data`, not to an unrelated unit-level
        // `task req_data`.
        // Resolution starts at the outermost receiver.  For `a.b.c` that is `a`;
        // `b` is a field of `a`'s type, not a declaration the module has.
        const std::string& base_receiver =
            target.object_path.empty() ? target.object_name : target.object_path.front();

        // `super` and `this` are not declarations to look up; they name the base
        // class and the enclosing class directly.  Resolving `super` explicitly
        // matters for a member the derived class also declares -- a constructor
        // always is one, which is why `super.arm()` worked and `super.new()`
        // answered with the derived class's own `new`.
        std::optional<std::string> class_type;
        if (!target.scope_class.empty()) {
            if (base_receiver == "super")
                class_type = find_class_base_name(class_lookup_shards(), target.scope_class);
            else if (base_receiver == "this")
                class_type = target.scope_class;
        }
        if (!class_type)
            class_type = class_type_for_object_reference(current_index, target.scope_module,
                                                          base_receiver, use_line_one_based);
        if (!class_type) {
            class_type = class_type_for_object_reference_in_tree(
                *state.tree, uri, target.scope_module, base_receiver, use_line_one_based);
        }
        // Inside a class body the enclosing scope is the class, not a module,
        // and the receiver may be a property, a method local, an argument, or a
        // field the class only inherits.
        if (!class_type && !target.scope_class.empty()) {
            class_type = class_type_for_object_reference(current_index, target.scope_class,
                                                         base_receiver, use_line_one_based);
            if (!class_type) {
                class_type = class_type_for_object_reference_in_tree(
                    *state.tree, uri, target.scope_class, base_receiver, use_line_one_based);
            }
            if (!class_type) {
                class_type = find_class_field_type_in_hierarchy(class_lookup_shards(),
                                                                target.scope_class, base_receiver);
            }
        }

        // A receiver declared with a `parameter type` (or any typedef alias)
        // names a stand-in, not the type that declares the members.  Take the
        // alias hop before the lookups below, bounded by a visited set so a
        // circular alias cannot spin.
        const auto resolve_alias_chain = [&](std::optional<std::string> type) {
            std::unordered_set<std::string> visited;
            while (type && visited.insert(*type).second) {
                auto next = typedef_alias_target(current_index, *type);
                if (!next) {
                    for (const auto& extra : extra_files) {
                        if (skip_extra(extra))
                            continue;
                        next = typedef_alias_target(extra.index_ref(), *type);
                        if (next)
                            break;
                    }
                }
                if (!next)
                    break;
                type = std::move(next);
            }
            return type;
        };

        class_type = resolve_alias_chain(std::move(class_type));

        // Follow the rest of the chain one field at a time: the type of `a.b` is
        // the type of field `b` inside `a`'s type.  Bounded by the number of
        // dots the user actually typed, and every step is a lookup in an index
        // already held for this request.
        for (size_t i = 1; class_type && i < target.object_path.size(); ++i) {
            const auto shards = class_lookup_shards();
            auto next = find_class_field_type_in_hierarchy(shards, *class_type,
                                                           target.object_path[i]);
            if (!next) {
                for (const auto& shard : shards) {
                    next = find_typedef_field_type(*shard.index, *class_type,
                                                   target.object_path[i]);
                    if (next)
                        break;
                }
            }
            class_type = resolve_alias_chain(std::move(next));
        }

        if (class_type) {
            // The object type may name either a class or a typedef'd aggregate.
            // Search both compact index families so `obj.field` works for
            // closed-file classes as well as structs/unions.  Class lookup
            // walks the `extends` chain; typedef aggregates have no base type.
            const auto shards = class_lookup_shards();
            if (auto loc = find_class_member_in_hierarchy(shards, *class_type, target.name))
                return loc;
            if (auto loc = find_typedef_field_definition(current_index, uri, *class_type, target.name))
                return loc;
            for (const auto& extra : extra_files) {
                if (skip_extra(extra))
                    continue;
                if (auto loc = find_typedef_field_definition(extra.index_ref(), extra.uri,
                                                             *class_type, target.name))
                    return loc;
            }
        }

        // Interface ports.  `AXI_BUS.Slave bus;` then `bus.aw_valid` — the
        // receiver's declared type names an interface, and the member is one of
        // that interface's signals.  Also covers a cursor on the modport of
        // `AXI_BUS.Slave`, where the receiver is the interface name itself.
        if (target.object_path.size() <= 1) {
            std::string interface_name;
            if (auto type_text = declared_type_text_for_object_reference(
                    current_index, target.scope_module, base_receiver, use_line_one_based))
                interface_name = interface_name_from_type_text(*type_text);
            // A virtual interface handle is a class property, not a module
            // value, so the module-scope lookup above finds nothing.  The
            // receiver type already resolved for the class path is the answer
            // there: `virtual bus_if #(.W_ADDR(8)) vif;` reduces to `bus_if`,
            // whose members live where an instance's do.
            if (interface_name.empty() && class_type)
                interface_name = interface_name_from_type_text(*class_type);
            if (interface_name.empty())
                interface_name = base_receiver;

            if (auto loc = find_interface_member_definition(current_index, uri, interface_name,
                                                            target.name))
                return loc;
            for (const auto& extra : extra_files) {
                if (skip_extra(extra))
                    continue;
                if (auto loc = find_interface_member_definition(extra.index_ref(), extra.uri,
                                                                interface_name, target.name))
                    return loc;
            }
        }

        // `gen_stall_mem.rf_rd_a_hz` — the receiver is a generate block label,
        // not a value, so nothing above can explain it.
        if (state.tree && !target.object_path.empty()) {
            if (auto loc = find_generate_block_member_in_tree(*state.tree, uri,
                                                              target.object_path.back(),
                                                              target.name))
                return loc;
        }
    }

    // `g_lane[0].acc` addresses the `acc` inside that generate block, so the
    // module's own same-named signal is a different object.  Once the member
    // lookups above have missed, answering from the enclosing scope is worse
    // than answering nothing — it sends the user to the wrong declaration, and
    // find-references and rename then merge the two.  Restricted to a receiver
    // that names a scope (a generate block here), because a handle whose type
    // this file cannot resolve still benefits from the generic fallback.
    // Same rule when the path starts at an instance (`u_top.gen_lane[0].sig`):
    // that is an address into another module, and the enclosing module's own
    // same-named signal is a different object.  definition_of() still gets to
    // try the hierarchical walk after this returns nothing.
    const auto path_root_names_a_scope = [&](const std::string& root) {
        if (root.empty())
            return false;
        return (state.tree && generate_block_label_exists_in_tree(*state.tree, root)) ||
               find_instance_definition(current_index, uri, target.scope_module, root).has_value();
    };

    bool receiver_names_a_scope =
        target.kind == DefinitionTargetKind::ClassMember && !target.object_path.empty() &&
        path_root_names_a_scope(target.object_path.front());

    // A longer path (`u_top.gen_lane[0].sig`) never reaches the ClassMember
    // classification — slang spells it as a scoped name — so read the dotted
    // path straight from the source and apply the same rule.
    if (!receiver_names_a_scope) {
        if (auto ident = identifier_at(uri, line, col)) {
            const auto segments = hierarchical_path_at(state.text, line, col, *ident);
            if (segments.size() >= 2)
                receiver_names_a_scope = path_root_names_a_scope(segments.front());
        }
    }

    if (receiver_names_a_scope) {
        // `gen_lane[0].u_leaf.state_q` with the cursor on the instance segment:
        // the name is an instantiation inside that block, not a member of it.
        if (auto loc = find_instance_definition(current_index, uri, target.scope_module,
                                                target.name))
            return loc;
        return std::nullopt;
    }

    std::vector<ImportEntry> visible_imports;
    if (state.tree)
        visible_imports = get_dynamic_index(state).imports;

    if (auto loc = find_generic_definition(*state.tree, uri, target.name, target.scope_module,
                                           target.scope_package, visible_imports,
                                           use_line_one_based))
        return loc;

    // The receiver half of a hierarchical reference: in `u_leaf.state_q` the
    // cursor on `u_leaf` is a plain identifier that matches no declarator, so it
    // used to resolve to nothing even though `state_q` resolved fine.
    if (target.kind == DefinitionTargetKind::Generic) {
        if (auto loc = find_instance_definition(current_index, uri, target.scope_module,
                                                target.name))
            return loc;
    }

    // An unqualified name inside a class body that the current file cannot
    // explain may be an inherited member:
    //
    //     class pkt_child extends pkt_base;
    //         function void bump();
    //             depth = depth + 1;   // pkt_base::depth, declared elsewhere
    //
    // This runs after the current-file walk so method locals and same-file
    // declarations keep shadowing the base class, and before the project-wide
    // generic scan so an unrelated same-named symbol cannot win over a real
    // inherited member.
    if (!target.scope_class.empty()) {
        if (auto loc =
                find_class_member_in_hierarchy(class_lookup_shards(), target.scope_class,
                                               target.name))
            return loc;
    }

    for (const auto& extra : extra_files) {
        if (skip_extra(extra))
            continue;
        if (auto loc = find_generic_definition_from_index(extra.index_ref(), extra.uri, target.name,
                                                          target.scope_module, target.scope_package,
                                                          visible_imports, use_line_one_based))
            return loc;
    }

    return std::nullopt;
}

std::vector<Location> Analyzer::find_references(const std::string& uri, int line, int col,
                                                bool include_declaration) const {
    auto target = identifier_at(uri, line, col);
    // References/Rename must not walk closed project-file ASTs.  Current and
    // other open files are live SyntaxTrees; closed project files require a
    // future reference-occurrence index before they can participate scalably.
    std::vector<ExtraFileInfo> extra_files;
    auto state = get_state(uri);
    if (!state)
        return {};

    // Keep one immutable view of the closed-file project index for the whole
    // request.  Grabbing extra_index_snapshot_ptr() repeatedly would both
    // contend on map_mutex_ and could mix different background-index
    // generations in one references response if a didChange / shard publish
    // happened between loops.
    const auto extra_idx = extra_index_snapshot_ptr();
    if (!target)
        return {};
    const auto target_info = state->tree ? definition_target_at(*state->tree, uri, line, col)
                                         : DefinitionTarget{};
    auto target_def = definition_of_state(*state, uri, line, col, extra_files);
    if (!target_def && target_info.kind == DefinitionTargetKind::Instance) {
        for (const auto& extra : *extra_idx) {
            if ((target_def = find_module_definition(extra.index_ref(), extra.uri,
                                                     target_info.module_name)))
                break;
        }
    } else if (!target_def && target_info.kind == DefinitionTargetKind::NamedPort) {
        for (const auto& extra : *extra_idx) {
            if ((target_def = find_port_definition(extra.index_ref(), extra.uri,
                                                   target_info.module_name, target_info.name)))
                break;
        }
    } else if (!target_def && target_info.kind == DefinitionTargetKind::NamedParameter) {
        for (const auto& extra : *extra_idx) {
            if ((target_def = find_port_definition(extra.index_ref(), extra.uri,
                                                   target_info.module_name, target_info.name)))
                break;
        }
    } else if (!target_def && (target_info.kind == DefinitionTargetKind::Generic ||
                               target_info.kind == DefinitionTargetKind::PackageMember)) {
        // A name only a closed project file can explain — typically a package
        // member reached through an import, either bare or `pkg::`-qualified.
        // definition_of_state() above ran without extra files by design, so it
        // could not leave the open buffers.  Recover the declaration from the
        // compact shards rather than walking closed-file ASTs; without this,
        // references/rename started *from the use site* return nothing at all.
        const auto visible_imports =
            state->tree ? get_dynamic_index(*state).imports : std::vector<ImportEntry>{};
        for (const auto& extra : *extra_idx) {
            if (target_info.kind == DefinitionTargetKind::PackageMember) {
                target_def = find_package_member(extra.index_ref(), extra.uri,
                                                 target_info.package_qualifier, target_info.name);
            } else {
                target_def = find_generic_definition_from_index(
                    extra.index_ref(), extra.uri, target_info.name, target_info.scope_module,
                    target_info.scope_package, visible_imports, line + 1);
            }
            if (target_def)
                break;
        }
    } else if (!target_def && target_info.kind == DefinitionTargetKind::ClassMember) {
        // A member access whose declaring type lives in another file:
        // `handle.field`, `iface.sig`, `iface.task()`, `pkt.header`.  The
        // definition_of_state() call above deliberately ran with no extra files,
        // so it could not leave the open buffers and left target_def null -- and
        // without this recovery the request returns *nothing at all* from the use
        // site, while the very same cursor position answers go-to-definition
        // correctly and a search started from the declaration finds this use.
        //
        // Miss path only, so a references request that already resolves keeps its
        // current cost.  The snapshot is the shared immutable one go-to-definition
        // itself uses: closed project files carry only their compact index shard
        // (ExtraFileInfo::state is null for them), so this resolves through the
        // same index lookups and never walks a closed file's AST.
        const auto extra_full = extra_file_snapshot_ptr();
        target_def = definition_of_state(*state, uri, line, col, *extra_full, &uri);
    }
    if (!target_def) {
        // Last resort, still miss-path only: a hierarchical path such as
        // `u_mid.u_leaf.r_stage` is resolved by hierarchical_definition(), which
        // definition_of() calls *after* definition_of_state() and which the
        // recovery above therefore never reaches.  Without it, references from a
        // hierarchical use site return nothing even though go-to-definition on
        // the same token lands on the declaration.
        target_def = hierarchical_definition(*state, uri, line, col);
    }
    if (!target_def)
        return {};

    std::string target_symbol_debug;
    if (target_info.kind == DefinitionTargetKind::Instance) {
        // Go-to-definition on an instance name resolves to the instantiated
        // module declaration.  Use the module symbol identity for the closed
        // project occurrence index, matching other instantiations of that
        // module and the declaration itself instead of all same-spelled
        // instance names.
        target_symbol_debug = "module::" + target_info.module_name;
    } else if (target_info.kind == DefinitionTargetKind::NamedPort) {
        target_symbol_debug = "module_port::" + target_info.module_name + "::" + target_info.name;
    } else if (target_info.kind == DefinitionTargetKind::NamedParameter) {
        target_symbol_debug =
            "module_param::" + target_info.module_name + "::" + target_info.name;
    } else if (target_info.kind == DefinitionTargetKind::ClassMember) {
        const auto& current_index = get_structural_index(*state);
        const int use_line_one_based = line + 1;
        auto class_type =
            class_type_for_object_reference(current_index, target_info.scope_module,
                                            target_info.object_name, use_line_one_based);
        if (!class_type && state->tree) {
            class_type = class_type_for_object_reference_in_tree(
                *state->tree, uri, target_info.scope_module, target_info.object_name,
                use_line_one_based);
        }
        const bool has_class_method =
            class_type &&
            (find_class_method_definition(current_index, uri, *class_type, target_info.name)
                 .has_value() ||
             std::any_of(extra_idx->begin(), extra_idx->end(), [&](const ExtraIndexInfo& extra) {
                 return find_class_method_definition(extra.index_ref(), extra.uri, *class_type,
                                                     target_info.name)
                     .has_value();
             }));
        if (has_class_method)
            target_symbol_debug =
                symbol_canonical("class_method", *class_type, target_info.name);
    } else if (target_info.kind == DefinitionTargetKind::Macro) {
        target_symbol_debug = "macro::" + target_info.name;
    } else if (is_backtick_identifier(state->text, line, target->col) ||
               is_define_identifier(state->text, line, target->col)) {
        // Raw backtick fallback: if the cursor did not map to an expansion
        // token (or the cursor is on a `define name), still use the same macro
        // SymbolID that the syntax index emits for declarations and invocation
        // sites.
        target_symbol_debug = "macro::" + target->name;
    }
    if (target_symbol_debug.empty()) {
        // Prefer the symbol identity at the token the user actually clicked.
        // This matters for declaration tokens whose plain name appears in
        // multiple declaration scopes:
        //
        //   typedef struct { logic addr; } a_t;
        //   typedef struct { logic addr; } b_t;
        //                          ^ clicked here
        //
        // A generic definition fallback may find the first `addr` declaration
        // textually, but the current-file structural index has a scoped
        // declaration occurrence at the clicked location:
        //
        //   typedef_field::b_t::addr
        //
        // Recovering that ID first prevents same-name typedef fields from being
        // merged by references/rename.
        // By reference: this is the index cached on the immutable snapshot,
        // and `state` holds it alive for the rest of the request.  Binding it
        // to a value copied every declaration and reference occurrence in the
        // file, on a path a click runs.
        const auto& current_structural_index = get_structural_index(*state);
        Location clicked_loc{uri, target->line, target->col, target->line, target->end_col};
        // Prefer the symbol identity at the token the user actually clicked.
        // This matters for declaration tokens whose plain name appears in
        // multiple declaration scopes:
        //
        //   typedef struct { logic addr; } a_t;
        //   typedef struct { logic addr; } b_t;
        //                          ^ clicked here
        //
        // A generic definition fallback may find the first `addr` declaration
        // textually, but the current-file structural index has a scoped
        // declaration occurrence at the clicked location:
        //
        //   typedef_field::b_t::addr
        //
        // Recovering that ID first prevents same-name typedef fields from being
        // merged by references/rename.  It is also what identifies an override
        // as itself rather than as the base method it resolves to.
        if (auto id = symbol_id_for_index_location(current_structural_index, clicked_loc)) {
            // Two spellings name a *use*, not a declaration, and must not be
            // adopted here:
            //
            //   * `scoped_member::P::N` is what a shard records for `P::N` when
            //     it never parsed P.
            //   * `class_member::C::N` is the kind-neutral alias for
            //     `handle.member`; for an inherited member C is the *deriving*
            //     class, not the one that declares the member.
            //
            // Adopting either restricts the search to the occurrences spelled
            // that same weak way, silently dropping the declaration and every
            // sibling use -- which is what makes rename from such a use site
            // rewrite only part of the symbol and leave the code uncompilable.
            // Leave them to the alias bridging below and let the declaration's
            // own shard supply the authoritative identity.
            if (!id->starts_with("scoped_member::") && !id->starts_with("class_member::"))
                target_symbol_debug = *id;
        }

        // If the cursor was on a declaration or on a syntactic generic token
        // such as a hierarchy type, recover the project-index SymbolID from the
        // declaration location.  This keeps "find references from declaration"
        // precise without keeping closed-file ASTs alive.
        if (target_symbol_debug.empty() && target_def->uri == uri) {
            if (auto id = symbol_id_for_index_location(current_structural_index, *target_def))
                target_symbol_debug = *id;
        }
        if (target_symbol_debug.empty()) {
            for (const auto& extra : *extra_idx) {
                if (extra.uri != target_def->uri)
                    continue;
                if (auto id = symbol_id_for_index_location(extra.index_ref(), *target_def))
                    target_symbol_debug = *id;
                break;
            }
        }
    }
    std::string fallback_symbol_debug;
    if (target_symbol_debug.empty()) {
        // If the lightweight index cannot prove a scope-qualified identity, keep
        // open-buffer references on the AST/definition-verification path below
        // to avoid same-name false positives.  Closed project files cannot be
        // walked as ASTs, so retain a conservative unresolved-name SymbolID for
        // their compact occurrence shards.  This fixes the "empty references"
        // case for symbols whose only indexed identity is `name:<identifier>`
        // without downgrading precise open-file reference searches.
        // Do not use unresolved `name:<identifier>` as a bridge out of an open
        // buffer.  The AST path below can verify same-definition references
        // precisely, but a closed shard cannot distinguish scopes for generic
        // names such as a method-local `item` versus an unrelated `item` in a
        // library file.  Only a declaration that lives in a closed file needs
        // the bridge, because nothing can walk its AST to do better.
        //
        // Testing the declaration's own URI rather than the request URI is what
        // makes the guard hold: an open file is normally listed in the filelist
        // too, so its own closed shard offers exactly the `name:` ID this is
        // meant to refuse.  Owner-qualified SymbolIDs above still carry
        // cross-file references for modules, ports, parameters, typedef fields
        // and macros.
        if (!get_state(target_def->uri)) {
            for (const auto& extra : *extra_idx) {
                if (extra.uri != target_def->uri)
                    continue;
                if (auto id = symbol_id_for_index_location(extra.index_ref(), *target_def, true);
                    id && id->starts_with("name:")) {
                    fallback_symbol_debug = *id;
                }
                break;
            }
        }
    }
    // A module port is indexed under two identities at the same declaration
    // location: `module_port::M::P` (also emitted for `.P(...)` named port
    // connections at instantiation sites) and `module_signal::M::P` (emitted for
    // ordinary body references, because ports are module values too).  Parameter
    // ports pair `module_param::M::P` with `module_signal::M::P` the same way.
    // Whichever identity the clicked location resolves to, references/rename must
    // match both, otherwise renaming a port declaration misses every use of it in
    // the module body.  `module_port::M::P` exists only when M declares port P,
    // so aliasing a plain module signal cannot collide with an unrelated symbol.
    constexpr std::array<std::string_view, 3> kModuleMemberPrefixes = {
        "module_port::", "module_param::", "module_signal::"};
    std::vector<SymbolID> alias_symbol_ids;
    for (const auto prefix : kModuleMemberPrefixes) {
        if (!target_symbol_debug.starts_with(prefix))
            continue;
        const auto qualified_name = target_symbol_debug.substr(prefix.size());
        for (const auto alias_prefix : kModuleMemberPrefixes) {
            if (alias_prefix == prefix)
                continue;
            alias_symbol_ids.push_back(
                SymbolID::from_canonical(std::string(alias_prefix) + qualified_name));
        }
        break;
    }

    const SymbolID target_symbol_id = SymbolID::from_canonical(target_symbol_debug);
    const SymbolID fallback_symbol_id = SymbolID::from_canonical(fallback_symbol_debug);
    const bool allow_include_name_bridge =
        target && target_info.scope_package.empty() &&
        !target_symbol_debug.starts_with("class_method::");
    const SymbolID include_bridge_name_id =
        allow_include_name_bridge ? SymbolID::from_canonical("name:" + target->name) : SymbolID{};

    // Package members are reached from other files by import, and an importing
    // file cannot attribute the bare name to its owning package on its own:
    //
    //   common_pkg.sv   package common_pkg; function void foo(); ...
    //                     -> package_subroutine::common_pkg::foo
    //   pkg_use.sv      import common_pkg::*; ... foo();
    //                     -> name:foo, because this shard never saw the package
    //
    // Bridge the two, but only into shards whose own import list makes this
    // package's members visible, so unrelated same-named symbols elsewhere in
    // the project cannot be merged into the rename.
    std::string target_package;
    {
        auto owning_package = [&](const SyntaxIndex& index) {
            for (const auto& [package_name, symbols] : index.package_symbols) {
                if (std::find(symbols.begin(), symbols.end(), target->name) != symbols.end())
                    return package_name;
            }
            return std::string{};
        };
        if (target_def->uri == uri)
            target_package = owning_package(get_structural_index(*state));
        if (target_package.empty()) {
            for (const auto& extra : *extra_idx) {
                if (extra.uri != target_def->uri)
                    continue;
                target_package = owning_package(extra.index_ref());
                break;
            }
        }
    }
    const SymbolID import_bridge_name_id =
        target_package.empty() ? SymbolID{} : SymbolID::from_canonical("name:" + target->name);

    // A file that writes `common_pkg::foo` instead of importing states the
    // owning package at the use site, so its shard records
    // `scoped_member::common_pkg::foo` without having parsed the package.  The
    // spelling is deliberately kind-neutral: the referencing shard cannot tell
    // a parameter from a typedef, a function or an enum member, and all four
    // reach it the same way.  Matching on scope and name is what makes one
    // alias cover them, and it stays additive -- a shard that did parse the
    // package still emits the precise `package_value::` identity, which
    // target_symbol_id already matches.
    const SymbolID scoped_member_alias_id =
        target_package.empty()
            ? SymbolID{}
            : SymbolID::from_canonical("scoped_member::" + target_package + "::" + target->name);

    // `handle.member` in another file is recorded under the receiver's bare
    // type name and a kind-neutral `class_member::` prefix, because that shard
    // never parsed the class body: it knows neither the owning package nor
    // whether the member is a field or a method.  Treat that spelling as the
    // same symbol.  When the class is package-scoped the alias is admitted only
    // inside shards importing that package, so a same-named member on an
    // unrelated class stays out of the result.
    SymbolID class_member_alias_id;
    std::string class_member_package;
    std::string class_member_class;
    std::string class_member_name;
    // An interface member — `bus.gnt`, `tb_bus.drive(...)` — is recorded by a
    // file that only *uses* the interface under the same kind-neutral
    // `class_member::` alias, so its declaration accepts that spelling too.
    // Gated on the owner really being an interface, so an ordinary module
    // signal never starts matching handle-shaped occurrences.
    const auto owner_is_interface = [&](std::string_view owner) {
        if (owner.empty())
            return false;
        const std::string owner_name(owner);
        if (get_structural_index(*state).interface_names.contains(owner_name))
            return true;
        for (const auto& extra : *extra_idx) {
            if (extra.index_ref().interface_names.contains(owner_name))
                return true;
        }
        if (auto project = project_index_snapshot()) {
            for (const auto& shard : project->shards) {
                if (shard.index && shard.index->interface_names.contains(owner_name))
                    return true;
            }
        }
        return false;
    };

    // `typedef_field::` joins the two class spellings here: a struct field
    // reached through a receiver whose typedef this shard never parsed is
    // recorded with the same kind-neutral `class_member::` alias, because that
    // shard cannot tell a struct field from a class member either.
    for (const auto prefix : {std::string_view("class_field::"),
                              std::string_view("class_method::"),
                              std::string_view("typedef_field::"),
                              std::string_view("module_signal::"),
                              std::string_view("interface_subroutine::")}) {
        if (!target_symbol_debug.starts_with(prefix))
            continue;
        const std::string_view rest = std::string_view(target_symbol_debug).substr(prefix.size());
        const auto member_sep = rest.rfind("::");
        if (member_sep == std::string_view::npos)
            break;
        const auto owner = rest.substr(0, member_sep);
        // A module signal or subroutine only takes the handle-shaped alias when
        // its owner really is an interface; a plain module signal is reached by
        // a hierarchical path, which is a different question.
        if ((prefix == "module_signal::" || prefix == "interface_subroutine::") &&
            !owner_is_interface(owner))
            break;
        const auto member = rest.substr(member_sep + 2);
        const auto scope_sep = owner.rfind("::");
        if (scope_sep == std::string_view::npos) {
            class_member_class = std::string(owner);
        } else {
            class_member_package = std::string(owner.substr(0, scope_sep));
            class_member_class = std::string(owner.substr(scope_sep + 2));
        }
        class_member_name = std::string(member);
        if (!class_member_class.empty())
            class_member_alias_id = SymbolID::from_canonical(
                "class_member::" + class_member_class + "::" + class_member_name);
        break;
    }

    // An unqualified inherited member — `depth` inside a class that extends the
    // one declaring it — is recorded by the deriving file's shard as
    // `class_member::<that class>::depth`, because that shard never parsed the
    // base class and cannot name the owner.  Complete the hop here: accept such
    // an occurrence only when its class really derives from the class the
    // clicked declaration belongs to, so an unrelated class with a same-named
    // member stays out of references and rename.
    std::vector<ClassLookupShard> hierarchy_shards;
    std::unordered_map<std::string, bool> derives_cache;
    const auto* current_structural_for_hierarchy =
        class_member_class.empty() ? nullptr : &get_structural_index(*state);
    if (current_structural_for_hierarchy) {
        hierarchy_shards.reserve(extra_idx->size() + 1);
        hierarchy_shards.push_back(ClassLookupShard{current_structural_for_hierarchy, &uri});
        for (const auto& extra : *extra_idx)
            hierarchy_shards.push_back(ClassLookupShard{&extra.index_ref(), &extra.uri});
    }

    auto is_inherited_member_occurrence = [&](const ReferenceEntry& ref) {
        static constexpr std::string_view kPrefix = "class_member::";
        if (class_member_class.empty() || !ref.symbol_debug.starts_with(kPrefix))
            return false;
        const std::string_view rest = std::string_view(ref.symbol_debug).substr(kPrefix.size());
        const auto member_sep = rest.rfind("::");
        if (member_sep == std::string_view::npos)
            return false;
        if (rest.substr(member_sep + 2) != class_member_name)
            return false;
        std::string derived(rest.substr(0, member_sep));
        if (derived == class_member_class)
            return false; // already covered by class_member_alias_id
        auto [it, inserted] = derives_cache.try_emplace(derived, false);
        if (inserted)
            it->second = class_derives_from(hierarchy_shards, derived, class_member_class);
        return it->second;
    };

    // A file may name a package type without importing it — `pkg::beat_t b;`
    // then `b.field`.  Its shard has no ImportEntry, but it does declare
    // something of that qualified type, which is the same proof that the
    // occurrence means this package's type and not a same-named one elsewhere.
    // Declarations are orders of magnitude fewer than occurrences, and the
    // answer is cached per shard, so this stays off the hot path.
    std::unordered_map<const SyntaxIndex*, bool> declares_qualified_owner_cache;
    auto shard_declares_qualified_owner = [&](const SyntaxIndex& index) {
        if (class_member_package.empty() || class_member_class.empty())
            return false;
        auto [it, inserted] = declares_qualified_owner_cache.try_emplace(&index, false);
        if (!inserted)
            return it->second;

        const std::string qualified = class_member_package + "::" + class_member_class;
        const auto names_type = [&](const std::string& type_text) {
            if (type_text.rfind(qualified, 0) != 0)
                return false;
            // `pkg::beat_t` and `pkg::beat_t [3:0]`, but not `pkg::beat_two_t`.
            return type_text.size() == qualified.size() ||
                   !syntax_fragment_edge_is_wordlike(type_text[qualified.size()]);
        };

        bool found = false;
        for (const auto& value : index.values) {
            if (names_type(value.type)) {
                found = true;
                break;
            }
        }
        if (!found) {
            for (const auto& module : index.modules) {
                for (const auto& port : module.ports) {
                    if (names_type(port.type) || names_type(port.decl_type)) {
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        }
        it->second = found;
        return found;
    };

    auto admits_class_member_alias = [&](const std::vector<ImportEntry>& imports,
                                         const SyntaxIndex& index) {
        // A class outside any package is reached by bare name, so there is no
        // import to require.
        if (class_member_package.empty())
            return true;
        if (std::any_of(imports.begin(), imports.end(), [&](const ImportEntry& import) {
                return import.package_name == class_member_package &&
                       (import.wildcard || import.symbol_name == class_member_class);
            }))
            return true;
        return shard_declares_qualified_owner(index);
    };

    auto imports_target_package = [&](const std::vector<ImportEntry>& imports) {
        return std::any_of(imports.begin(), imports.end(), [&](const ImportEntry& import) {
            return import.package_name == target_package &&
                   (import.wildcard || import.symbol_name == target->name);
        });
    };

    auto index_includes_target_source = [&](const SyntaxIndex& index) {
        // Included-file declarations can be seen under two different syntactic
        // worlds: the standalone header that the user opened, and every parsed
        // includer shard.  If a shard lists the definition URI as an include
        // dependency, unresolved `name:<identifier>` occurrences in that shard
        // are plausible references to the clicked header declaration.
        //
        // Important: do not treat `source_files` alone as a match.  The root
        // file is also present there, and enabling a name bridge for ordinary
        // same-file symbols revives the broad fallback false positives that the
        // owner-qualified SymbolID path is designed to avoid.
        return target_def &&
               std::find(index.include_dependencies.begin(), index.include_dependencies.end(),
                         target_def->uri) != index.include_dependencies.end();
    };

    auto reference_matches_target = [&](const SyntaxIndex& index, const ReferenceEntry& ref,
                                        const std::vector<ImportEntry>& imports) {
        if (target_symbol_id && ref.symbol_id == target_symbol_id)
            return true;
        if (std::find(alias_symbol_ids.begin(), alias_symbol_ids.end(), ref.symbol_id) !=
            alias_symbol_ids.end())
            return true;
        if (fallback_symbol_id && ref.symbol_id == fallback_symbol_id)
            return true;
        if (include_bridge_name_id && ref.symbol_id == include_bridge_name_id &&
            index_includes_target_source(index))
            return true;
        if (import_bridge_name_id && ref.symbol_id == import_bridge_name_id &&
            imports_target_package(imports))
            return true;
        if (scoped_member_alias_id && ref.symbol_id == scoped_member_alias_id)
            return true;
        if (class_member_alias_id && ref.symbol_id == class_member_alias_id &&
            admits_class_member_alias(imports, index))
            return true;
        if (is_inherited_member_occurrence(ref) && admits_class_member_alias(imports, index))
            return true;
        return false;
    };

    std::vector<Location> result;
    std::set<std::tuple<std::string, int, int>> seen;

    auto add_if_same_definition =
        [&](const std::string& file_uri, int ref_line, int ref_col,
            const std::function<std::optional<Location>(const std::string&, int, int)>& resolver) {
            auto candidate_def = resolver(file_uri, ref_line, ref_col);
            if (!candidate_def || !same_location(*candidate_def, *target_def))
                return;
            if (!include_declaration && file_uri == target_def->uri &&
                ref_line == target_def->line && ref_col == target_def->col)
                return;

            auto key = std::make_tuple(file_uri, ref_line, ref_col);
            if (!seen.insert(key).second)
                return;
            result.push_back(Location{file_uri, ref_line, ref_col, ref_line,
                                      ref_col + (int)utf16_length(target->name)});
        };

    auto visit_tree =
        [&](const slang::syntax::SyntaxTree& tree, const std::string& fallback_uri,
            const std::function<std::optional<Location>(const std::string&, int, int)>& resolver) {
            struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
                const slang::SourceManager& sm;
                const std::string& fallback_uri;
                const std::string& name;
                const std::function<void(const std::string&, int, int)>& add;

                Visitor(const slang::SourceManager& sm, const std::string& fallback_uri,
                        const std::string& name,
                        const std::function<void(const std::string&, int, int)>& add)
                    : sm(sm), fallback_uri(fallback_uri), name(name), add(add) {}

                void visitToken(slang::parsing::Token token) {
                    if (!token || token.kind != slang::parsing::TokenKind::Identifier ||
                        token.valueText() != name)
                        return;

                    const auto loc = location_from_token_actual_uri(sm, fallback_uri, token);
                    add(loc.uri, loc.line, loc.col);
                }
            };

            std::function<void(const std::string&, int, int)> add =
                [&](const std::string& candidate_uri, int ref_line, int ref_col) {
                    add_if_same_definition(candidate_uri, ref_line, ref_col, resolver);
                };

            Visitor visitor(tree.sourceManager(), fallback_uri, target->name, add);
            tree.root().visit(visitor);
        };

    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> open_states;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        open_states.reserve(docs_.size());
        for (const auto& [state_uri, state] : docs_)
            open_states.emplace_back(state_uri, state);
    }

    std::unordered_map<std::string, std::shared_ptr<const DocumentState>> open_state_by_uri;
    std::unordered_set<std::string> open_uris;
    for (const auto& [state_uri, state] : open_states) {
        open_state_by_uri[state_uri] = state;
        // Compare against extra.uri's canonical spelling below, not the raw
        // client URI: a client can open a file through a symlinked path while
        // the filelist indexer reaches the same file through its canonical
        // path (uri_from_path() resolves symlinks via weakly_canonical()).  A
        // raw string mismatch here would fail to skip the file's own closed
        // shard, double-counting every reference in it.
        open_uris.insert(state ? uri_from_path(state->normalized_path) : state_uri);
    }

    auto resolve_snapshot = [&](const std::string& candidate_uri, int ref_line,
                                int ref_col) -> std::optional<Location> {
        if (auto it = open_state_by_uri.find(candidate_uri); it != open_state_by_uri.end()) {
            if (!it->second)
                return std::nullopt;
            return definition_of_state(*it->second, candidate_uri, ref_line, ref_col, extra_files);
        }
        return std::nullopt;
    };

    auto add_indexed_reference = [&](const std::string& file_uri, const SyntaxIndex& index,
                                     const ReferenceEntry& ref) {
        // `file_uri` is the parsed shard / open document URI.  Reference
        // occurrences can originate from an included header inside that shard,
        // so prefer the per-token URI when the index captured one:
        //
        //   memory.sv     `include "params.svh"
        //   params.svh    task add_number; endtask
        //
        // Without this, the params.svh line/column would be reported against
        // memory.sv and could appear to point at an unrelated token such as an
        // input port named `address`.
        const auto actual_uri = index.source_uri(ref.file_id);
        const std::string& result_uri = actual_uri.empty() ? file_uri : actual_uri;
        const int ref_line = to_lsp_line(ref.line);
        if (!include_declaration && result_uri == target_def->uri &&
            ref_line == target_def->line && ref.col == target_def->col)
            return;

        auto key = std::make_tuple(result_uri, ref_line, ref.col);
        if (!seen.insert(key).second)
            return;
        result.push_back(Location{
            .uri = result_uri,
            .line = ref_line,
            .col = ref.col,
            .end_line = ref_line,
            .end_col = ref.end_col,
            .form = ref.form,
        });
    };

    for (const auto& [state_uri, state] : open_states) {
        if (!state || !state->tree)
            continue;

        if (target_symbol_id) {
            // For owner-qualified symbols (module / port / parameter), use the
            // same compact occurrence representation for open files that closed
            // project files use.  This is important for cross-file open buffers:
            //
            //   memory.sv      module memory; endmodule
            //   memory_top.sv  memory u_mem();
            //
            // Resolving `memory` in memory_top.sv through definition_of_state()
            // would require closed/project-file ASTs in the resolver.  The
            // SymbolID path avoids that by matching `module:memory` directly.
            // By reference, for the reason above; `state` is the loop's own
            // snapshot handle and outlives every use below.
            const auto& open_index = get_structural_index(*state);
            // The structural index deliberately omits imports; the dynamic
            // shard is the cached view that carries them.  The class-member
            // alias needs them too: admits_class_member_alias() proves a
            // `handle.member` occurrence really means *this* package's class by
            // finding the import that makes the class visible, so with an empty
            // import list it refuses the occurrence and a use in an open buffer
            // is dropped -- including the one under the cursor.  The dynamic
            // index is cached per immutable DocumentState, so asking for it here
            // is a lookup, not a rebuild.
            const auto& open_imports = (import_bridge_name_id || class_member_alias_id)
                                           ? get_dynamic_index(*state).imports
                                           : open_index.imports;
            for (const auto& ref : open_index.references) {
                if (reference_matches_target(open_index, ref, open_imports))
                    add_indexed_reference(state_uri, open_index, ref);
            }
            if ((target_info.kind == DefinitionTargetKind::ClassMember &&
                 target_symbol_debug.starts_with("class_method::")) ||
                target_symbol_debug.starts_with("class_field::"))
                // A class field is written both bare inside the class body and
                // as `handle.field` elsewhere.  Only the first form carries the
                // scoped `class_field::` identity in a shard: the second is
                // indexed as an unresolved name, because the shard cannot type
                // the receiver.  Verifying candidate tokens against the
                // declaration recovers those uses without widening the
                // SymbolID match to every same-named symbol in the project.
                visit_tree(*state->tree, state_uri, resolve_snapshot);
        } else {
            visit_tree(*state->tree, state_uri, resolve_snapshot);
        }
    }

    // Closed project files are represented by compact reference-occurrence
    // shards.  We intentionally do not load or walk their full SyntaxTrees here.
    for (const auto& extra : *extra_idx) {
        if (open_uris.contains(extra.uri))
            continue;
        if (!target_symbol_id && !fallback_symbol_id && !include_bridge_name_id &&
            !import_bridge_name_id && !scoped_member_alias_id)
            continue;

        // SyntaxIndex intentionally no longer stores SymbolID -> reference
        // acceleration buckets.  That keeps initial project indexing lighter and
        // closer to the v1.0.4 model; explicit references / rename requests pay
        // the linear scan cost over compact ReferenceEntry records instead.
        for (const auto& ref : extra.index_ref().references) {
            if (reference_matches_target(extra.index_ref(), ref, extra.index_ref().imports))
                add_indexed_reference(extra.uri, extra.index_ref(), ref);
        }
    }

    std::sort(result.begin(), result.end(), [](const Location& a, const Location& b) {
        return std::tie(a.uri, a.line, a.col) < std::tie(b.uri, b.line, b.col);
    });
    return result;
}

std::vector<std::pair<int, int>> Analyzer::find_occurrences(const std::string& uri,
                                                            const std::string& name) const {
    auto state = get_state(uri);
    if (!state || name.empty())
        return {};

    std::string_view src = state->text;
    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };

    // Build line-start offsets
    std::vector<size_t> ls;
    ls.push_back(0);
    for (size_t i = 0; i < src.size(); ++i)
        if (src[i] == '\n')
            ls.push_back(i + 1);

    std::vector<std::pair<int, int>> result;
    size_t pos = 0;
    while (pos < src.size()) {
        auto found = src.find(name, pos);
        if (found == std::string_view::npos)
            break;

        bool before_ok = (found == 0) || !is_id(src[found - 1]);
        bool after_ok = (found + name.size() >= src.size()) || !is_id(src[found + name.size()]);

        if (before_ok && after_ok) {
            auto it = std::upper_bound(ls.begin(), ls.end(), found);
            int line = (int)(it - ls.begin()) - 1; // 0-based
            int col = (int)(found - ls[(size_t)line]);
            result.push_back({line, col});
        }
        pos = found + 1;
    }
    return result;
}

void Analyzer::set_project_index_publish_debounce_ms(int debounce_ms) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    background_publish_debounce_ms_ = std::max(0, debounce_ms);
}

void Analyzer::set_defines(const std::vector<std::string>& defines) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    defines_ = defines;
    // Invalidate extra-file cache so reopened files pick up the new defines.
    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_include_dirs(const std::vector<std::string>& include_dirs) {
    std::vector<std::string> normalized_include_dirs;
    normalized_include_dirs.reserve(include_dirs.size());
    for (const auto& dir : include_dirs)
        normalized_include_dirs.push_back(normalize_filesystem_path(dir).string());

    auto resolved_include_dirs = resolve_include_dirs(normalized_include_dirs);

    std::lock_guard<std::mutex> lock(map_mutex_);
    include_dirs_ = std::move(normalized_include_dirs);
    include_dir_paths_ = std::move(resolved_include_dirs);

    // Include paths affect parsing every explicit filelist source.  Clear the
    // cache even if the filelist itself did not change, otherwise a newly added
    // UVM include directory would not be visible until the next source edit.
    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_extra_files(const std::vector<std::string>& paths,
                               const std::string& filelist_path) {
    std::vector<std::string> normalized_paths;
    normalized_paths.reserve(paths.size());
    for (const auto& path : paths)
        normalized_paths.push_back(normalize_filesystem_path(path).string());

    std::lock_guard<std::mutex> lock(map_mutex_);
    filelist_path_ = filelist_path;
    extra_files_ = std::move(normalized_paths);
    extra_file_set_.clear();
    extra_file_set_.reserve(extra_files_.size());
    for (const auto& path : extra_files_)
        extra_file_set_.insert(path);
    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();

    // Always index configured project files asynchronously, regardless of
    // whether they came from a .f file or an explicit path list.  This keeps
    // configuration reload / startup from synchronously parsing large designs.
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_project_config(const std::vector<std::string>& defines,
                                  const std::vector<std::string>& include_dirs,
                                  const std::vector<std::string>& extra_files,
                                  const std::string& filelist_path,
                                  const std::string& project_root) {
    std::vector<std::string> normalized_include_dirs;
    normalized_include_dirs.reserve(include_dirs.size());
    for (const auto& dir : include_dirs)
        normalized_include_dirs.push_back(normalize_filesystem_path(dir).string());

    std::vector<std::string> normalized_extra_files;
    normalized_extra_files.reserve(extra_files.size());
    for (const auto& path : extra_files)
        normalized_extra_files.push_back(normalize_filesystem_path(path).string());

    auto resolved_include_dirs = resolve_include_dirs(normalized_include_dirs);

    // Opened before the lock.  create_directories() plus a .gitignore write is
    // filesystem work, and on the shared filesystems this cache is aimed at it
    // is a round trip -- map_mutex_ is the lock every request handler contends
    // for, and initialize and every config reload would otherwise hold it
    // across that.
    auto config_digest = IndexCache::config_digest(defines, resolved_include_dirs);
    auto cache = project_root.empty() ? std::nullopt : IndexCache::open(project_root);

    std::lock_guard<std::mutex> lock(map_mutex_);

    // Apply every parse-affecting project input under one lock.  A config reload
    // commonly changes several of these at once (for example a new .f file plus
    // +incdir+ entries and preprocessor defines).  Clearing and scheduling once
    // avoids creating redundant background generations that cannot commit but
    // can still burn CPU / shared-filesystem bandwidth while they parse.
    defines_ = defines;
    include_dirs_ = std::move(normalized_include_dirs);
    include_dir_paths_ = std::move(resolved_include_dirs);

    filelist_path_ = filelist_path;
    extra_files_ = std::move(normalized_extra_files);
    extra_file_set_.clear();
    extra_file_set_.reserve(extra_files_.size());
    for (const auto& path : extra_files_)
        extra_file_set_.insert(path);

    // Installed before the burst is scheduled so the preload gate finds them
    // ready.  The digest covers defines and include directories together: both
    // change what a parse of an unchanged file means, and a shard keyed on only
    // one of them would be served after the other moved.
    index_cache_config_digest_ = config_digest;
    index_cache_ = std::move(cache);

    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();

    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::prune_cache_once_per_generation(
    uint64_t generation, const std::unordered_set<std::string>& live_uris) const {
    std::optional<IndexCache> cache;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (!index_cache_ || generation != background_generation_ ||
            index_cache_pruned_generation_ == generation)
            return;
        index_cache_pruned_generation_ = generation;
        cache = index_cache_;
    }
    // On the writer thread, which already runs at the lowest priority this
    // process asks for, and after a shard has been written -- so it never sits
    // between a parse and the launch that wants it.  One stat per shard.
    cache->prune_missing_sources(live_uris);
}

void Analyzer::reserve_shard_writes(size_t count) const {
    if (count == 0)
        return;
    // Called with map_mutex_ held, which fixes the lock order against
    // index_cache_writer_loop(): map_mutex_ first, then this one.
    std::lock_guard<std::mutex> lock(index_cache_write_mutex_);
    index_cache_writes_reserved_ += count;
}

bool Analyzer::shard_writes_need_inline_drain() const {
    // On a one-CPU slice there is no other core to move the write to, and a
    // second runnable thread only adds context switches and holds the shard
    // alive while it queues.  Measured on a 5953-shard project: handing writes
    // to a thread is 25% off a cold start with every CPU available and 19%
    // *onto* it with one, and one is the slice a batch-scheduled node grants.
    // So the writer exists exactly when there is somewhere for it to run.
    static const bool use_writer_thread = available_cpu_count() > 1;
    return !use_writer_thread;
}

void Analyzer::drain_shard_writes_inline() const {
    for (;;) {
        PendingShardWrite write;
        {
            std::lock_guard<std::mutex> lock(index_cache_write_mutex_);
            if (index_cache_write_queue_.empty())
                return;
            write = std::move(index_cache_write_queue_.front());
            index_cache_write_queue_.pop_front();
            index_cache_writing_ = true;
        }

        // Same generation check the writer thread makes: a shard keyed on
        // defines that have since moved would serve the wrong index.
        bool current_generation = false;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            current_generation = write.generation == background_generation_;
        }
        if (current_generation) {
            if (write.index) {
                store_shard_in_cache(write.uri, *write.index, write.include_resolutions,
                                     write.extra_dependency_uri, write.stands_alone);
            }
            if (write.prune_only)
                prune_cache_once_per_generation(write.generation, write.live_uris);
        }

        std::lock_guard<std::mutex> lock(index_cache_write_mutex_);
        index_cache_writing_ = false;
        index_cache_write_cv_.notify_all();
    }
}

void Analyzer::queue_shard_write(PendingShardWrite write) const {
    std::lock_guard<std::mutex> lock(index_cache_write_mutex_);
    // Reservation to queue entry in one step: the wait counts both, so the
    // write is never invisible to it.
    if (index_cache_writes_reserved_ > 0)
        --index_cache_writes_reserved_;
    if (index_cache_writer_stop_) {
        index_cache_write_cv_.notify_all();
        return;
    }
    // The queue is the same either way; what differs is who empties it.  With a
    // core to spare a thread takes it; on a one-CPU slice it is drained inline
    // once the burst has published its index -- see drain_shard_writes_inline().
    //
    // The one-CPU case used to write here, on the indexing worker, which put
    // serialization and a write() in the middle of the parse loop the user is
    // waiting on: measured at +21 to +28% on a cold launch against the same
    // build with the cache off.  The work is identical on one core; what
    // changes is that it happens after the index is usable rather than before.
    //
    // Queueing it costs no memory worth counting: the SyntaxIndex held here is
    // the same object extra_cache_ already points at.
    if (!shard_writes_need_inline_drain() && !index_cache_writer_.joinable()) {
        index_cache_writer_ = std::thread([this] {
            // Same courtesy the index workers extend: a cache write is the
            // least urgent thing this process does.
            apply_background_thread_nice(10);
            index_cache_writer_loop();
        });
    }
    index_cache_write_queue_.push_back(std::move(write));
    index_cache_write_cv_.notify_all();
}

void Analyzer::index_cache_writer_loop() const {
    for (;;) {
        PendingShardWrite write;
        {
            std::unique_lock<std::mutex> lock(index_cache_write_mutex_);
            index_cache_write_cv_.wait(lock, [&] {
                return index_cache_writer_stop_ || !index_cache_write_queue_.empty();
            });
            if (index_cache_writer_stop_ && index_cache_write_queue_.empty())
                return;
            write = std::move(index_cache_write_queue_.front());
            index_cache_write_queue_.pop_front();
            index_cache_writing_ = true;
        }

        // A generation bump means the config moved, so this shard would be
        // keyed on defines that are no longer current.  Dropping it costs one
        // reparse next launch; writing it would serve the wrong index.
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            if (write.generation != background_generation_) {
                std::lock_guard<std::mutex> write_lock(index_cache_write_mutex_);
                index_cache_writing_ = false;
                index_cache_write_cv_.notify_all();
                continue;
            }
        }

        if (write.index)
            store_shard_in_cache(write.uri, *write.index, write.include_resolutions,
                                 write.extra_dependency_uri, write.stands_alone);
        if (write.prune_only)
            prune_cache_once_per_generation(write.generation, write.live_uris);

        std::lock_guard<std::mutex> lock(index_cache_write_mutex_);
        index_cache_writing_ = false;
        index_cache_write_cv_.notify_all();
    }
}

void Analyzer::wait_for_index_cache_writes_idle() const {
    const bool inline_drain = shard_writes_need_inline_drain();

    std::unique_lock<std::mutex> lock(index_cache_write_mutex_);
    while (true) {
        index_cache_write_cv_.wait(lock, [&] {
            if (index_cache_writes_reserved_ == 0 && index_cache_write_queue_.empty() &&
                !index_cache_writing_)
                return true;
            // With no writer thread behind the queue, waiting for it to empty
            // would be waiting on this thread to empty it.  Wake and do that.
            return inline_drain && !index_cache_write_queue_.empty();
        });
        if (!inline_drain || index_cache_write_queue_.empty())
            return;
        lock.unlock();
        drain_shard_writes_inline();
        lock.lock();
    }
}

std::optional<IndexCache::Digest> Analyzer::cached_file_digest(const std::string& uri,
                                                               uint64_t generation) const {
    {
        std::lock_guard<std::mutex> lock(index_cache_digest_mutex_);
        if (index_cache_digest_generation_ != generation) {
            index_cache_digests_.clear();
            index_cache_parsed_digests_.clear();
            index_cache_digest_generation_ = generation;
        }
        else if (const auto it = index_cache_digests_.find(uri);
                 it != index_cache_digests_.end()) {
            return it->second;
        }
    }

    // Read and hash with the memo unlocked: two workers racing on the same file
    // both hash it once, which is cheaper than either waiting for the other.
    auto digest = IndexCache::digest_file(path_from_file_uri(uri));

    std::lock_guard<std::mutex> lock(index_cache_digest_mutex_);
    if (index_cache_digest_generation_ == generation)
        index_cache_digests_.insert_or_assign(uri, digest);
    return digest;
}

void Analyzer::remember_parsed_digests(const DocumentState& state, uint64_t generation) const {
    if (state.parsed_digests.empty() && state.parsed_texts.empty())
        return;
    std::lock_guard<std::mutex> lock(index_cache_digest_mutex_);
    if (index_cache_digest_generation_ != generation) {
        index_cache_digests_.clear();
        index_cache_parsed_digests_.clear();
        index_cache_digest_generation_ = generation;
    }
    // First parse of a file wins.  Every parse in a burst reads the same bytes
    // -- that is what the header projection guarantees -- so a later one has
    // nothing to add.
    for (const auto& [uri, digest] : state.parsed_digests)
        index_cache_parsed_digests_.try_emplace(uri, IndexCache::Digest{digest.first,
                                                                        digest.second});
    // Hashed here rather than at the parse, and only when the memo does not
    // already hold the file.  A header every module includes is read once and
    // reached by every parse after it; hashing per parse is the O(files x
    // header) term this memo exists to remove.
    for (const auto& [uri, text] : state.parsed_texts) {
        if (index_cache_parsed_digests_.contains(uri))
            continue;
        index_cache_parsed_digests_.emplace(uri, IndexCache::digest_source_buffer(text));
    }
}

std::optional<IndexCache::Digest> Analyzer::parsed_file_digest(const std::string& uri,
                                                               uint64_t generation) const {
    std::lock_guard<std::mutex> lock(index_cache_digest_mutex_);
    if (index_cache_digest_generation_ != generation)
        return std::nullopt;
    const auto it = index_cache_parsed_digests_.find(uri);
    if (it == index_cache_parsed_digests_.end())
        return std::nullopt;
    return it->second;
}

void Analyzer::store_shard_in_cache(const std::string& uri, const SyntaxIndex& index,
                                    const std::vector<IncludeResolution>& include_resolutions,
                                    const std::string& extra_dependency_uri,
                                    bool stands_alone) const {
    std::optional<IndexCache> cache;
    IndexCache::Digest config_digest;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (!index_cache_)
            return;
        cache = index_cache_;
        config_digest = index_cache_config_digest_;
        generation = background_generation_;
    }

    // Digests come from what the burst's parses read, never from a fresh read
    // of the file.  A shard keyed on bytes other than the ones it was built
    // from is a false hit forever after; see index_cache_parsed_digests_.  A
    // file with no recorded digest is one this burst did not read in full, so
    // there is nothing to key it on and the shard is not written -- one reparse
    // next launch, against a wrong answer on every launch.
    const auto content = parsed_file_digest(uri, generation);
    if (!content)
        return;

    IndexCache::Key key;
    key.content = *content;
    key.config = config_digest;
    key.include_resolutions = include_resolutions;

    auto add_dependency = [&](const std::string& dependency_uri) {
        if (dependency_uri.empty() || dependency_uri == uri)
            return true;
        const auto digest = parsed_file_digest(dependency_uri, generation);
        if (!digest)
            return false;
        key.dependencies.emplace_back(dependency_uri, *digest);
        return true;
    };

    for (const auto& dependency : index.include_dependencies) {
        if (!add_dependency(dependency))
            return;
    }
    if (!add_dependency(extra_dependency_uri))
        return;

    cache->store(uri, key, index, stands_alone);
}

void Analyzer::preload_cached_shards(uint64_t generation) const {
    std::optional<IndexCache> cache;
    IndexCache::Digest config_digest;
    std::vector<std::filesystem::path> include_dirs;
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (!index_cache_ || generation != background_generation_)
            return;
        cache = index_cache_;
        config_digest = index_cache_config_digest_;
        // Copied out with the rest of the burst's inputs: re-running a header
        // search below needs the same directories, in the same order, that a
        // parse of these files would use.
        include_dirs = include_dir_paths_;
        files.assign(background_pending_files_.begin(), background_pending_files_.end());
    }

    // One digest per file for the whole burst, shared with the store path: a
    // header shared by hundreds of modules is hashed once, whether it is being
    // validated on the way in or recorded on the way out.
    const auto digest_of = [&](const std::string& uri) {
        return cached_file_digest(uri, generation);
    };

    // Re-run slang's header search, memoized on what decides it.  A design
    // includes a handful of distinct spellings from a handful of distinct
    // directories, so this collapses to a few stats for the whole burst rather
    // than one search per directive per file.
    //
    // Mirrors SourceManager::readHeader(): an absolute spelling is taken as
    // written; a system include searches only system directories, of which the
    // parse path configures none, so it resolves to nothing; everything else
    // tries the including file's own directory and then the configured include
    // directories in order.
    const auto resolved_uri_if_file = [](const std::filesystem::path& candidate) {
        std::error_code ec;
        return std::filesystem::is_regular_file(candidate, ec) ? uri_from_path(candidate)
                                                               : std::string{};
    };

    // Two memos, not one.  The include directories are searched identically for
    // every file, so their answer depends only on the spelling -- folding them
    // into a key that also carries the including directory recomputes the whole
    // ordered walk once per directory that spells the same header.  On a design
    // with a few dozen include directories that is the dominant cost of the
    // check.  Split, it is one stat per (directory, spelling) plus one walk per
    // distinct spelling.
    //
    // Both are read and written from every preload thread.  A hit is a hash
    // lookup and a miss is one stat, so a single mutex over both costs less
    // than per-thread memos would: the whole burst resolves a handful of
    // distinct spellings, and per-thread copies would turn "one stat per
    // (directory, spelling)" back into one per thread.
    std::mutex                                   resolution_mutex;
    std::unordered_map<std::string, std::string> incdir_resolution;
    std::unordered_map<std::string, std::string> local_resolution;

    const auto resolve_include = [&](const IncludeResolution& recorded) -> std::string {
        std::lock_guard<std::mutex> resolution_lock(resolution_mutex);
        const std::filesystem::path spelling(recorded.spelling);
        if (spelling.is_absolute())
            return resolved_uri_if_file(spelling);
        // System includes search system directories only, and the parse path
        // configures none, so they resolve to nothing.
        if (recorded.is_system)
            return {};

        // The including file's own directory comes first.
        const auto from_directory =
            std::filesystem::path(path_from_file_uri(recorded.from_uri)).parent_path().string();
        if (!from_directory.empty()) {
            auto local_key = from_directory;
            local_key += '\n';
            local_key += recorded.spelling;
            const auto it = local_resolution.find(local_key);
            const auto& local =
                it != local_resolution.end()
                    ? it->second
                    : local_resolution
                          .emplace(std::move(local_key),
                                   resolved_uri_if_file(std::filesystem::path(from_directory) /
                                                        spelling))
                          .first->second;
            if (!local.empty())
                return local;
        }

        if (const auto it = incdir_resolution.find(recorded.spelling);
            it != incdir_resolution.end())
            return it->second;
        std::string resolved;
        for (const auto& directory : include_dirs) {
            resolved = resolved_uri_if_file(directory / spelling);
            if (!resolved.empty())
                break;
        }
        incdir_resolution.emplace(recorded.spelling, resolved);
        return resolved;
    };

    // A shard is usable only when everything it was built from still holds.
    // Content, config, and every `include`d file answer "did what I read
    // change"; the recorded resolutions answer "would I read the same thing",
    // which no digest can -- a header created for the first time, or one added
    // to a directory earlier in the search order, changes no file the key
    // hashes.
    const auto still_valid = [&](const std::string& uri, const IndexCache::Key& key) {
        if (key.config != config_digest)
            return false;
        const auto content = digest_of(uri);
        if (!content || !(*content == key.content))
            return false;
        for (const auto& [dependency_uri, dependency_digest] : key.dependencies) {
            const auto current = digest_of(dependency_uri);
            if (!current || !(*current == dependency_digest))
                return false;
        }
        for (const auto& resolution : key.include_resolutions) {
            if (resolve_include(resolution) != resolution.resolved_uri)
                return false;
        }
        return true;
    };

    struct Hit {
        std::string path;
        std::string uri;
        std::shared_ptr<const SyntaxIndex> index;
        std::vector<std::tuple<std::string, std::shared_ptr<const SyntaxIndex>, bool>> headers;
    };
    struct HeaderHit {
        std::shared_ptr<const SyntaxIndex> index;
        bool stands_alone{false};
    };
    std::mutex                                header_mutex;
    std::unordered_map<std::string, HeaderHit> header_hits;
    std::unordered_set<std::string> header_misses;

    // Checking one file is independent of checking any other: read a shard,
    // hash what it was built from, and decide.  Nothing in it touches analyzer
    // state -- the digest memo has its own lock, and the two maps above are the
    // only other shared things -- so the whole sweep runs on as many threads as
    // the CPU slice allows.
    //
    // It is the warm start's entire cost, and it was serial: one worker ran
    // this while every other one waited on the cache-preload gate.  So a warm
    // launch did not get faster with more cores, it got *slower* -- measured on
    // a 1154-shard project at 99.8 / 114.5 / 128.7 ms for 1 / 2 / 4 CPUs, the
    // extra threads paying wakeups for work they were not allowed to do.
    //
    // Results go into a slot per file rather than a shared list, so the order
    // they are installed in does not depend on which thread finished first.
    // That matters beyond tidiness: installing a hit claims the header shards
    // it carries, first claim winning, so a shared list would make which
    // includer claims a shared header vary run to run.
    std::vector<std::optional<Hit>> results(files.size());

    const auto check_one = [&](size_t index) {
        const auto& path = files[index];
        const auto  uri  = uri_from_path(path);
        auto        loaded = cache->load(uri);
        if (!loaded || !still_valid(uri, loaded->key))
            return;

        // A file's shard is only usable together with shards for the headers it
        // pulled in: skipping its parse skips the only thing that would have
        // built them.  If any header shard is missing or stale, this file has to
        // be parsed after all -- that parse is what produces them.
        Hit hit{.path = path, .uri = uri};
        bool headers_ok = true;
        for (const auto& dependency : loaded->index.include_dependencies) {
            {
                std::lock_guard<std::mutex> header_lock(header_mutex);
                if (header_misses.count(dependency)) {
                    headers_ok = false;
                    break;
                }
                if (const auto known = header_hits.find(dependency); known != header_hits.end()) {
                    hit.headers.emplace_back(dependency, known->second.index,
                                             known->second.stands_alone);
                    continue;
                }
            }
            // Read and validate with the map unlocked.  Two threads reaching
            // the same header both do the work, which is cheaper than either
            // waiting for the other -- the same trade cached_file_digest()
            // makes -- and the insert below keeps whichever arrives first, so
            // every includer still ends up pointing at one shared index.
            auto header = cache->load(dependency);
            const bool usable = header && still_valid(dependency, header->key);

            std::lock_guard<std::mutex> header_lock(header_mutex);
            if (!usable) {
                header_misses.insert(dependency);
                headers_ok = false;
                break;
            }
            auto [entry, inserted] = header_hits.try_emplace(
                dependency,
                HeaderHit{std::make_shared<const SyntaxIndex>(std::move(header->index)),
                          header->stands_alone});
            hit.headers.emplace_back(dependency, entry->second.index, entry->second.stands_alone);
        }
        if (!headers_ok)
            return;

        hit.index      = std::make_shared<const SyntaxIndex>(std::move(loaded->index));
        results[index] = std::move(hit);
    };

    const size_t worker_count =
        std::min<size_t>(std::max<size_t>(available_cpu_count(), 1), files.size());
    if (worker_count <= 1) {
        for (size_t i = 0; i < files.size(); ++i)
            check_one(i);
    } else {
        // Hand out indices one at a time rather than in blocks: a shard's cost
        // tracks the size of the file it indexes, and a filelist is not sorted
        // by size, so a static split strands one thread on the generated
        // register blocks while the rest finish.
        std::atomic<size_t>      next{0};
        std::vector<std::thread> workers;
        workers.reserve(worker_count - 1);
        const auto drain = [&] {
            for (size_t i = next.fetch_add(1, std::memory_order_relaxed); i < files.size();
                 i = next.fetch_add(1, std::memory_order_relaxed))
                check_one(i);
        };
        for (size_t i = 0; i + 1 < worker_count; ++i)
            workers.emplace_back(drain);
        drain(); // this thread takes a share too
        for (auto& worker : workers)
            worker.join();
    }

    std::vector<Hit> hits;
    hits.reserve(results.size());
    for (auto& result : results)
        if (result)
            hits.push_back(std::move(*result));

    // Queued whatever the preload found.  The sweep has to happen on the launch
    // that reuses everything just as much as on one that rebuilds, and that
    // launch writes no shards for it to hang off.
    //
    // Everything above was just proved to be on disk, so the sweep is told and
    // skips those shards by name.  On an unchanged project that is all of them,
    // which turns a read of every shard in the directory into a set lookup --
    // the difference is most of a warm start on a one-CPU slice, where the
    // sweep has no second core to run on.
    std::unordered_set<std::string> live_uris;
    live_uris.reserve(files.size() + header_hits.size());
    for (const auto& path : files)
        live_uris.insert(uri_from_path(path));
    for (const auto& [header_uri, header_hit] : header_hits)
        live_uris.insert(header_uri);

    reserve_shard_writes(1);
    queue_shard_write(PendingShardWrite{.prune_only = true,
                                        .generation = generation,
                                        .live_uris = std::move(live_uris)});

    if (hits.empty())
        return;

    std::lock_guard<std::mutex> lock(map_mutex_);
    if (generation != background_generation_)
        return;

    size_t installed = 0;
    for (auto& hit : hits) {
        // An open buffer is newer than anything on disk, exactly as in the
        // parse path: never let a cached shard replace one.
        if (const auto doc = docs_.find(hit.uri); doc != docs_.end() && doc->second)
            continue;
        extra_cache_[hit.uri] = ExtraFileCacheEntry{
            .path = hit.path,
            .uri = hit.uri,
            .index = hit.index,
        };
        for (auto& [header_uri, header_index, stands_alone] : hit.headers) {
            // Claimed as well as committed.  The claim is what stops a worker
            // that parses some other includer from rebuilding a header this
            // pass already has.
            if (!background_header_claims_.insert(header_uri).second)
                continue;
            // Restored too: it is what lets an open buffer be served this
            // header's directives alone instead of re-reading it per keystroke.
            if (stands_alone)
                standalone_header_uris_.insert(header_uri);
            background_header_shards_[header_uri] = ExtraFileCacheEntry{
                .path = path_from_file_uri(header_uri),
                .uri = header_uri,
                .index = header_index,
            };
        }
        background_pending_set_.erase(hit.path);
        ++installed;
    }

    if (installed == 0)
        return;

    // Rebuild the queue from what is left rather than erasing from the middle
    // of a deque once per hit.
    std::deque<std::string> remaining;
    for (auto& path : background_pending_files_) {
        if (background_pending_set_.count(path))
            remaining.push_back(std::move(path));
    }
    background_pending_files_ = std::move(remaining);
    invalidate_extra_snapshots_locked();

    // The publish is otherwise requested only when a worker finishes parsing a
    // file, and a project that is entirely unchanged has no such worker: every
    // file was installed from disk.  Without this, a fully cached start-up
    // produces no ProjectIndexSnapshot at all -- shards loaded, and nothing
    // able to see them.
    schedule_background_project_publish_locked();
}

void Analyzer::refresh_changed_extra_files(const std::vector<std::string>& changed_uris,
                                           const std::vector<std::string>& deleted_uris) {
    auto normalized_project_path = [](std::string uri) -> std::string {
        if (uri.starts_with("file://"))
            uri = path_from_file_uri(uri);
        if (uri.empty())
            return {};
        return normalize_filesystem_path(uri).string();
    };

    std::vector<std::string> deleted_paths;
    deleted_paths.reserve(deleted_uris.size());
    for (const auto& uri : deleted_uris)
        deleted_paths.push_back(normalized_project_path(uri));

    std::vector<std::string> changed_paths;
    changed_paths.reserve(changed_uris.size());
    for (const auto& uri : changed_uris)
        changed_paths.push_back(normalized_project_path(uri));

    // Open-buffer header text is kept across keystrokes with no revalidation, so
    // this notification is the only thing that can make it stale.  Drop it before
    // taking map_mutex_: OpenParseHeaderCache has its own mutex and the same rule
    // as HeaderTextCache — never take it under map_mutex_.
    for (const auto& path : deleted_paths)
        open_parse_header_texts_.invalidate(path);
    for (const auto& path : changed_paths)
        open_parse_header_texts_.invalidate(path);

    std::lock_guard<std::mutex> lock(map_mutex_);

    bool queued_changed_file = false;
    bool removed_deleted_file = false;
    std::unordered_set<std::string> seen_changed_paths;

    for (const auto& path : deleted_paths) {
        if (path.empty() || !extra_file_set_.contains(path))
            continue;
        extra_cache_.erase(uri_from_path(path));
        invalidate_extra_snapshots_locked();
        removed_deleted_file = true;
    }

    for (const auto& path : changed_paths) {
        if (path.empty() || !seen_changed_paths.insert(path).second)
            continue;

        if (extra_file_set_.contains(path)) {
            // Queue only the explicitly reported file.  This is the important HPC
            // property: a rename/workspace-edit notification does not trigger a
            // whole-design rescan and does not perform metadata checks for every
            // filelist entry.  The background worker will parse disk contents for a
            // closed file, or use the live DocumentState if the file is open.
            queue_background_file_locked(path, /*front=*/true);
            queued_changed_file = true;
            continue;
        }

        // A shared header is usually not a filelist entry, so the branch above
        // never queues it.  Its declarations reach the project index only
        // through the shard background_index_loop() builds for it, and that
        // shard is claimed once per generation: without dropping the claim the
        // header keeps serving whatever it contained when the claim was taken,
        // for the rest of the session.
        const auto header_uri = uri_from_path(path);
        const bool was_indexed_header = background_header_claims_.erase(header_uri) > 0;
        background_header_shards_.erase(header_uri);
        standalone_header_uris_.erase(header_uri);
        if (!was_indexed_header)
            continue;
        invalidate_extra_snapshots_locked();

        // Nothing queues a header directly.  Re-queue the files that `include`
        // it instead; the first one to commit re-claims the header and rebuilds
        // its shard from that file's fresh tree.
        for (const auto& [entry_uri, entry] : extra_cache_) {
            if (!entry.index)
                continue;
            const auto& deps = entry.index->include_dependencies;
            if (std::find(deps.begin(), deps.end(), header_uri) == deps.end())
                continue;
            queue_background_file_locked(entry.path, /*front=*/true);
            queued_changed_file = true;
        }
    }

    if (!queued_changed_file && !removed_deleted_file)
        return;

    // Invalidate any in-flight parse that started before the client reported
    // these edits.  Pending unrelated files remain queued and will parse under
    // the new generation when the worker reaches them.
    ++background_generation_;

    if (removed_deleted_file)
        schedule_background_project_publish_locked();
    if (queued_changed_file) {
        start_background_indexer_locked();
        background_cv_.notify_all();
    }
}

void Analyzer::wait_for_background_index_idle() const {
    std::unique_lock<std::mutex> lock(map_mutex_);
    background_cv_.wait(lock, [&] {
        return background_pending_files_.empty() && background_index_active_ == 0 &&
               !background_publish_requested_;
    });
}

void Analyzer::invalidate_extra_snapshots_locked() const {
    // Both snapshot vectors summarize extra_cache_.  The ExtraFileInfo variant
    // also records which project files are currently open by consulting docs_,
    // so document open/change/close paths must invalidate these caches too.
    // This helper is intentionally tiny and must only be called while
    // map_mutex_ is held by the mutating path.
    extra_file_snapshot_cache_.reset();
    extra_index_snapshot_cache_.reset();
}

std::shared_ptr<const std::vector<ExtraFileInfo>>
Analyzer::build_extra_file_snapshot_locked() const {
    auto result = std::make_shared<std::vector<ExtraFileInfo>>();
    result->reserve(extra_cache_.size() + background_header_shards_.size());
    // Header shards belong here for the same reason they belong in the index
    // snapshot: a file's shard no longer carries what it `include`d, and an open
    // buffer past kDirectivesOnlySeedBytes does not carry it either.  Without
    // this, definition and every other feature reading this snapshot could only
    // find a header's declarations through some includer that happened to be
    // parsed before the projection was installed.
    const auto append = [&](const ExtraFileCacheEntry& entry) {
        // Closed project files intentionally have no DocumentState here.  They
        // are represented only by the compact SyntaxIndex shard.  If the file
        // is open, attach the live state so AST-only features can inspect
        // unsaved text without keeping closed project ASTs alive.
        //
        // Do not rebuild the open file's dynamic index here.  Open filelist
        // entries replace their shard in update_extra_cache_for_live_state_locked()
        // on didOpen/didChange, so entry.index is already the live-buffer shard.
        // Rebuilding while map_mutex_ is held would make hover/definition/RTL
        // requests serialize behind AST-derived indexing work.
        if (const auto it = docs_.find(entry.uri); it != docs_.end() && it->second) {
            result->push_back(ExtraFileInfo{
                .path = entry.path,
                .uri = entry.uri,
                .state = it->second,
                .index = entry.index,
            });
            return;
        }
        result->push_back(ExtraFileInfo{
            .path = entry.path,
            .uri = entry.uri,
            .state = nullptr,
            .index = entry.index,
        });
    };
    for (const auto& [key, entry] : extra_cache_)
        append(entry);
    for (const auto& [key, entry] : background_header_shards_) {
        // A header that is also a filelist entry is already above; appending it
        // twice would give every by-name lookup two candidates for one file.
        if (!extra_cache_.contains(key))
            append(entry);
    }
    return result;
}

std::shared_ptr<const std::vector<ExtraIndexInfo>>
Analyzer::build_extra_index_snapshot_locked() const {
    auto result = std::make_shared<std::vector<ExtraIndexInfo>>();
    result->reserve(extra_cache_.size() + background_header_shards_.size());
    const auto append = [&result](const ExtraFileCacheEntry& entry) {
        result->push_back(ExtraIndexInfo{
            .path = entry.path,
            .uri = entry.uri,
            .index = entry.index,
        });
    };
    for (const auto& [key, entry] : extra_cache_)
        append(entry);
    // A file's shard no longer carries what it `include`d, so header shards have
    // to appear here too or header declarations become invisible to every
    // feature reading this snapshot.
    for (const auto& [key, entry] : background_header_shards_)
        append(entry);
    return result;
}

std::shared_ptr<const std::vector<ExtraFileInfo>>
Analyzer::extra_file_snapshot_ptr() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);
    // Do not poll the .f file mtime on request paths.  The filelist is treated
    // as configuration loaded at startup / config reload.  This avoids metadata
    // I/O in HPC environments and keeps edits to listed open buffers
    // incremental through update_extra_cache_for_live_state_locked().
    // Request paths must remain read-only with respect to parsing.  If the
    // cache is still warming or some configured files failed to parse, return
    // the shards currently available instead of synchronously parsing missing
    // files under map_mutex_.
    if (!extra_file_snapshot_cache_)
        extra_file_snapshot_cache_ = build_extra_file_snapshot_locked();
    log_perf("extra_file_snapshot_ptr files=" + std::to_string(extra_file_snapshot_cache_->size()), start);
    return extra_file_snapshot_cache_;
}

std::shared_ptr<const std::vector<ExtraIndexInfo>>
Analyzer::extra_index_snapshot_ptr() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);
    if (!extra_index_snapshot_cache_)
        extra_index_snapshot_cache_ = build_extra_index_snapshot_locked();
    log_perf("extra_index_snapshot_ptr files=" + std::to_string(extra_index_snapshot_cache_->size()), start);
    return extra_index_snapshot_cache_;
}

std::shared_ptr<const ProjectIndexSnapshot> Analyzer::project_index_snapshot() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);

    if (!project_index_snapshot_cache_)
        project_index_snapshot_cache_ = std::make_shared<ProjectIndexSnapshot>();
    log_perf("project_index_snapshot shards=" + std::to_string(project_index_snapshot_cache_->shards.size()), start);
    return project_index_snapshot_cache_;
}

void Analyzer::set_project_index_publish_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    project_index_publish_callback_ = std::move(callback);
}

std::shared_ptr<const std::vector<OpenIndexShard>>
Analyzer::opened_file_index_shards(const std::string& current_uri) const {
    std::vector<std::shared_ptr<const DocumentState>> states;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        states.reserve(docs_.size());
        for (const auto& [uri, state] : docs_) {
            if (uri == current_uri || !state)
                continue;
            states.push_back(state);
        }
    }

    // Build the per-file dynamic indexes outside map_mutex_.  get_dynamic_index()
    // may lazily walk the immutable DocumentState AST the first time a shard is
    // requested, but it returns a cache owned by that same DocumentState.  The
    // OpenIndexShard keeps the state alive so the raw index pointer remains
    // valid even if didChange concurrently swaps docs_[uri] to a newer state.
    auto result = std::make_shared<std::vector<OpenIndexShard>>();
    result->reserve(states.size());
    for (const auto& state : states) {
        if (!state)
            continue;
        result->push_back(OpenIndexShard{
            .uri = state->uri,
            .state = state,
            .index = &get_dynamic_index(*state),
        });
    }
    return result;
}

CompilationSnapshot Analyzer::compilation_snapshot() const {
    std::lock_guard<std::mutex> lock(map_mutex_);

    CompilationSnapshot snapshot;
    snapshot.defines = defines_;
    snapshot.include_dirs = include_dirs_;

    std::unordered_set<std::string> seen_uris;
    std::unordered_set<std::string> seen_paths;

    for (const auto& [uri, state] : docs_) {
        if (!state)
            continue;

        snapshot.files.push_back(CompilationSourceFile{
            .uri = uri,
            .path = state->normalized_path,
            .text = std::shared_ptr<const std::string>(state, &state->text),
        });
        snapshot.open_uris.push_back(uri);
        seen_uris.insert(uri);
        seen_paths.insert(state->normalized_path);
        if (const auto it = latest_version_.find(uri); it != latest_version_.end())
            snapshot.uri_versions[uri] = it->second;
    }

    for (const auto& path_string : extra_files_) {
        // extra_files_ is normalized at the configuration boundary by
        // set_extra_files() / set_project_config().  compilation_snapshot() is
        // called by the background compiler while holding map_mutex_, so avoid
        // repeating filesystem path normalization for every filelist entry here.
        // Keeping this path-to-URI conversion allocation-only prevents large
        // project snapshots from extending the analyzer critical section with
        // redundant per-file path work.
        const auto uri = uri_from_path(path_string);
        if (seen_uris.contains(uri) || seen_paths.contains(path_string))
            continue;

        snapshot.files.push_back(CompilationSourceFile{
            .uri = uri,
            .path = path_string,
            .text = nullptr,
        });
        seen_uris.insert(uri);
        seen_paths.insert(path_string);
    }

    return snapshot;
}

void Analyzer::set_semantic_diagnostics(
    std::unordered_map<std::string, std::vector<ParseDiagInfo>> diagnostics,
    const std::unordered_map<std::string, uint64_t>& snapshot_versions) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    for (auto& [uri, diags] : diagnostics) {
        // Check version only for open buffers that have a tracked version.
        // Closed/filelist-only files have no entry in latest_version_ and
        // no entry in snapshot_versions — commit their diagnostics unconditionally.
        const auto snap_it = snapshot_versions.find(uri);
        const auto cur_it = latest_version_.find(uri);
        const bool is_open_buffer = (snap_it != snapshot_versions.end());
        if (is_open_buffer && cur_it != latest_version_.end()
            && snap_it->second != cur_it->second)
            continue;  // open buffer was edited since snapshot — stale
        semantic_diagnostics_[uri] = VersionedSemanticDiags{
            .version = (cur_it != latest_version_.end()) ? cur_it->second : 0,
            .diags = std::move(diags),
        };
    }
}

void Analyzer::clear_semantic_diagnostics(const std::string& uri) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    semantic_diagnostics_.erase(uri);
}

void Analyzer::clear_all_semantic_diagnostics() {
    std::lock_guard<std::mutex> lock(map_mutex_);
    semantic_diagnostics_.clear();
}

std::vector<ParseDiagInfo> Analyzer::semantic_diagnostics(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    const auto it = semantic_diagnostics_.find(uri);
    if (it == semantic_diagnostics_.end())
        return {};
    // Mirror set_semantic_diagnostics(): only open buffers are version-tracked.
    // Closed/filelist-only files (and files opened via the synchronous CLI
    // Analyzer::open() path, which does not register a version) have no
    // latest_version_ entry — publish their diagnostics unconditionally rather
    // than treating "untracked" the same as "stale".
    const auto cur_it = latest_version_.find(uri);
    if (cur_it != latest_version_.end() && it->second.version != cur_it->second)
        return {};  // stale cache entry — don't publish
    return it->second.diags;
}

std::vector<std::string> Analyzer::semantic_diagnostic_uris() const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    std::vector<std::string> uris;
    uris.reserve(semantic_diagnostics_.size());
    for (const auto& [uri, _] : semantic_diagnostics_)
        uris.push_back(uri);
    return uris;
}

namespace {

struct RtlIndexedInstance {
    InstanceEntry entry;
    std::string uri;
};

struct RtlModuleLocation {
    std::string uri;
    int line{0};
    int col{0};
};

struct RtlIndexView {
    std::unordered_map<std::string, RtlModuleLocation> modules;
    std::vector<RtlIndexedInstance> instances;
    // parent module name -> indexes into `instances`.
    //
    // Forward RTL tree construction asks the same question at every hierarchy
    // node: "which instances are declared directly inside this module?"  The
    // old implementation answered that by scanning every indexed instance for
    // every visited module, which made tree building O(visited modules × total
    // instances).  Keep the instance vector as the canonical per-request
    // storage (reverse tree construction still benefits from a flat list), and
    // build this small adjacency table once after all open/project shards have
    // been merged into the request-local view.
    std::unordered_map<std::string, std::vector<size_t>> instances_by_parent;
};

void add_rtl_index_file(RtlIndexView& view, std::unordered_set<std::string>& seen_uris,
                        const std::string& uri, const SyntaxIndex& index) {
    if (!seen_uris.insert(uri).second)
        return;

    for (const auto& module : index.modules) {
        // First definition wins to preserve the historical RTL tree behavior
        // where duplicate module names resolve to the first indexed file.  Keep
        // the source location next to the URI so editor clients can jump to the
        // definition line without issuing a second definition request.
        view.modules.try_emplace(module.name, RtlModuleLocation{
            .uri = uri,
            .line = module.line,
            .col = module.col,
        });
    }

    for (const auto& instance : index.instances)
        view.instances.push_back(RtlIndexedInstance{.entry = instance, .uri = uri});
}

void build_rtl_parent_adjacency(RtlIndexView& view) {
    view.instances_by_parent.clear();

    for (size_t i = 0; i < view.instances.size(); ++i) {
        const auto& parent_module = view.instances[i].entry.parent_module;
        if (parent_module.empty())
            continue;
        view.instances_by_parent[parent_module].push_back(i);
    }
}

} // namespace

std::optional<RtlTreeNode> Analyzer::rtl_tree(const std::string& uri) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;
    // By reference, for the reason above.
    const auto& state_index = get_structural_index(*state);
    if (state_index.modules.empty())
        return std::nullopt;

    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> open_states;
    std::shared_ptr<const std::vector<ExtraFileInfo>> extra_snapshot;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        open_states.reserve(docs_.size());
        for (const auto& [state_uri, state_snapshot] : docs_)
            open_states.emplace_back(state_uri, state_snapshot);
        if (!extra_file_snapshot_cache_)
            extra_file_snapshot_cache_ = build_extra_file_snapshot_locked();
        extra_snapshot = extra_file_snapshot_cache_;
    }

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for (const auto& [state_uri, state_snapshot] : open_states) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, get_structural_index(*state_snapshot));
    }
    for (const auto& extra : *extra_snapshot)
        add_rtl_index_file(view, seen_uris, extra.uri, extra.index_ref());
    build_rtl_parent_adjacency(view);

    const auto* root = &state_index.modules.front();
    for (const auto& module : state_index.modules) {
        if (module.line > 0 && (root->line <= 0 || module.line < root->line))
            root = &module;
    }

    std::function<RtlTreeNode(const std::string&, size_t, std::unordered_set<std::string>&)> build =
        [&](const std::string& module_name, size_t depth,
            std::unordered_set<std::string>& seen) -> RtlTreeNode {
        auto module_it = view.modules.find(module_name);
        RtlTreeNode node{
            .name = module_name,
            .inst = {},
            .file = module_it != view.modules.end() ? module_it->second.uri : std::string{},
            .line = module_it != view.modules.end() ? module_it->second.line : 0,
            .col = module_it != view.modules.end() ? module_it->second.col : 0,
            .children = {},
            .recursive = seen.contains(module_name),
        };
        if (node.recursive || module_it == view.modules.end())
            return node;
        if (depth >= kMaxRtlTreeDepth) {
            node.truncated = true;
            return node;
        }

        seen.insert(module_name);
        const auto children = view.instances_by_parent.find(module_name);
        if (children == view.instances_by_parent.end()) {
            seen.erase(module_name);
            return node;
        }
        for (const size_t instance_index : children->second) {
            const auto& inst = view.instances[instance_index];
            if (inst.entry.module_name == module_name)
                continue;
            auto child = build(inst.entry.module_name, depth + 1, seen);
            child.inst = inst.entry.instance_name;
            node.children.push_back(std::move(child));
        }
        seen.erase(module_name);
        return node;
    };

    std::unordered_set<std::string> seen;
    return build(root->name, 0, seen);
}

std::optional<RtlTreeNode> Analyzer::rtl_tree_reverse(const std::string& uri) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;
    // By reference, for the reason above.
    const auto& state_index = get_structural_index(*state);
    if (state_index.modules.empty())
        return std::nullopt;

    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> open_states;
    std::shared_ptr<const std::vector<ExtraFileInfo>> extra_snapshot;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        open_states.reserve(docs_.size());
        for (const auto& [state_uri, state_snapshot] : docs_)
            open_states.emplace_back(state_uri, state_snapshot);
        if (!extra_file_snapshot_cache_)
            extra_file_snapshot_cache_ = build_extra_file_snapshot_locked();
        extra_snapshot = extra_file_snapshot_cache_;
    }

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for (const auto& [state_uri, state_snapshot] : open_states) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, get_structural_index(*state_snapshot));
    }
    for (const auto& extra : *extra_snapshot)
        add_rtl_index_file(view, seen_uris, extra.uri, extra.index_ref());

    const auto* target = &state_index.modules.front();
    for (const auto& module : state_index.modules) {
        if (module.line > 0 && (target->line <= 0 || module.line < target->line))
            target = &module;
    }

    struct ParentRef {
        std::string parent_module;
        std::string inst_name;
        std::string file_uri;
    };
    std::unordered_map<std::string, std::vector<ParentRef>> reverse_map;
    for (const auto& inst : view.instances) {
        if (inst.entry.parent_module.empty())
            continue;
        reverse_map[inst.entry.module_name].push_back(ParentRef{
            .parent_module = inst.entry.parent_module,
            .inst_name = inst.entry.instance_name,
            .file_uri = inst.uri,
        });
    }

    std::function<RtlTreeNode(const std::string&, size_t, std::unordered_set<std::string>&)> build =
        [&](const std::string& module_name, size_t depth,
            std::unordered_set<std::string>& seen) -> RtlTreeNode {
        auto module_it = view.modules.find(module_name);
        RtlTreeNode node{
            .name = module_name,
            .inst = {},
            .file = module_it != view.modules.end() ? module_it->second.uri : std::string{},
            .line = module_it != view.modules.end() ? module_it->second.line : 0,
            .col = module_it != view.modules.end() ? module_it->second.col : 0,
            .children = {},
            .recursive = seen.contains(module_name),
        };
        if (node.recursive)
            return node;
        if (depth >= kMaxRtlTreeDepth) {
            node.truncated = true;
            return node;
        }

        seen.insert(module_name);
        auto refs = reverse_map.find(module_name);
        if (refs == reverse_map.end()) {
            seen.erase(module_name);
            return node;
        }

        for (const auto& parent : refs->second) {
            auto child = build(parent.parent_module, depth + 1, seen);
            child.inst = parent.inst_name;
            if (child.file.empty())
                child.file = parent.file_uri;
            node.children.push_back(std::move(child));
        }
        seen.erase(module_name);
        return node;
    };

    std::unordered_set<std::string> seen;
    return build(target->name, 0, seen);
}

bool Analyzer::queue_include_dependents_locked(const std::string& uri) const {
    bool queued = false;
    for (const auto& [other_uri, other_state] : docs_) {
        if (other_uri == uri || !other_state ||
            !other_state->include_dependency_set.contains(uri))
            continue;
        // Indirect include fanout belongs on the background path.  A common
        // header can be included by many open files; reparsing all of them
        // synchronously would violate the current-file AST / background-project-
        // index split and can lag badly on shared HPC filesystems.  The worker
        // reparses live open buffers from their in-memory text and open include
        // overlays.
        queue_background_file_locked(other_state->normalized_path, /*front=*/true);
        queued = true;
    }
    // Closed project files get one requeue between them, not one each.
    //
    // A header's declarations live in the header's own shard, and that shard is
    // rebuilt by whichever file claims the header — so a single includer is
    // enough to refresh what the project index answers about this header, from
    // the unsaved buffer via the open overlays.  Reparsing all of them instead
    // costs one parse per includer, hundreds for a common header, on every
    // typing burst; and saving asks for that same full fanout again through
    // refresh_changed_extra_files(), against the text that actually reached
    // disk.  A closed file's own shard is index-authoritative from disk anyway,
    // so what it loses here is a copy of header declarations the header's shard
    // already holds.
    //
    // Dropping the claim is what makes the requeue rebuild the header rather
    // than serve the shard claimed under the previous text.  The old shard stays
    // in place until the new one commits, so the project index never has a gap.
    background_header_claims_.erase(uri);
    standalone_header_uris_.erase(uri);
    for (const auto& [extra_uri, entry] : extra_cache_) {
        // Test the shard's dependency list in place.  Offering a
        // `std::vector<std::string>{}` fallback made the conditional expression
        // a prvalue, so every shard's list was deep-copied on every edit, under
        // map_mutex_.
        if (docs_.contains(extra_uri) || !entry.index)
            continue;
        const auto& deps = entry.index->include_dependencies;
        if (std::find(deps.begin(), deps.end(), uri) == deps.end())
            continue;
        queue_background_file_locked(entry.path, /*front=*/true);
        queued = true;
        break;
    }
    return queued;
}

void Analyzer::queue_background_file_locked(std::string path, bool front) const {
    if (!background_pending_set_.insert(path).second)
        return;
    if (front)
        background_pending_files_.push_front(std::move(path));
    else
        background_pending_files_.push_back(std::move(path));
}

void Analyzer::start_background_indexer_locked() const {
    // Project indexing is CPU-bound and embarrassingly parallel: every
    // configured file is parsed and indexed from its own SourceManager, and an
    // include-heavy design re-preprocesses the same headers once per file.  A
    // single worker made a cold start scale linearly with the filelist, so fan
    // the queue out over a small pool.
    //
    // available_cpu_count() reports the slice this process may actually use
    // rather than the size of the machine, which matters on batch-scheduled and
    // containerised nodes.  It is sampled once: re-reading it per schedule would
    // make the pool size depend on when indexing happened to be triggered.
    static const unsigned cpu_budget =
        std::clamp(available_cpu_count(), 1u, kMaxBackgroundIndexThreads);

    // Never spawn more workers than there is queued work.  Closing one buffer
    // queues a single reparse and should not start a whole pool.  The pool only
    // ever grows, so a later full reindex still reaches the CPU budget.
    const auto queued = std::max<size_t>(background_pending_files_.size(), 1);
    const auto desired = static_cast<size_t>(std::min<size_t>(cpu_budget, queued));
    if (background_indexers_.size() >= desired)
        return;

    background_indexers_.reserve(desired);
    while (background_indexers_.size() < desired) {
        background_indexers_.emplace_back([this] {
            apply_background_thread_nice(kBackgroundIndexNiceValue);
            background_index_loop();
        });
    }
}

void Analyzer::schedule_background_reindex_locked() const {
    // A new generation invalidates parse results from older define/include/file
    // configurations.  The worker checks the generation again just before
    // committing each shard, so a slow parse can never overwrite newer project
    // state.
    ++background_generation_;
    // Include directories may have moved, so a path the previous parse resolved
    // an `include to is no longer evidence of what the same directive resolves
    // to now.  Size and mtime cannot catch that; only dropping the cache can.
    open_parse_header_texts_.clear();
    background_pending_files_.clear();
    background_pending_set_.clear();
    // Header shards belong to the generation that built them.  A changed header,
    // a changed define set and a changed filelist all land here, so dropping
    // both maps is what guarantees a header is re-indexed rather than served
    // from the previous configuration.
    background_header_shards_.clear();
    background_header_claims_.clear();
    standalone_header_uris_.clear();
    for (const auto& path : extra_files_)
        queue_background_file_locked(path, /*front=*/false);
    start_background_indexer_locked();
    background_cv_.notify_all();
}

void Analyzer::schedule_background_project_publish_locked() const {
    background_publish_requested_ = true;
    background_publish_due_time_ = Clock::now() +
        std::chrono::milliseconds(std::max(0, background_publish_debounce_ms_));
    start_background_indexer_locked();
    background_cv_.notify_all();
}

void Analyzer::background_index_loop() const {
    // Hold the parse config across files instead of copying both vectors under
    // map_mutex_ once per filelist entry.  Every writer of defines_ /
    // include_dirs_ bumps the background generation before any later work can
    // be queued, so a matching generation means the copy is still current.
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> include_dirs;
    uint64_t config_generation = std::numeric_limits<uint64_t>::max();

    while (!background_stop_.load()) {
        std::string path_string;
        std::string uri;
        std::vector<OpenTextOverlay> open_overlays;
        std::shared_ptr<const DocumentState> live_doc;
        uint64_t generation = 0;
        // Whether this worker took the burst's warmup file; see
        // background_warmup_generation_.
        bool warmup_owner = false;

        {
            std::unique_lock<std::mutex> lock(map_mutex_);
            background_cv_.wait(lock, [&] {
                return background_stop_.load() || !background_pending_files_.empty() ||
                       background_publish_requested_;
            });
            if (background_stop_.load())
                break;

            if (background_publish_requested_ && background_pending_files_.empty()) {
                const auto now = Clock::now();
                if (background_publish_due_time_ > now) {
                    background_cv_.wait_until(lock, background_publish_due_time_, [&] {
                        return background_stop_.load() || !background_pending_files_.empty() ||
                               background_publish_due_time_ <= Clock::now();
                    });
                    continue;
                }

                background_publish_requested_ = false;
                auto publish_callback = publish_project_index_snapshot_locked();
                background_cv_.notify_all();
                lock.unlock();
                // The queue has drained, so this burst is over.  Dropping the
                // header text here keeps the cache from outliving the work it
                // was collected for: the next burst re-reads whatever it needs
                // and therefore always sees current disk contents.  Done after
                // unlocking because the cache has its own mutex and must never
                // be taken under map_mutex_.
                background_header_texts_.clear();
                if (publish_callback)
                    publish_callback();
                // The burst is over and its index is published, so the shard
                // writes it produced are no longer in anybody's way.  On a
                // one-CPU slice this thread is the only one there is to do
                // them; with a core to spare the writer thread already has.
                if (shard_writes_need_inline_drain())
                    drain_shard_writes_inline();
                continue;
            }

            // Warmup gate: one worker takes the first file of a burst alone, so
            // whatever it `include`s has a shard and a directives-only
            // projection before the rest of the queue is released.  Without it
            // the number of files that re-parse the whole header is whatever the
            // scheduler allows -- measured on a 4-core box as 1 file on a
            // single-core slice, 9-12 idle and 35 of 60 under load.
            //
            // The cost is bounded at exactly one file: a worker never waits for
            // a second one, so a project whose first file `include`s nothing
            // pays one file's parse of lost parallelism and no more.  On a
            // single-worker slice -- the HPC target -- there is nothing to gate
            // and the wait is never entered.
            // Cache-preload gate.  One worker asks the on-disk cache which
            // files are unchanged and installs their shards; the rest wait,
            // because a worker that starts parsing a file the preload was about
            // to satisfy has already spent what the cache exists to save.
            if (index_cache_ && background_preload_generation_ != background_generation_) {
                if (background_preload_running_) {
                    background_cv_.wait(lock, [&] {
                        return background_stop_.load() || !background_preload_running_ ||
                               background_preload_generation_ == background_generation_;
                    });
                    continue;
                }
                background_preload_running_ = true;
                const auto preload_generation = background_generation_;
                lock.unlock();
                preload_cached_shards(preload_generation);
                lock.lock();
                background_preload_running_ = false;
                background_preload_generation_ = preload_generation;
                background_cv_.notify_all();
                // Back to the top: the preload may have emptied the queue
                // outright, which is the whole point on an unchanged project.
                continue;
            }

            if (background_warmup_generation_ != background_generation_) {
                if (background_warmup_running_) {
                    background_cv_.wait(lock, [&] {
                        return background_stop_.load() || !background_warmup_running_ ||
                               background_warmup_generation_ == background_generation_;
                    });
                    continue;
                }
                background_warmup_running_ = true;
                warmup_owner = true;
            }

            path_string = std::move(background_pending_files_.front());
            background_pending_files_.pop_front();
            // Drop membership before parsing, not after committing: an edit that
            // lands while this parse is in flight must be able to re-queue the
            // file rather than be swallowed as a duplicate.
            background_pending_set_.erase(path_string);
            ++background_index_active_;
            const auto path = normalize_filesystem_path(path_string);
            path_string = path.string();
            uri = uri_from_path(path);
            generation = background_generation_;
            if (config_generation != generation) {
                defines = defines_;
                include_dirs = include_dir_paths_;
                config_generation = generation;
            }
            open_overlays.reserve(docs_.size());
            for (const auto& [open_uri, open_state] : docs_) {
                if (!open_state || open_uri == uri)
                    continue;
                if (open_state->normalized_path == path_string)
                    continue;
                open_overlays.push_back(OpenTextOverlay{
                    .uri = open_uri,
                    .path = open_state->normalized_path,
                    .state = open_state,
                });
            }

            // Open buffers are already parsed from unsaved text by didOpen /
            // didChange.  Avoid reparsing stale disk contents, but also avoid
            // building the dynamic shard while map_mutex_ is held: that AST walk
            // can be noticeable for large RTL files and would otherwise block
            // unrelated request handlers.
            if (const auto doc = docs_.find(uri); doc != docs_.end() && doc->second) {
                live_doc = doc->second;
            }
        }

        // Release the warmup gate.  Must run on every path that leaves this
        // iteration, or the workers waiting above never wake.  Called with
        // map_mutex_ held.  Recording this worker's own generation rather than
        // the current one is deliberate: if the generation moved on while this
        // file was parsing, the gate stays armed for the new burst.
        const auto release_warmup_locked = [&] {
            if (!warmup_owner)
                return;
            warmup_owner = false;
            background_warmup_running_ = false;
            background_warmup_generation_ = generation;
        };

        if (live_doc) {
            // The queued path may represent an indirect include dependency
            // refresh, not a direct edit to this open document.  Reparse the
            // live text in the background so includes are resolved against the
            // latest open-buffer overlays, then atomically replace the stale
            // DocumentState if the user has not edited it meanwhile.
            auto reparsed_live_doc = make_state(uri, live_doc->text);
            auto live_index = get_dynamic_index(*reparsed_live_doc);
            std::vector<std::string> headers_to_build;
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                if (generation == background_generation_) {
                    if (const auto doc = docs_.find(uri);
                        doc != docs_.end() && doc->second == live_doc) {
                        docs_[uri] = reparsed_live_doc;
                        invalidate_extra_snapshots_locked();
                        if (extra_file_set_.contains(path_string)) {
                            extra_cache_[uri] = ExtraFileCacheEntry{
                                .path = path_string,
                                .uri = uri,
                                .index = std::make_shared<SyntaxIndex>(std::move(live_index)),
                            };
                            invalidate_extra_snapshots_locked();
                            schedule_background_project_publish_locked();
                        }
                    }

                    // Claim this buffer's headers too.  What makes an open buffer
                    // different is its unsaved text, and a header's shard is built
                    // from the header itself, so nothing about it is unsaved.
                    // Skipping the claim here left a header that only an open
                    // buffer includes without a shard of its own, which is both a
                    // gap in the project index and the one case the edit path
                    // cannot serve as directives alone.
                    for (const auto& dependency : reparsed_live_doc->include_dependencies) {
                        if (background_header_claims_.insert(dependency).second)
                            headers_to_build.push_back(dependency);
                    }
                }
            }

            auto built_headers = build_header_shards(headers_to_build, *reparsed_live_doc, defines,
                                                     include_dirs, open_overlays,
                                                     background_header_texts_, generation);
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                if (generation == background_generation_ && !built_headers.empty()) {
                    for (auto& header : built_headers) {
                        if (header.stands_alone)
                            standalone_header_uris_.insert(header.uri);
                        background_header_shards_[header.uri] = ExtraFileCacheEntry{
                            .path = path_from_file_uri(header.uri),
                            .uri = header.uri,
                            .index = std::move(header.index),
                        };
                    }
                    invalidate_extra_snapshots_locked();
                }
                release_warmup_locked();
                --background_index_active_;
                if (background_pending_files_.empty() && background_index_active_ == 0)
                    schedule_background_project_publish_locked();
                background_cv_.notify_all();
            }
            continue;
        }

        auto state = make_file_state_with_options(path_string, defines, include_dirs,
                                                  open_overlays, false,
                                                  &background_header_texts_, generation,
                                                  /*collect_diagnostics=*/false,
                                                  /*restrict_index_to_own_file=*/true);
        if (background_stop_.load() || !state || !state->tree) {
            std::lock_guard<std::mutex> lock(map_mutex_);
            release_warmup_locked();
            --background_index_active_;
            background_cv_.notify_all();
            continue;
        }

        // Recorded before the warmup gate is released below, so a sibling
        // worker that parses next -- seeded with this parse's headers -- finds
        // their digests already there and can key its own shard on them.
        remember_parsed_digests(*state, generation);

        // Take the shard out of the dying DocumentState and wrap it before
        // locking.  Copying it under map_mutex_ deep-copied every vector and
        // map in the index while all workers and request handlers waited.
        auto committed_index = std::make_shared<SyntaxIndex>(std::move(state->index));

        std::vector<std::string> headers_to_build;
        // Shards to write to the on-disk cache once map_mutex_ is released:
        // storing hashes every file the shard depends on, which must not happen
        // under the lock every request handler contends for.
        std::shared_ptr<const SyntaxIndex> shard_to_cache;
        std::vector<std::tuple<std::string, std::shared_ptr<const SyntaxIndex>, bool>>
            headers_to_cache;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            if (generation != background_generation_) {
                release_warmup_locked();
                --background_index_active_;
                background_cv_.notify_all();
                continue;
            }

            // If the user opened/edited this file while the disk parse was in
            // flight, the live buffer is newer and must win. The didOpen /
            // didChange path builds and commits that live shard outside this
            // mutex, so do not build it here while holding map_mutex_.
            const bool opened_mid_parse = [&] {
                const auto doc = docs_.find(uri);
                return doc != docs_.end() && doc->second;
            }();
            if (!opened_mid_parse) {
                extra_cache_[uri] = ExtraFileCacheEntry{
                    .path = path_string,
                    .uri = uri,
                    .index = committed_index,
                };
                invalidate_extra_snapshots_locked();
                shard_to_cache = std::move(committed_index);
            }

            // Claim the headers this parse pulled in.  Claiming inside the same
            // critical section that commits the shard is what keeps two workers
            // from building the same header concurrently; the build itself
            // happens below, outside the lock.
            //
            // Claimed even when the buffer opened mid-parse and this file's own
            // shard is discarded: a header's shard is built from the header, so
            // whose text pulled it in does not matter.  Dropping the claim there
            // left a header nothing else includes without a shard at all, since
            // nothing re-queues a file that is now open.
            for (const auto& dependency : state->include_dependencies) {
                if (background_header_claims_.insert(dependency).second)
                    headers_to_build.push_back(dependency);
            }
        }

        // Build the claimed headers outside map_mutex_ for the same reason the
        // file's own shard is built outside it.  This worker stays counted as
        // active until they are committed, so the publish below cannot fire on a
        // project index that is still missing header shards.
        auto built_headers = build_header_shards(headers_to_build, *state, defines, include_dirs,
                                                 open_overlays, background_header_texts_,
                                                 generation, state.get());
        // Again, for the headers build_header_shards() parsed on their own.
        remember_parsed_digests(*state, generation);

        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            if (generation == background_generation_ && !built_headers.empty()) {
                for (auto& header : built_headers) {
                    if (header.stands_alone)
                        standalone_header_uris_.insert(header.uri);
                    headers_to_cache.emplace_back(header.uri, header.index, header.stands_alone);
                    background_header_shards_[header.uri] = ExtraFileCacheEntry{
                        .path = path_from_file_uri(header.uri),
                        .uri = header.uri,
                        .index = std::move(header.index),
                    };
                }
                invalidate_extra_snapshots_locked();
            }

            release_warmup_locked();

            // Claim the shard writes below while still holding map_mutex_.  The
            // handover happens after this block, but the wake that follows the
            // decrement is what a test takes as "indexing finished" -- so the
            // writes have to be countable before it, or the drain wait after it
            // sees an empty queue and reports a cache that is not written yet.
            {
                size_t reserved = shard_to_cache ? 1 : 0;
                for (const auto& [header_uri, header_index, stands_alone] : headers_to_cache) {
                    (void)header_uri;
                    (void)stands_alone;
                    if (header_index)
                        ++reserved;
                }
                reserve_shard_writes(reserved);
            }

            // ProjectIndex is an immutable view derived from per-file shards.
            // Do not publish after every single file while the initial .f cache
            // is warming: that would repeatedly notify downstream features for
            // partially warmed snapshots.  Request one debounced publish only
            // after the queue drains so [design].project_index_publish_debounce_ms
            // applies consistently to disk-backed reindex and live edit paths.
            // With several workers draining the queue, "drained" also requires
            // that no sibling worker is still parsing a file.
            --background_index_active_;
            if (background_pending_files_.empty() && background_index_active_ == 0)
                schedule_background_project_publish_locked();
            background_cv_.notify_all();
        }

        // Handed to the writer thread rather than written here: a cache write
        // is an optimization for the *next* launch and must never sit between
        // this one's last parse and its publish.
        if (shard_to_cache) {
            queue_shard_write(PendingShardWrite{
                .uri = uri,
                .index = std::move(shard_to_cache),
                .include_resolutions = resolutions_written_in(state->include_resolutions, uri),
                .generation = generation,
            });
        }
        for (auto& [header_uri, header_index, stands_alone] : headers_to_cache) {
            if (!header_index)
                continue;
            // A header that did not stand alone was sharded from this file's
            // tree, so its shard is only valid while that file is unchanged --
            // nothing inside the shard records that, so it is passed in.
            queue_shard_write(PendingShardWrite{
                .uri = header_uri,
                .index = std::move(header_index),
                .include_resolutions =
                    resolutions_written_in(state->include_resolutions, header_uri),
                .extra_dependency_uri = stands_alone ? std::string{} : uri,
                .stands_alone = stands_alone,
                .generation = generation,
            });
        }
    }
}

std::function<void()> Analyzer::publish_project_index_snapshot_locked() const {
    auto snapshot = std::make_shared<ProjectIndexSnapshot>();

    // Header shards are published alongside file shards: a file's own shard no
    // longer carries what it `include`d, so the header's shard is the only place
    // those declarations live.
    snapshot->shards.reserve(extra_cache_.size() + background_header_shards_.size());
    const auto add_shard = [&snapshot](const ExtraFileCacheEntry& entry) {
        if (!entry.index)
            return;

        snapshot->shards.push_back(ProjectIndexSnapshot::Shard{
            .path = entry.path,
            .uri = entry.uri,
            .index = entry.index,
        });

        // Lightweight global module lookup.  Keep first definition wins to
        // preserve the historical merge behavior for duplicate module names.
        for (size_t i = 0; i < entry.index->modules.size(); ++i) {
            const auto& module = entry.index->modules[i];
            snapshot->module_by_name.try_emplace(module.name, ProjectIndexModuleRef{
                .shard = entry.index,
                .module_index = i,
            });
        }
    };

    for (const auto& [key, entry] : extra_cache_)
        add_shard(entry);
    for (const auto& [key, entry] : background_header_shards_)
        add_shard(entry);

    project_index_snapshot_cache_ = std::move(snapshot);

    // Return the callback to the caller instead of invoking it here.  This
    // function is called while map_mutex_ is held; endpoint notifications can
    // block on client or logging behavior and must not run under the analyzer
    // mutex.
    return project_index_publish_callback_;
}

void Analyzer::clear_project_index_snapshot_locked() const {
    project_index_snapshot_cache_.reset();
}

void Analyzer::update_extra_cache_for_live_state_locked(
    std::shared_ptr<const DocumentState> state, SyntaxIndex index) {
    if (!state)
        return;

    const auto path_string = state->normalized_path;
    const auto uri = state->uri;

    // Only files explicitly listed in the design filelist participate in the
    // project index.  Random open buffers should not pollute project-wide
    // completion for the configured design.
    if (!extra_file_set_.contains(path_string))
        return;

    extra_cache_[uri] = ExtraFileCacheEntry{
        .path = path_string,
        .uri = uri,
        .index = std::make_shared<SyntaxIndex>(std::move(index)),
    };
    invalidate_extra_snapshots_locked();

    // The per-file shard changed, so the published merged project snapshot is
    // stale until the background indexer republishes it.  Publish asynchronously
    // instead of merging synchronously on the edit/open path; this preserves the
    // HPC-friendly rule that request/edit handlers do not rebuild whole-project
    // state inline.
    schedule_background_project_publish_locked();

}
