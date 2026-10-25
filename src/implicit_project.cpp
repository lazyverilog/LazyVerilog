#include "implicit_project.hpp"

#include "project_root.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

std::optional<ImplicitProjectMode> parse_implicit_project_mode(std::string_view text) {
    if (text == "off")
        return ImplicitProjectMode::Off;
    if (text == "workspace")
        return ImplicitProjectMode::Workspace;
    if (text == "auto")
        return ImplicitProjectMode::Auto;
    return std::nullopt;
}

std::string_view implicit_project_mode_name(ImplicitProjectMode mode) {
    switch (mode) {
    case ImplicitProjectMode::Off:
        return "off";
    case ImplicitProjectMode::Workspace:
        return "workspace";
    case ImplicitProjectMode::Auto:
        return "auto";
    }
    return "auto";
}

bool implicit_glob_match(std::string_view pattern, std::string_view path) {
    if (pattern.empty())
        return path.empty();
    if (pattern.starts_with("**")) {
        auto rest = pattern.substr(2);
        if (rest.starts_with('/')) {
            // `**/x` matches `x` itself as well as `a/b/x`.
            rest.remove_prefix(1);
            if (implicit_glob_match(rest, path))
                return true;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '/' && implicit_glob_match(rest, path.substr(i + 1)))
                    return true;
            }
            return false;
        }
        for (size_t i = 0; i <= path.size(); ++i) {
            if (implicit_glob_match(rest, path.substr(i)))
                return true;
        }
        return false;
    }
    if (pattern[0] == '*') {
        for (size_t i = 0; i <= path.size(); ++i) {
            if (implicit_glob_match(pattern.substr(1), path.substr(i)))
                return true;
            if (i < path.size() && path[i] == '/')
                break;
        }
        return false;
    }
    if (path.empty())
        return false;
    if (pattern[0] == '?')
        return path[0] != '/' && implicit_glob_match(pattern.substr(1), path.substr(1));
    return pattern[0] == path[0] && implicit_glob_match(pattern.substr(1), path.substr(1));
}

namespace {

/// Directories no design keeps its sources in: tool output, caches, and every
/// simulator's work library.  Checked by name at every depth.
bool is_skipped_directory_name(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        "node_modules", "out", "work", "sim_build", "obj_dir", "xcelium.d",
        "INCA_libs", "simv.daidir", "csrc",
    };
    static constexpr std::string_view kSuffixes[] = {".runs", ".cache", ".sim", ".gen"};
    if (name.starts_with('.') || name.starts_with("build"))
        return true;
    if (std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames))
        return true;
    return std::any_of(std::begin(kSuffixes), std::end(kSuffixes),
                       [&](std::string_view suffix) { return name.ends_with(suffix); });
}

bool is_source_extension(std::string_view ext) { return ext == ".sv" || ext == ".v"; }

bool is_header_extension(std::string_view ext) {
    return ext == ".svh" || ext == ".vh" || ext == ".svi";
}

/// What the `.gitignore` files from the root down to one directory exclude.
struct IgnoreRules {
    /// Directory names excluded at any depth below the `.gitignore`.
    std::vector<std::string> dir_names;
    /// File extensions (with the dot) excluded at any depth below it.
    std::vector<std::string> extensions;
};

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

/// @p inherited plus what @p gitignore says.  Anchored names (`/gen`) apply
/// only to the directory holding the file, so they are returned separately.
///
/// shortcut: a .gitignore subset (plain and anchored names, applied to
/// directories, and `*.ext`); negation, path patterns and globs are ignored.
/// Upgrade if users report generated RTL being indexed.
std::shared_ptr<const IgnoreRules> read_gitignore(const IgnoreRules& inherited,
                                                  const fs::path& gitignore,
                                                  std::vector<std::string>& anchored) {
    auto rules = std::make_shared<IgnoreRules>(inherited);
    const auto text = read_file_text_optional(gitignore);
    if (!text)
        return rules;
    size_t pos = 0;
    while (pos <= text->size()) {
        auto end = text->find('\n', pos);
        if (end == std::string::npos)
            end = text->size();
        std::string_view line(text->data() + pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#' || line.front() == '!')
            continue;
        const bool is_anchored = line.front() == '/';
        if (is_anchored)
            line.remove_prefix(1);
        const bool dir_only = !line.empty() && line.back() == '/';
        if (dir_only)
            line.remove_suffix(1);
        if (line.empty() || line.find('/') != std::string_view::npos)
            continue;
        const bool has_glob = line.find_first_of("*?[") != std::string_view::npos;
        if (!has_glob) {
            (is_anchored ? anchored : rules->dir_names).emplace_back(line);
        } else if (!is_anchored && !dir_only && line.starts_with("*.") &&
                   line.substr(1).find_first_of("*?[") == std::string_view::npos) {
            rules->extensions.emplace_back(line.substr(1));
        }
    }
    return rules;
}

} // namespace

