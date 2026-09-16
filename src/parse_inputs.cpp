#include "parse_inputs.hpp"

#include "string_utils.hpp"

#include <slang/text/Glob.h>

#include <system_error>
#include <utility>

namespace fs = std::filesystem;

std::vector<fs::path> resolve_include_dirs(const std::vector<std::string>& dirs) {
    std::vector<fs::path> resolved;
    resolved.reserve(dirs.size());
    for (const auto& dir : dirs) {
        if (dir.empty())
            continue;
        slang::SmallVector<fs::path> matches;
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

ParseInputs make_parse_inputs(const std::vector<std::string>& defines,
                              const std::vector<std::string>& include_dirs) {
    ParseInputs inputs;
    inputs.defines = defines;

    inputs.include_dirs.reserve(include_dirs.size());
    for (const auto& dir : include_dirs)
        inputs.include_dirs.push_back(normalize_filesystem_path(dir).string());

    inputs.include_dir_paths = resolve_include_dirs(inputs.include_dirs);
    // Digested from the normalized spellings, not the globbed result: the glob
    // depends on which directories happen to exist right now, so keying on it
    // would make creating an unrelated directory that matches a pattern look
    // like a config change and throw away every shard.
    inputs.config_digest = IndexCache::config_digest(inputs.defines, inputs.include_dir_paths);
    return inputs;
}

ProjectParseInputs::ProjectParseInputs(std::shared_ptr<const ProjectRootResolver> resolver)
    : resolver_(std::move(resolver)) {}

ProjectParseInputs::ProjectParseInputs(const ProjectParseInputs& other)
    : resolver_(other.resolver_), defaults_(other.defaults_) {
    by_root_.reserve(other.by_root_.size());
    for (const auto& [root, inputs] : other.by_root_)
        by_root_.emplace(root, std::make_unique<ParseInputs>(*inputs));
}

void ProjectParseInputs::set_defaults(ParseInputs inputs) { defaults_ = std::move(inputs); }

void ProjectParseInputs::set_resolver(std::shared_ptr<const ProjectRootResolver> resolver) {
    resolver_ = std::move(resolver);
}

void ProjectParseInputs::set_for_root(const fs::path& root, ParseInputs inputs) {
    by_root_[root.string()] = std::make_unique<ParseInputs>(std::move(inputs));
}

const ParseInputs& ProjectParseInputs::for_path(const fs::path& path) const {
    if (resolver_ && !by_root_.empty()) {
        if (auto info = resolver_->project_info(path)) {
            auto it = by_root_.find(info->source_root.string());
            if (it != by_root_.end())
                return *it->second;
        }
    }
    // clangd's fallback command: a file outside every known project is parsed
    // with something rather than not at all.  Answering "no inputs" here would
    // make an unconfigured file fail to preprocess instead of merely being
    // parsed without a project's defines.
    return defaults_;
}

const ParseInputs& ProjectParseInputs::for_uri(std::string_view uri) const {
    return for_path(fs::path(path_from_file_uri(std::string(uri))));
}

std::vector<const ParseInputs*> ProjectParseInputs::all() const {
    std::vector<const ParseInputs*> result;
    result.reserve(by_root_.size() + 1);
    result.push_back(&defaults_);
    for (const auto& [root, inputs] : by_root_)
        result.push_back(inputs.get());
    return result;
}
