#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

/// Where the time inside one indexing pass goes, summed across every file.
///
/// `LAZYVERILOG_TRACE_PERF=1` already reports one line per file from
/// `make_file_state_with_options()`, which answers "which file was slow".  It
/// cannot answer "which part of indexing a file is slow", and that gap is what
/// made the question get answered by sampling stacks instead -- which was wrong
/// twice over on this code.  A sampled profile put 13% of a cold start in
/// slang's `SourceManager` lock and named the per-token line/column conversion
/// as the cause; caching every one of those away (434 345 hits, 4 351 misses)
/// moved a single-core cold start from 822 ms to 821 ms.  The same profile,
/// taken over the open-buffer path, put 25% in `collect_include_dependency_uris`;
/// timed directly it is 4 ms of 542.
///
/// Sampling attributes to whichever frame is on top, and short leaf functions
/// called very often -- an uncontended `shared_lock` is a handful of
/// instructions -- collect samples out of all proportion to the time they hold.
/// So the phases that matter are timed rather than sampled.  The counters are
/// relaxed atomics read once at the end; with tracing off, the timer reads the
/// clock twice and adds, which is not measurable against the work it brackets.
namespace perf_trace {

inline bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("LAZYVERILOG_TRACE_PERF");
        return value && *value && std::string_view(value) != "0";
    }();
    return on;
}

enum class Phase {
    IndexBuild,       ///< SyntaxIndex::build(), whole
    Occurrences,      ///< the reference/occurrence collector inside it
    IncludeDependencyUris,
    Count,
};

inline std::string_view name_of(Phase phase) {
    switch (phase) {
    case Phase::IndexBuild:            return "index_build";
    case Phase::Occurrences:           return "occurrences";
    case Phase::IncludeDependencyUris: return "include_dependency_uris";
    case Phase::Count:                 break;
    }
    return "?";
}

using Counters = std::array<std::atomic<long long>, static_cast<size_t>(Phase::Count)>;

inline Counters& totals() {
    static Counters counters{};
    return counters;
}

inline void add_ns(Phase phase, long long nanoseconds) {
    totals()[static_cast<size_t>(phase)].fetch_add(nanoseconds, std::memory_order_relaxed);
}

inline long long total_ms(Phase phase) {
    return totals()[static_cast<size_t>(phase)].load(std::memory_order_relaxed) / 1000000;
}

/// Times its scope into @p phase.  Does nothing when tracing is off.
class ScopedPhase {
public:
    explicit ScopedPhase(Phase phase) : phase_(phase), on_(enabled()) {
        if (on_)
            start_ = std::chrono::steady_clock::now();
    }
    ~ScopedPhase() {
        if (!on_)
            return;
        add_ns(phase_, std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start_)
                           .count());
    }

    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;

private:
    Phase                                 phase_;
    bool                                  on_;
    std::chrono::steady_clock::time_point start_;
};

/// One line per phase with a non-zero total, or an empty string when tracing is
/// off.  Phases nest, so the totals overlap: `occurrences` is part of
/// `index_build`, and both are summed over every thread that ran one.
inline std::string summary() {
    if (!enabled())
        return {};
    std::string out;
    for (size_t i = 0; i < static_cast<size_t>(Phase::Count); ++i) {
        const auto phase = static_cast<Phase>(i);
        const auto ms    = total_ms(phase);
        if (ms == 0)
            continue;
        out += "[lazyverilog][perf] ";
        out += name_of(phase);
        out += ": ";
        out += std::to_string(ms);
        out += "ms\n";
    }
    return out;
}

} // namespace perf_trace
