#include "background_compiler.hpp"
#include "compilation_builder.hpp"
#include "cpu_budget.hpp"
#include "syntax_index_shared.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <slang/ast/Compilation.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/text/SourceManager.h>

namespace {

using Clock = std::chrono::steady_clock;

// Keep optional semantic compilation conservative.  This is a background
// diagnostic path, not a throughput benchmark; every worker compiles the whole
// design, so extra workers multiply peak memory rather than splitting work.
constexpr int kMaxBackgroundCompilerThreads = 4;

// Semantic compilation is the most deferrable work the server does: nothing
// waits on it, and it is the heaviest thing running.  Yield harder than the
// project indexer, which gates when cross-file features start answering.
constexpr int kBackgroundCompilerNiceValue = 10;

static std::string diagnostic_uri(const slang::SourceManager& sm, const std::string& fallback_uri,
                                  slang::SourceLocation location) {
    if (!location.valid())
        return fallback_uri;

    try {
        auto uri = uri_from_source_location(sm, location);
        return uri.empty() ? fallback_uri : uri;
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

struct BackgroundCompiler::WorkerSlot {
    explicit WorkerSlot(size_t worker_id) : id(worker_id) {}

    // Monotonic debug identity.  Do not use this as the desired worker index:
    // workers can retire and be erased, then later workers may be spawned with
    // larger ids.  The explicit retire flag is the source of truth.
    size_t id{0};

    // Guarded by BackgroundCompiler::mutex_.  configure() sets retire when the
    // configured worker count shrinks.  The worker checks it only between jobs,
    // so currently-running slang compilation is allowed to finish cleanly.
    bool retire{false};

    // Guarded by BackgroundCompiler::mutex_.  Set immediately before the worker
    // thread returns; configure() can then join and erase the thread without
    // blocking on long background compilation work.
    bool exited{false};

    std::thread thread;
};

BackgroundCompiler::BackgroundCompiler(SnapshotCallback snapshot_callback,
                                       ResultCallback result_callback)
    : snapshot_callback_(std::move(snapshot_callback)),
      result_callback_(std::move(result_callback)) {}

BackgroundCompiler::~BackgroundCompiler() { stop(); }

std::vector<std::thread> BackgroundCompiler::collect_exited_workers_locked() {
    std::vector<std::thread> exited_threads;

    auto it = workers_.begin();
    while (it != workers_.end()) {
        auto& slot = *it;
        if (!slot->exited) {
            ++it;
            continue;
        }

        if (slot->thread.joinable())
            exited_threads.push_back(std::move(slot->thread));
        it = workers_.erase(it);
    }

    return exited_threads;
}

void BackgroundCompiler::configure(BackgroundCompilerConfig config) {
    config.thread_count = std::clamp(config.thread_count, 1, kMaxBackgroundCompilerThreads);
    config.debounce_ms = std::max(0, config.debounce_ms);

    std::vector<std::thread> exited_threads;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        enabled_ = config.enabled;
        log_timing_ = config.log_timing;
        debounce_ms_ = config.debounce_ms;

        exited_threads = collect_exited_workers_locked();

        if (!enabled_) {
            pending_ = false;
            ++latest_generation_;
            lock.unlock();
            cv_.notify_all();
        } else {
            // If the user raises the count again before previously-retired
            // workers have exited, keep those workers instead of spawning an
            // unnecessary replacement.  Retire requests are only a graceful
            // shrink mechanism, not an irreversible cancellation.
            if (static_cast<int>(workers_.size()) <= config.thread_count) {
                for (auto& slot : workers_)
                    slot->retire = false;
            }

            while (static_cast<int>(workers_.size()) < config.thread_count) {
                auto slot = std::make_shared<WorkerSlot>(next_worker_id_++);
                slot->thread = std::thread([this, slot] {
                    apply_background_thread_nice(kBackgroundCompilerNiceValue);
                    worker_loop(std::move(slot));
                });
                workers_.push_back(std::move(slot));
            }

            // Graceful shrink: mark newest excess workers for retirement.  An
            // idle worker exits immediately after the notify below; a busy
            // worker exits after publishing/skipping its current generation and
            // before taking another pending snapshot.
            int kept = 0;
            for (auto& slot : workers_) {
                if (slot->exited)
                    continue;
                if (kept < config.thread_count) {
                    ++kept;
                    continue;
                }
                slot->retire = true;
            }

            lock.unlock();
            cv_.notify_all();
        }
    }

    // Join only workers that have already reported exit.  This keeps config
    // reload responsive: lowering the thread count does not block on a large
    // in-progress semantic compile; cleanup happens on a later configure() or
    // stop().
    for (auto& worker : exited_threads) {
        if (worker.joinable())
            worker.join();
    }
}

void BackgroundCompiler::schedule() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || stopping_)
        return;

