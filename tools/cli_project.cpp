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
                                resolve_vcode_path(project.root, project.config));
}

void run_synchronous_semantic_compile(Analyzer& analyzer, const CliProject& project) {
    if (!project.config.compilation.background_compilation)
        return;

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

    // debounce_ms=0: this is a one-shot blocking compile, not the server's
    // rapid-edit coalescing path, so there is nothing to wait out.
    compiler.configure(BackgroundCompilerConfig{
        .enabled = true,
        .debounce_ms = 0,
        .log_timing = project.config.compilation.log_timing,
    });
    compiler.schedule();

    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done; });
    lock.unlock();

    compiler.stop();
    analyzer.set_semantic_diagnostics(std::move(result.diagnostics_by_uri), result.uri_versions);
}
