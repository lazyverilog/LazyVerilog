#pragma once

/// Number of CPUs this process may actually use.
///
/// std::thread::hardware_concurrency() reports the whole machine: libstdc++
/// implements it as sysconf(_SC_NPROCESSORS_ONLN) and MSVC as
/// GetMaximumProcessorCount(), and neither honours CPU affinity masks or
/// cgroup quotas.  On a batch-scheduled node or inside a container that
/// over-reports by a large factor, so sizing a worker pool from it oversubscribes
/// the slice the process was actually given.
///
/// This returns the smallest limit that any available signal reports:
///   * Linux CPU affinity mask (taskset, numactl, Slurm --cpu-bind)
///   * cgroup v2 cpu.max / cgroup v1 cpu.cfs_quota_us, including ancestors
///   * Windows process affinity mask
///   * batch scheduler environment (Slurm, LSF, PBS) and OMP_NUM_THREADS
///
/// Signals that do not exist on a platform are skipped, so an unconstrained
/// desktop falls through to hardware_concurrency() and behaves exactly as
/// before.  Never returns 0.
///
/// Deliberately excluded: system load average.  It is the only signal that
/// would change the result on an idle-but-busy desktop, and it makes the worker
/// count depend on whatever else the user happens to be running.
unsigned available_cpu_count();
