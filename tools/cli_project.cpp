#include "cli_project.hpp"
#include "filelist.hpp"

#include <condition_variable>
#include <mutex>

CliProject resolve_cli_project(const std::filesystem::path& start,
                               const std::string& filelist_override) {
    CliProject project;
    project.root = find_config_root(start);
    if (project.root.empty())
        project.root = std::filesystem::is_directory(start) ? start : start.parent_path();

    project.config = load_config(project.root);

    if (!filelist_override.empty()) {
        // Resolve against the current directory (not `project.root`) and store
        // as absolute so filelist.cpp's root-relative resolution is a no-op:
        // an explicit -f flag means "this exact file", not "this path relative
        // to wherever lazyverilog.toml happened to be found".
        project.config.design.vcode =
            std::filesystem::absolute(filelist_override).lexically_normal().string();
    }

    return project;
}

void index_cli_project(Analyzer& analyzer, const CliProject& project) {
    auto vcode = load_vcode(project.root, project.config);
    analyzer.set_project_config(project.config.design.define, vcode.include_dirs, vcode.files,
                                nullptr, vcode.file_sizes);
}

void run_synchronous_semantic_compile(Analyzer& analyzer, uint32_t error_limit) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    BackgroundCompileResult result;

    BackgroundCompiler compiler(
        [&analyzer] { return analyzer.compilation_snapshot(); },
        [&](BackgroundCompileResult r) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                result = std::move(r);
                done = true;
            }
            cv.notify_one();
        });

    compiler.configure(BackgroundCompilerConfig{
        .enabled = true,
        .error_limit = error_limit,
    });
    // compile_now(), not schedule(): this is a one-shot blocking compile, not
    // the server's rapid-edit path, so there is no next keystroke to coalesce
    // with and the debounce window would be pure latency on every CLI run.
    compiler.compile_now();

    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done; });
    lock.unlock();

    compiler.stop();
    analyzer.set_semantic_diagnostics(std::move(result.diagnostics_by_uri), result.uri_versions);
}
