#include "project_root.hpp"

#include "string_utils.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

/// Read an environment variable, treating an empty value as unset.
///
/// `XDG_CACHE_HOME=` is specified to mean "unset", and an empty `HOME` would
/// otherwise turn into a relative path that lands wherever the server happened
/// to be started from.
std::optional<std::string> env_value(const char* name) {
#ifdef _WIN32
    // getenv() on Windows reads a snapshot taken at process start, which misses
    // a variable set by the parent after that.  GetEnvironmentVariable reads
    // the live block.
    DWORD needed = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0)
        return std::nullopt;
    std::string value(needed, '\0');
    DWORD written = ::GetEnvironmentVariableA(name, value.data(), needed);
    if (written == 0 || written >= needed)
        return std::nullopt;
    value.resize(written);
    if (value.empty())
        return std::nullopt;
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::string(value);
#endif
}

} // namespace

void ProjectRootResolver::set_forced_root(fs::path directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    forced_root_ = directory.empty() ? fs::path{} : normalize_filesystem_path(directory);
    directories_.clear();
}

void ProjectRootResolver::invalidate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    directories_.clear();
}

bool ProjectRootResolver::directory_has_marker(const fs::path& directory) const {
    const auto key = directory.string();
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = directories_.find(key);
        if (it != directories_.end()) {
            const auto age = now - it->second.checked_at;
            const auto window = it->second.has_marker ? kFreshFound : kFreshMissing;
            if (age < window)
                return it->second.has_marker;
        }
    }

    // Stat outside the lock.  On a shared filesystem this is a round trip, and
    // every worker resolving a file in the same tree would otherwise queue
    // behind whichever one is waiting on the server.
    std::error_code ec;
    const bool found = fs::is_regular_file(directory / kMarker, ec) && !ec;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        directories_[key] = DirectoryCache{found, now};
    }
    return found;
}

std::optional<ProjectInfo> ProjectRootResolver::project_info(const fs::path& file,
                                                             PathKind kind) const {
    fs::path forced;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        forced = forced_root_;
    }

    // An explicit root bypasses the walk entirely, the way clangd's
    // CompileCommandsDir replaces SearchDirs with a single entry.  It is still
    // a root only if it actually holds a config: a path that names nothing is
    // a misconfiguration, and inventing a project there would write shards into
    // a directory the user never pointed at.
    if (!forced.empty()) {
        if (directory_has_marker(forced))
            return ProjectInfo{forced};
        return std::nullopt;
    }

    if (file.empty())
        return std::nullopt;

    auto absolute = normalize_filesystem_path(file);
    // Only when the caller does not already know.  The directory cache below
    // cannot answer this -- it is keyed on directories and the question is
    // about the path itself -- so it is an uncached stat on a path that is
    // resolved once per request and once per parse.
    fs::path dir;
    if (kind == PathKind::File) {
        dir = absolute.parent_path();
    } else {
        std::error_code ec;
        dir = fs::is_directory(absolute, ec) && !ec ? absolute : absolute.parent_path();
    }

    // Walk to the filesystem root.  parent_path() of a root path is itself,
    // which is the loop's only stop condition -- `dir.empty()` never becomes
    // true for an absolute path.
    while (!dir.empty()) {
        if (directory_has_marker(dir))
            return ProjectInfo{dir};
        auto parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = std::move(parent);
    }
    return std::nullopt;
}

std::optional<fs::path> user_cache_directory() {
#if defined(_WIN32)
    if (auto local = env_value("LOCALAPPDATA"))
        return fs::path(*local);
    return std::nullopt;
#elif defined(__APPLE__)
    if (auto home = env_value("HOME"))
        return fs::path(*home) / "Library" / "Caches";
    return std::nullopt;
#else
    if (auto xdg = env_value("XDG_CACHE_HOME")) {
        // A relative XDG_CACHE_HOME is specified to be ignored.  Honouring it
        // would put the cache wherever the editor happened to be launched from,
        // which is both wrong and different on every launch.
        fs::path path(*xdg);
        if (path.is_absolute())
            return path;
    }
    if (auto home = env_value("HOME"))
        return fs::path(*home) / ".cache";
    return std::nullopt;
#endif
}
