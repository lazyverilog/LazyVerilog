#include "cpu_budget.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#ifdef __linux__
#include <filesystem>
#include <fstream>
#include <sched.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr unsigned kUnlimited = 0;

unsigned to_cpu_count(long long value) {
    if (value <= 0)
        return kUnlimited;
    constexpr long long max_value = 1 << 20;
    return static_cast<unsigned>(std::min(value, max_value));
}

/// Batch schedulers publish the per-task CPU allocation, and OMP_NUM_THREADS is
/// the convention HPC users already set for every tool on the node.  Only plain
/// integer variables are read: SLURM_JOB_CPUS_PER_NODE can be written as
/// "4(x2)", which does not describe this task's share.
unsigned cpu_count_from_environment() {
    static constexpr const char* kVariables[] = {
        "SLURM_CPUS_PER_TASK", // Slurm
        "LSB_DJOB_NUMPROC",    // LSF
        "NCPUS",               // PBS Pro
        "PBS_NUM_PPN",         // Torque
        "OMP_NUM_THREADS",     // de-facto convention
    };

    unsigned limit = kUnlimited;
    for (const auto* name : kVariables) {
        const char* value = std::getenv(name);
        if (!value || !*value)
            continue;

        char* end = nullptr;
        const long long parsed = std::strtoll(value, &end, 10);
        if (end == value)
            continue;

        const unsigned count = to_cpu_count(parsed);
        if (count == kUnlimited)
            continue;
        limit = limit == kUnlimited ? count : std::min(limit, count);
    }
    return limit;
}

#ifdef __linux__

unsigned cpu_count_from_affinity() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return kUnlimited;
    return to_cpu_count(CPU_COUNT(&set));
}

/// Convert a quota/period pair into whole CPUs, rounding to nearest so that a
/// "1.5 CPU" container neither claims 2 nor collapses to 1 by accident.  A
/// sub-CPU quota still yields one worker: the pool cannot be empty.
unsigned cpus_from_quota(long long quota, long long period) {
    if (quota <= 0 || period <= 0)
        return kUnlimited;
    const long long cpus = (quota + period / 2) / period;
    return to_cpu_count(cpus > 0 ? cpus : 1);
}

unsigned cpus_from_cpu_max(const std::filesystem::path& file) {
    std::ifstream input(file);
    std::string quota;
    std::string period;
    if (!(input >> quota >> period))
        return kUnlimited;
    if (quota == "max")
        return kUnlimited;
    return cpus_from_quota(std::strtoll(quota.c_str(), nullptr, 10),
                           std::strtoll(period.c_str(), nullptr, 10));
}

unsigned cpus_from_cfs_files(const std::filesystem::path& dir) {
    std::ifstream quota_input(dir / "cpu.cfs_quota_us");
    std::ifstream period_input(dir / "cpu.cfs_period_us");
    long long quota = 0;
    long long period = 0;
    if (!(quota_input >> quota) || !(period_input >> period))
        return kUnlimited;
    if (quota < 0) // -1 means unlimited
        return kUnlimited;
    return cpus_from_quota(quota, period);
}

/// Walk from the process's own cgroup up to the mount root.  A quota is
/// commonly set on an ancestor slice rather than the leaf, and a delegated leaf
/// may not expose the file at all, so every level has to be consulted.
template <typename ReadLimit>
unsigned cpu_count_from_cgroup(const std::filesystem::path& root, const std::string& cgroup_path,
                               ReadLimit read_limit) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return kUnlimited;

    auto dir = root;
    if (!cgroup_path.empty() && cgroup_path != "/")
        dir = root / std::filesystem::path(cgroup_path).relative_path();

    unsigned limit = kUnlimited;
    for (;;) {
        const unsigned level = read_limit(dir);
        if (level != kUnlimited)
            limit = limit == kUnlimited ? level : std::min(limit, level);

        if (dir == root)
            break;
        auto parent = dir.parent_path();
        if (parent == dir || parent.string().size() < root.string().size())
            break;
        dir = std::move(parent);
    }
    return limit;
}

struct SelfCgroups {
    std::optional<std::string> unified; // cgroup v2, the "0::<path>" line
    std::optional<std::string> cpu;     // cgroup v1, the entry owning the cpu controller
};

bool controller_list_has_cpu(std::string_view controllers) {
    while (!controllers.empty()) {
        const auto comma = controllers.find(',');
        const auto name = controllers.substr(0, comma);
        if (name == "cpu")
            return true;
        if (comma == std::string_view::npos)
            break;
        controllers.remove_prefix(comma + 1);
    }
    return false;
}

SelfCgroups read_self_cgroups() {
    SelfCgroups result;
    std::ifstream input("/proc/self/cgroup");
    std::string line;
    // Each line is "hierarchy-ID:controller-list:cgroup-path".  cgroup v2 uses
    // a single entry with an empty controller list.
    while (std::getline(input, line)) {
        const auto first = line.find(':');
        if (first == std::string::npos)
            continue;
        const auto second = line.find(':', first + 1);
        if (second == std::string::npos)
            continue;

        const std::string controllers = line.substr(first + 1, second - first - 1);
        std::string path = line.substr(second + 1);
        if (controllers.empty())
            result.unified = std::move(path);
        else if (controller_list_has_cpu(controllers))
            result.cpu = std::move(path);
    }
    return result;
}

#endif // __linux__

#ifdef _WIN32

unsigned cpu_count_from_process_affinity() {
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        return kUnlimited;

    // Only the current processor group is visible here.  Machines with more
    // than 64 logical CPUs therefore under-report, which is harmless: callers
    // cap the worker count far below that anyway.
    unsigned count = 0;
    for (DWORD_PTR bits = process_mask; bits != 0; bits &= bits - 1)
        ++count;
    return to_cpu_count(count);
}

#endif // _WIN32

} // namespace

unsigned available_cpu_count() {
    unsigned limit = kUnlimited;
    const auto consider = [&limit](unsigned candidate) {
        if (candidate == kUnlimited)
            return;
        limit = limit == kUnlimited ? candidate : std::min(limit, candidate);
    };

#ifdef __linux__
    consider(cpu_count_from_affinity());
    const auto cgroups = read_self_cgroups();
    if (cgroups.unified) {
        consider(cpu_count_from_cgroup("/sys/fs/cgroup", *cgroups.unified,
                                       [](const std::filesystem::path& dir) {
                                           return cpus_from_cpu_max(dir / "cpu.max");
                                       }));
    }
    if (cgroups.cpu) {
        consider(cpu_count_from_cgroup("/sys/fs/cgroup/cpu", *cgroups.cpu, cpus_from_cfs_files));
    }
#endif

#ifdef _WIN32
    consider(cpu_count_from_process_affinity());
#endif

    consider(cpu_count_from_environment());

    // Every signal above describes a restriction on what this process may use,
    // so the machine itself is still the ceiling.  Environment variables are
    // the one source that can name a larger number than the hardware offers
    // (OMP_NUM_THREADS is routinely set optimistically); clamp rather than
    // trust it.
    if (const unsigned hardware = std::thread::hardware_concurrency(); hardware != 0) {
        if (limit == kUnlimited)
            return hardware;
        return std::min(limit, hardware);
    }
    return limit == kUnlimited ? 1u : limit;
}
