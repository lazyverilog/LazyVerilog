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
///   * batch scheduler environment (Slurm, LSF, PBS)
///
/// Signals that do not exist on a platform are skipped, so an unconstrained
/// desktop falls through to hardware_concurrency() and behaves exactly as
/// before.  Never returns 0.
///
/// Deliberately excluded: system load average.  It is the only signal that
/// would change the result on an idle-but-busy desktop, and it makes the worker
/// count depend on whatever else the user happens to be running.
///
/// Also deliberately excluded: OMP_NUM_THREADS.  It states an OpenMP compute
/// budget rather than the CPU slice this process owns, and HPC shell profiles
/// commonly set it to 1 for unrelated tools; an editor inherits that and would
/// index with a single worker.
unsigned available_cpu_count();

/// Make the calling background worker thread yield to interactive work.
///
/// Linux keeps the nice value per thread, so this affects only the caller and
/// leaves the LSP request thread at its original priority.  It is a no-op
/// elsewhere: on macOS the same call is process-wide and would slow down
/// request handling along with background work, and Windows has no POSIX nice.
///
/// Only ever raises the nice value.  A server started under `nice` may already
/// sit above the requested value, and lowering one needs CAP_SYS_NICE, so
/// asking would fail and log once per worker.
void apply_background_thread_nice(int nice_value);