ImplicitScan scan_implicit_root(const fs::path& root, const std::vector<std::string>& exclude,
                                const ImplicitScanLimits& limits) {
    const auto start = std::chrono::steady_clock::now();
    const auto excluded = [&](const std::string& relative) {
        return std::any_of(exclude.begin(), exclude.end(), [&](const std::string& pattern) {
            return implicit_glob_match(pattern, relative);
        });
    };
    const auto give_up = [](std::string reason) {
        ImplicitScan scan;
        scan.over_limit = true;
        scan.over_limit_reason = std::move(reason);
        return scan;
    };

    struct Pending {
        fs::path dir;
        std::string relative; ///< '/'-separated, empty for the root
        std::shared_ptr<const IgnoreRules> rules;
    };
    std::vector<Pending> stack{{normalize_filesystem_path(root), {},
                                std::make_shared<const IgnoreRules>()}};
    std::vector<std::pair<std::string, uintmax_t>> sources;
    std::set<std::string> header_dirs;
    size_t entries = 0;

    while (!stack.empty()) {
        auto current = std::move(stack.back());
        stack.pop_back();

        // The whole directory is listed before anything in it is kept: a
        // lazyverilog.toml anywhere in the listing makes the directory an
        // explicit project, and a .gitignore applies to its siblings.
        std::vector<fs::directory_entry> subdirs;
        std::vector<fs::directory_entry> files;
        bool has_marker = false;
        bool has_gitignore = false;
        std::error_code ec;
        fs::directory_iterator it(current.dir, fs::directory_options::skip_permission_denied, ec);
        const fs::directory_iterator done;
        for (; !ec && it != done; it.increment(ec)) {
            if (++entries > limits.max_dir_entries)
                return give_up("more than " + std::to_string(limits.max_dir_entries) +
                               " directory entries");
            if ((entries & 1023) == 0 &&
                std::chrono::steady_clock::now() - start > limits.max_time)
                return give_up("scanning took longer than " +
                               std::to_string(limits.max_time.count()) + " ms");
            const auto& entry = *it;
            std::error_code kind_ec;
            if (entry.is_symlink(kind_ec)) {
                // Not followed into: a link can leave the root or loop.  A
                // linked file is still a file in this tree.
                if (entry.is_regular_file(kind_ec))
                    files.push_back(entry);
                continue;
            }
            if (entry.is_directory(kind_ec)) {
                subdirs.push_back(entry);
                continue;
            }
            if (!entry.is_regular_file(kind_ec))
                continue;
            const auto name = entry.path().filename();
            if (name == ProjectRootResolver::kMarker)
                has_marker = true;
            else if (name == ".gitignore")
                has_gitignore = true;
            else
                files.push_back(entry);
        }
        if (has_marker)
            continue;

        std::vector<std::string> anchored;
        auto rules = has_gitignore ? read_gitignore(*current.rules, current.dir / ".gitignore",
                                                    anchored)
                                   : current.rules;
        const auto relative_of = [&](const fs::directory_entry& entry) {
            const auto name = entry.path().filename().generic_string();
            return current.relative.empty() ? name : current.relative + "/" + name;
        };

        for (const auto& entry : files) {
            const auto ext = entry.path().extension().string();
            const bool source = is_source_extension(ext);
            if (!source && !is_header_extension(ext))
                continue;
            if (contains(rules->extensions, ext) || excluded(relative_of(entry)))
                continue;
            if (!source) {
                header_dirs.insert(normalize_filesystem_path(current.dir).string());
                continue;
            }
            std::error_code size_ec;
            const auto size = entry.file_size(size_ec);
            sources.emplace_back(normalize_filesystem_path(entry.path()).string(),
                                 size_ec ? 0 : size);
            if (sources.size() > limits.max_source_files)
                return give_up("more than " + std::to_string(limits.max_source_files) +
                               " source files");
        }

        for (const auto& entry : subdirs) {
            const auto name = entry.path().filename().string();
            if (is_skipped_directory_name(name) || contains(rules->dir_names, name) ||
                contains(anchored, name))
                continue;
            auto relative = relative_of(entry);
            if (excluded(relative) || excluded(relative + "/"))
                continue;
            stack.push_back(Pending{entry.path(), std::move(relative), rules});
        }
    }

    // Sorted, whatever order the directories were listed in: fold order and the
    // include path's order both reach a digest, and neither may depend on it.
    std::sort(sources.begin(), sources.end());
    ImplicitScan scan;
    scan.files.reserve(sources.size());
    scan.file_sizes.reserve(sources.size());
    for (auto& [path, size] : sources) {
        scan.files.push_back(std::move(path));
        scan.file_sizes.push_back(size);
    }
    scan.include_dirs.assign(header_dirs.begin(), header_dirs.end());
    if (scan.include_dirs.size() > limits.max_include_dirs)
        scan.include_dirs.resize(limits.max_include_dirs);
    return scan;
}
