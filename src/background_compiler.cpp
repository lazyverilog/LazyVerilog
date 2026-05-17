#include "background_compiler.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <slang/ast/Compilation.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <slang/util/Bag.h>
#include <unordered_set>

namespace {

using Clock = std::chrono::steady_clock;

static std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static std::string path_to_uri(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).lexically_normal().string();
}

static std::string normalize_path(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}

static std::string diagnostic_uri(const slang::SourceManager& sm, const std::string& fallback_uri,
                                  slang::SourceLocation location) {
    if (!location.valid())
        return fallback_uri;

    try {
        auto file_name = sm.getFileName(location);
        if (file_name.empty())
            return fallback_uri;

        std::string text(file_name);
        if (text.starts_with("file://"))
            return text;
        return path_to_uri(text);
    } catch (...) {
        return fallback_uri;
    }
}

static ParseDiagInfo convert_diagnostic(const slang::SourceManager& sm,
                                        slang::DiagnosticEngine& engine,
                                        const slang::Diagnostic& diagnostic,
                                        const std::string& fallback_uri, std::string& uri) {
    ParseDiagInfo info;
    try {
        auto loc = diagnostic.location.valid() ? sm.getFullyExpandedLoc(diagnostic.location)
                                               : diagnostic.location;
        uri = diagnostic_uri(sm, fallback_uri, loc);
        if (loc.valid() && sm.isFileLoc(loc)) {
            const size_t line = sm.getLineNumber(loc);
            const size_t col = sm.getColumnNumber(loc);
            info.line = line > 0 ? static_cast<int>(line) - 1 : 0;
            info.col = col > 0 ? static_cast<int>(col) - 1 : 0;
        }
    } catch (...) {
        uri = fallback_uri;
    }

    auto sev = slang::getDefaultSeverity(diagnostic.code);
    if (sev == slang::DiagnosticSeverity::Error || sev == slang::DiagnosticSeverity::Fatal)
        info.severity = 1;
    else if (sev == slang::DiagnosticSeverity::Warning)
        info.severity = 2;
    else
        info.severity = 3;

    try {
        info.message = engine.formatMessage(diagnostic);
    } catch (...) {
        info.message = "(semantic diagnostic format error)";
    }
    return info;
}

} // namespace

BackgroundCompiler::BackgroundCompiler(ResultCallback callback) : callback_(std::move(callback)) {}

BackgroundCompiler::~BackgroundCompiler() { stop(); }

void BackgroundCompiler::configure(bool enabled, int thread_count, int debounce_ms,
                                   bool log_timing) {
    thread_count = std::max(1, thread_count);
    debounce_ms = std::max(0, debounce_ms);

    std::unique_lock<std::mutex> lock(mutex_);
    enabled_ = enabled;
    log_timing_ = log_timing;
    debounce_ms_ = debounce_ms;

    if (!enabled_) {
        pending_.reset();
        ++latest_generation_;
        lock.unlock();
        cv_.notify_all();
        return;
    }

    while (static_cast<int>(workers_.size()) < thread_count)
        workers_.emplace_back([this] { worker_loop(); });

    // Keep existing workers if the count is lowered. They are idle unless jobs
    // are scheduled, and avoiding per-config thread teardown keeps this simple.
}

void BackgroundCompiler::schedule(CompilationSnapshot snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || stopping_)
        return;

    pending_ = std::move(snapshot);
    due_time_ = Clock::now() + std::chrono::milliseconds(debounce_ms_);
    ++latest_generation_;
    cv_.notify_all();
}

void BackgroundCompiler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        enabled_ = false;
        pending_.reset();
        ++latest_generation_;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
    workers_.clear();
}

void BackgroundCompiler::worker_loop() {
    while (true) {
        uint64_t generation = 0;
        CompilationSnapshot snapshot;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || (enabled_ && pending_.has_value()); });
            if (stopping_)
                return;

            while (!stopping_ && pending_.has_value()) {
                if (cv_.wait_until(lock, due_time_) == std::cv_status::timeout)
                    break;
            }
            if (stopping_)
                return;
            if (!enabled_ || !pending_)
                continue;

            generation = latest_generation_;
            snapshot = std::move(*pending_);
            pending_.reset();
        }

        auto result = compile(generation, std::move(snapshot));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || generation != latest_generation_)
                continue;
        }
        callback_(std::move(result));
    }
}

BackgroundCompileResult BackgroundCompiler::compile(uint64_t generation,
                                                    CompilationSnapshot snapshot) const {
    const auto start = Clock::now();
    BackgroundCompileResult result;
    result.generation = generation;
    result.open_uris = std::move(snapshot.open_uris);

    auto source_manager = std::make_unique<slang::SourceManager>();

    slang::parsing::PreprocessorOptions preprocessor_options;
    preprocessor_options.predefines = snapshot.defines;

    slang::ast::CompilationOptions compilation_options;
    compilation_options.flags |= slang::ast::CompilationFlags::LintMode;

    slang::Bag bag;
    bag.set(preprocessor_options);
    bag.set(compilation_options);

    slang::ast::Compilation compilation(bag);
    std::string first_uri;
    std::unordered_set<std::string> assigned_paths;

    auto refresh_assigned_paths = [&] {
        assigned_paths.clear();
        for (auto buffer : source_manager->getAllBuffers()) {
            const auto& path = source_manager->getFullPath(buffer);
            if (!path.empty())
                assigned_paths.insert(normalize_path(path));
        }
    };

    for (const auto& file : snapshot.files) {
        const auto normalized_path = normalize_path(file.path);
        if (assigned_paths.contains(normalized_path))
            continue;

        std::string text = file.text ? *file.text : read_file_text(file.path);
        if (text.empty() && !file.text)
            continue;

        if (first_uri.empty())
            first_uri = file.uri;

        try {
            auto tree = slang::syntax::SyntaxTree::fromText(std::string_view(text), *source_manager,
                                                            std::string_view(file.uri),
                                                            std::string_view(file.path), bag);
            compilation.addSyntaxTree(std::move(tree));
            refresh_assigned_paths();
        } catch (const std::exception& e) {
            bool log_timing = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                log_timing = log_timing_;
            }
            if (log_timing) {
                std::cerr << "[lazyverilog] semantic compile skipped " << file.path << ": "
                          << e.what() << "\n";
            }
            refresh_assigned_paths();
        }
    }

    if (!first_uri.empty()) {
        slang::DiagnosticEngine engine(*source_manager);
        try {
            for (const auto& diagnostic : compilation.getSemanticDiagnostics()) {
                std::string uri;
                auto info = convert_diagnostic(*source_manager, engine, diagnostic, first_uri, uri);
                result.diagnostics_by_uri[uri].push_back(std::move(info));
            }
        } catch (const std::exception& e) {
            bool log_timing = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                log_timing = log_timing_;
            }
            if (log_timing)
                std::cerr << "[lazyverilog] semantic diagnostics failed: " << e.what() << "\n";
        }
    }

    bool log_timing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        log_timing = log_timing_;
    }
    if (log_timing) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        std::cerr << "[lazyverilog][compilation] semantic compilation files="
                  << snapshot.files.size() << ": " << elapsed.count() << "ms\n";
    }

    return result;
}
