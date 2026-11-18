#pragma once

#include "analyzer.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

struct BackgroundCompileResult {
    uint64_t generation{0};
    std::unordered_map<std::string, std::vector<ParseDiagInfo>> diagnostics_by_uri;
    std::vector<std::string> open_uris;
    std::unordered_map<std::string, uint64_t> uri_versions;
};

/// slang's own `CompilationOptions::errorLimit` default.  Once elaboration has
/// produced more than this many errors slang sets `sawFatalError` and abandons
/// the rest of the pass, so every diagnostic it had not reached yet -- type
/// mismatches, width checks, unused-variable warnings -- is silently never
/// produced.  A design with one stray `timescale` reaches that limit on
/// `MissingTimeScale` alone, so the cutoff is worth being able to raise.
inline constexpr uint32_t kDefaultCompilationErrorLimit = 64;

/// How long schedule() waits for the edits to stop before compiling.
///
/// Not user-configurable, and deliberately not per project: it is one timer on
/// one worker, so a session with two projects open would have had to pick one
/// project's answer anyway.  Rapid typing pushes the window out, so what this
/// really sets is how long the user must pause before the heaviest thing the
/// server runs is allowed to start.
inline constexpr std::chrono::milliseconds kCompilationDebounce{1500};

/// Worker count and thread priority are not user-configurable: there is one
/// worker.  It compiles the whole design rather than sharing one compile, so a
/// second worker would only let a newer snapshot start before an older one
/// finishes -- at the cost of a duplicated full-design compilation whose result
/// the generation check usually discards.  CPU count is not the binding
/// resource here, peak memory is.
struct BackgroundCompilerConfig {
    bool enabled{false};
    /// Forwarded to slang's `CompilationOptions::errorLimit`.  0 means
    /// unlimited (slang's own encoding for "no limit").
    uint32_t error_limit{kDefaultCompilationErrorLimit};
};

class BackgroundCompiler {
  public:
    using SnapshotCallback = std::function<CompilationSnapshot()>;
    using ResultCallback = std::function<void(BackgroundCompileResult)>;

    BackgroundCompiler(SnapshotCallback snapshot_callback, ResultCallback result_callback);
    ~BackgroundCompiler();

    BackgroundCompiler(const BackgroundCompiler&) = delete;
    BackgroundCompiler& operator=(const BackgroundCompiler&) = delete;

    void configure(BackgroundCompilerConfig config);
    /// Request a semantic compile generation, once the edits have stopped for
    /// kCompilationDebounce.
    ///
    /// This is intentionally a lightweight trigger: the expensive full-design
    /// CompilationSnapshot is constructed by the worker only after the debounce
    /// window expires.  Rapid edits therefore coalesce before walking the full
    /// filelist / open-document set.
    void schedule();
    /// Request one with no coalescing window at all.
    ///
    /// For a caller that schedules exactly once and blocks for the result -- a
    /// CLI lint run, a test -- where there is no next keystroke to wait for and
    /// the window is pure latency.  Having nothing to coalesce is a property of
    /// the call, which is why it is spelled here rather than as a setting.
    void compile_now();
    void stop();

  private:
    void worker_loop();
    void schedule_in(std::chrono::milliseconds delay);
    BackgroundCompileResult compile(uint64_t generation, CompilationSnapshot snapshot) const;
    void compile_group(const CompilationSnapshot& snapshot, const CompilationGroup& group,
                       BackgroundCompileResult& result) const;

    SnapshotCallback snapshot_callback_;
    ResultCallback result_callback_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_{false};
    bool enabled_{false};
    // Read by compile() on worker threads without holding mutex_.
    std::atomic<uint32_t> error_limit_{kDefaultCompilationErrorLimit};
    uint64_t latest_generation_{0};
    bool pending_{false};
    std::chrono::steady_clock::time_point due_time_{};
    // Started by the first configure() that enables compilation; joined by stop().
    std::thread worker_;
};
