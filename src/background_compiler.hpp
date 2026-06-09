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

struct BackgroundCompilerConfig {
    bool enabled{false};
    int thread_count{1};
    int debounce_ms{1500};
    bool log_timing{false};
    int nice_value{10};
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
    /// Request a semantic compile generation.
    ///
    /// This is intentionally a lightweight trigger: the expensive full-design
    /// CompilationSnapshot is constructed by the worker only after the debounce
    /// window expires.  Rapid edits therefore coalesce before walking the full
    /// filelist / open-document set.
    void schedule();
    void stop();

  private:
    struct WorkerSlot;

    void worker_loop(std::shared_ptr<WorkerSlot> slot);
    std::vector<std::thread> collect_exited_workers_locked();
    BackgroundCompileResult compile(uint64_t generation, CompilationSnapshot snapshot) const;

    SnapshotCallback snapshot_callback_;
    ResultCallback result_callback_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_{false};
    bool enabled_{false};
    std::atomic<bool> log_timing_{false};
    int debounce_ms_{1500};
    int nice_value_{10};
    size_t next_worker_id_{0};
    uint64_t latest_generation_{0};
    bool pending_{false};
    std::chrono::steady_clock::time_point due_time_{};
    // Each worker owns a stable slot so configure() can request retirement
    // without relying on vector indices.  This matters when the user lowers
    // the configured thread count: excess workers should finish their current
    // compilation, then retire before taking another job.
    std::vector<std::shared_ptr<WorkerSlot>> workers_;
};
