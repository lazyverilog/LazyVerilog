#pragma once

#include "index_cache.hpp"
#include "project_root.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/// Everything a parse of one file needs from its project's configuration.
///
/// This is clangd's `tooling::CompileCommand`, reduced to what SystemVerilog
/// actually varies: preprocessor defines and the include search path.  Like a
/// compile command it belongs to a *file*, not to the session -- two files open
/// at once can be in different projects, and `+incdir+` entries that make one of
/// them parse are meaningless for the other.
struct ParseInputs {
    /// `[design].define`, as written.
    std::vector<std::string> defines;
    /// `+incdir+` entries as configured, normalized but not globbed.  Kept
    /// because the semantic-compilation snapshot passes them on verbatim.
    std::vector<std::string> include_dirs;
    /// The same entries globbed to directories that exist, once, rather than on
    /// every SourceManager.  With a few hundred include directories this
    /// dominated the edit path, and on a shared filesystem each stat is a round
    /// trip.  See resolve_include_dirs() in analyzer.cpp.
    std::vector<std::filesystem::path> include_dir_paths;
    /// Digest of the two lists above, which is what a shard is keyed on: they
    /// change what a parse of an unchanged file *means*.  Per project, because
    /// a shard written for one project's defines must not be served to another.
    IndexCache::Digest config_digest;
};

/// Answers "how is this file parsed", per file.
///
/// The analog of clangd's `GlobalCompilationDatabase::getCompileCommand(File)`,
/// including its fallback: a file that belongs to no known project is parsed
/// with the default inputs rather than refused.
///
/// One indexer consults this per file; there is deliberately no second Analyzer
/// per project.  That is clangd's shape too -- one `BackgroundIndex`, commands
/// looked up per file -- and it is what keeps a second project costing a map
/// entry instead of another set of worker threads and another source manager.
///
/// Immutable once built.  Callers hold a `shared_ptr<const ProjectParseInputs>`
/// across a whole parse while a config reload installs a replacement, so there
/// is no window where a parse reads half of one project's inputs and half of
/// another's.
class ProjectParseInputs {
public:
    /// @p resolver decides which project a path belongs to.  Null resolves
    /// everything to the defaults, which is what the CLI tools and most tests
    /// want: they are given one project outright.
    explicit ProjectParseInputs(std::shared_ptr<const ProjectRootResolver> resolver = nullptr);

    /// Deep copy.  The entries are held by unique_ptr so that for_path() can
    /// hand out a reference that stays valid while the map grows, which means
    /// the implicit copy is deleted -- and copying is exactly how a new table
    /// is built before being published in place of the old one.
    ProjectParseInputs(const ProjectParseInputs& other);
    ProjectParseInputs& operator=(const ProjectParseInputs&) = delete;

    /// Inputs used for a file in no registered project.
    ///
    /// Also what `Analyzer::set_defines()` / `set_include_dirs()` write, and
    /// what a single-project session uses for everything -- so the behaviour
    /// with one project is exactly what it was before this existed.
    void set_defaults(ParseInputs inputs);

    /// Point lookups at @p resolver.  Set on a copy before the copy is
    /// published, which is how the Analyzer swaps inputs without a worker ever
    /// seeing a half-built table.
    void set_resolver(std::shared_ptr<const ProjectRootResolver> resolver);

    /// Register @p inputs for files under @p root.
    void set_for_root(const std::filesystem::path& root, ParseInputs inputs);

    /// The inputs @p path is parsed with.  Never null, and stable for the
    /// lifetime of this object.
    const ParseInputs& for_path(const std::filesystem::path& path) const;
    /// Convenience for the many call sites that hold a URI.
    const ParseInputs& for_uri(std::string_view uri) const;

    /// The inputs registered for @p root itself, or the defaults if none is.
    ///
    /// for_path() answers "how does this file parse" and walks up to find the
    /// project; this answers "how does this project parse" for a caller that
    /// already holds the root.  Semantic compilation needs that form: it builds
    /// one preprocessor for a whole project, and asking through one of its files
    /// would take the wrong answer for a `.f` that names a sibling project's
    /// source.
    const ParseInputs& for_root(const std::filesystem::path& root) const;

    /// Which project @p path belongs to, or an empty path when it belongs to
    /// none.
    ///
    /// The same walk, and the same per-directory cache, that decides the file's
    /// config and its shard directory -- deliberately, because a third notion
    /// of "which project is this file in" is how the features came to disagree
    /// about it in the first place.  Unlike for_path() this does not require the
    /// root to have registered parse inputs: a file's project is a fact about
    /// the file, and a caller asking whether two files are in the same one is
    /// not asking how either of them parses.
    std::filesystem::path project_root_for(const std::filesystem::path& path) const;

    /// The merged inputs, for a caller that has no project to ask about -- a file
    /// under none, and the semantic compilation fallback taken when no project
    /// has registered what it compiles.  A project that has is served by
    /// for_root() instead.
    const ParseInputs& defaults() const { return defaults_; }

    /// Every registered root's inputs plus the defaults, for callers that need
    /// the union rather than one file's answer.
    std::vector<const ParseInputs*> all() const;

private:
    std::shared_ptr<const ProjectRootResolver> resolver_;
    ParseInputs defaults_;
    /// Stable addresses: for_path() hands out a reference held across a parse.
    std::unordered_map<std::string, std::unique_ptr<ParseInputs>> by_root_;
};

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
std::vector<std::filesystem::path>
resolve_include_dirs(const std::vector<std::string>& dirs);

/// Glob `+incdir+` entries to the directories that exist and compute the
/// digest, producing the inputs a project parses with.
ParseInputs make_parse_inputs(const std::vector<std::string>& defines,
                              const std::vector<std::string>& include_dirs);