    pending_ = true;
    due_time_ = Clock::now() + std::chrono::milliseconds(debounce_ms_);
    ++latest_generation_;
    cv_.notify_all();
}

void BackgroundCompiler::stop() {
    std::vector<std::thread> workers_to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        enabled_ = false;
        pending_ = false;
        ++latest_generation_;

        // Move thread handles out while holding mutex_ so configure() cannot
        // concurrently erase or append workers_ while stop() is deciding what
        // must be joined.  The worker threads keep their WorkerSlot alive via
        // the shared_ptr captured by the thread function; clearing this vector
        // only drops the server's bookkeeping references.
        workers_to_join.reserve(workers_.size());
        for (auto& slot : workers_) {
            if (slot->thread.joinable())
                workers_to_join.push_back(std::move(slot->thread));
        }
        workers_.clear();
    }
    cv_.notify_all();
    for (auto& worker : workers_to_join) {
        if (worker.joinable())
            worker.join();
    }
}

void BackgroundCompiler::worker_loop(std::shared_ptr<WorkerSlot> slot) {
    while (true) {
        uint64_t generation = 0;
        CompilationSnapshot snapshot;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return stopping_ || slot->retire || (enabled_ && pending_);
            });
            if (stopping_ || slot->retire) {
                slot->exited = true;
                return;
            }

            while (!stopping_ && !slot->retire && pending_) {
                if (cv_.wait_until(lock, due_time_) == std::cv_status::timeout)
                    break;
            }
            if (stopping_ || slot->retire) {
                slot->exited = true;
                return;
            }
            if (!enabled_ || !pending_)
                continue;

            generation = latest_generation_;
            pending_ = false;
        }

        // Build the full design snapshot only after debounce has elapsed.  This
        // avoids walking the whole filelist/open-document set for every
        // keystroke when rapid edits will be coalesced anyway.
        if (snapshot_callback_)
            snapshot = snapshot_callback_();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || generation != latest_generation_)
                continue;
        }

        auto result = compile(generation, std::move(snapshot));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || generation != latest_generation_)
                continue;
        }
        result_callback_(std::move(result));
    }
}

BackgroundCompileResult BackgroundCompiler::compile(uint64_t generation,
                                                    CompilationSnapshot snapshot) const {
    const auto start = Clock::now();
    BackgroundCompileResult result;
    result.generation = generation;
    result.open_uris = std::move(snapshot.open_uris);

    slang::ast::CompilationOptions compilation_options;
    compilation_options.flags |= slang::ast::CompilationFlags::LintMode;

    auto built = build_compilation(snapshot, compilation_options,
                                   log_timing_.load(std::memory_order_relaxed));
    auto& source_manager = built.source_manager;
    const std::string& first_uri = built.first_uri;

    if (!first_uri.empty()) {
        slang::DiagnosticEngine engine(*source_manager);
        try {
            for (const auto& diagnostic : built.compilation->getSemanticDiagnostics()) {
                std::string uri;
                auto info = convert_diagnostic(*source_manager, engine, diagnostic, first_uri, uri);
                result.diagnostics_by_uri[uri].push_back(std::move(info));
            }
        } catch (const std::exception& e) {
            const bool log_timing = log_timing_.load(std::memory_order_relaxed);
            if (log_timing)
                std::cerr << "[lazyverilog] semantic diagnostics failed: " << e.what() << "\n";
        }
    }

    result.uri_versions = std::move(snapshot.uri_versions);

    const bool log_timing = log_timing_.load(std::memory_order_relaxed);
    if (log_timing) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        std::cerr << "[lazyverilog][compilation] semantic compilation files="
                  << snapshot.files.size() << ": " << elapsed.count() << "ms\n";
    }

    return result;
}
