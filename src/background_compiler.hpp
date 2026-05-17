#pragma once

#include "analyzer.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

struct BackgroundCompileResult {
    uint64_t generation{0};
    std::unordered_map<std::string, std::vector<ParseDiagInfo>> diagnostics_by_uri;
    std::vector<std::string> open_uris;
};

class BackgroundCompiler {
  public:
    using ResultCallback = std::function<void(BackgroundCompileResult)>;

    explicit BackgroundCompiler(ResultCallback callback);
    ~BackgroundCompiler();

    BackgroundCompiler(const BackgroundCompiler&) = delete;
    BackgroundCompiler& operator=(const BackgroundCompiler&) = delete;

    void configure(bool enabled, int thread_count, int debounce_ms, bool log_timing);
    void schedule(CompilationSnapshot snapshot);
    void stop();

  private:
    void worker_loop();
    BackgroundCompileResult compile(uint64_t generation, CompilationSnapshot snapshot) const;

    ResultCallback callback_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_{false};
    bool enabled_{false};
    bool log_timing_{false};
    int debounce_ms_{1500};
    uint64_t latest_generation_{0};
    std::optional<CompilationSnapshot> pending_;
    std::chrono::steady_clock::time_point due_time_{};
    std::vector<std::thread> workers_;
};
