#!/usr/bin/env python3
"""Measure cold project-index startup: the work the server does between reading
lazyverilog.toml and having a usable ProjectIndexSnapshot.

This wraps `index-bench` with resource accounting, repeat runs, and an optional
CPU-slice restriction, because startup cost is dominated by two things that a
single wall-clock number hides:

  * how many CPUs the indexer is actually allowed to use — a batch-scheduled or
    container-limited node gives far fewer than the machine has, and the pool
    sizes itself from that slice;
  * memory, which grows with shared-header fan-in rather than with file count.

Examples
--------
    tools/startup_bench.py                          # default corpus, 3 runs
    tools/startup_bench.py path/to/project -r 5
    tools/startup_bench.py --cpus 0                 # emulate a 1-CPU slice
    tools/startup_bench.py --trace                  # per-file timing summary
    tools/startup_bench.py --json                   # machine-readable output
"""
from __future__ import annotations

import argparse
import json
import os
import re
import resource
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = REPO_ROOT / "build" / "index-bench"
DEFAULT_PROJECT = REPO_ROOT / "tests" / "rtl" / "opentitan"

# index-bench prints: "run 0: 1234.5 ms, shards=1082, modules=816"
RUN_LINE = re.compile(r"run\s+\d+:\s+([0-9.]+)\s+ms,\s+shards=(\d+),\s+modules=(\d+)")
# LAZYVERILOG_TRACE_PERF=1 prints:
#   "[lazyverilog][perf] make_file_state_with_options file:///x: 12345us"
TRACE_LINE = re.compile(r"^\[lazyverilog\]\[perf\]\s+(.*):\s+(\d+)us$")


def run_once(cmd: list[str], env: dict[str, str]) -> dict:
    """Run one index-bench invocation and account for its resource usage.

    getrusage(RUSAGE_CHILDREN) is cumulative over all reaped children, so the
    deltas around this call are what belong to this run.
    """
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    wall_ms = (time.monotonic() - start) * 1000.0
    after = resource.getrusage(resource.RUSAGE_CHILDREN)

    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit(f"index-bench exited {proc.returncode}")

    shards = modules = 0
    reported_ms = None
    for line in proc.stdout.splitlines():
        if match := RUN_LINE.search(line):
            reported_ms = float(match.group(1))
            shards = int(match.group(2))
            modules = int(match.group(3))

    return {
        # index-bench times set_project_config + wait_for_idle; the wall figure
        # here also includes process start-up and config load.
        "index_ms": reported_ms if reported_ms is not None else wall_ms,
        "wall_ms": wall_ms,
        "user_s": after.ru_utime - before.ru_utime,
        "sys_s": after.ru_stime - before.ru_stime,
        # ru_maxrss is a high-water mark across all children, not a delta, so
        # report it as-is rather than subtracting.
        "max_rss_mb": after.ru_maxrss / 1024.0,
        "shards": shards,
        "modules": modules,
        "stderr": proc.stderr,
    }


def summarize_trace(stderr: str, top: int = 10) -> list[tuple[str, float]]:
    """Slowest per-file entries, in milliseconds.

    The tail is what matters: on OpenTitan the 100 slowest files are about half
    of total index time, so a single slow file can strand one pool worker.
    """
    entries: list[tuple[str, float]] = []
    for line in stderr.splitlines():
        match = TRACE_LINE.match(line.strip())
        if not match:
            continue
        label = match.group(1)
        # Pool workers write to the same unsynchronized cerr, so two entries can
        # splice into one line and yield a nonsense label and duration.  Drop
        # those instead of reporting them.  Use --cpus 0 for a clean trace.
        if "[lazyverilog]" in label:
            continue
        entries.append((label.rsplit("/", 1)[-1], int(match.group(2)) / 1000.0))
    entries.sort(key=lambda item: item[1], reverse=True)
    return entries[:top]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark cold project-index startup.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("project", nargs="?", default=str(DEFAULT_PROJECT),
                        help="project root containing lazyverilog.toml "
                             f"(default: {DEFAULT_PROJECT.relative_to(REPO_ROOT)})")
    parser.add_argument("-r", "--repeat", type=int, default=3,
                        help="number of runs (default: 3)")
    parser.add_argument("--cpus",
                        help="restrict to this CPU list via taskset, e.g. '0' or '0-3'. "
                             "Emulates the slice a batch scheduler or container grants.")
    parser.add_argument("--binary", default=str(DEFAULT_BINARY),
                        help=f"index-bench path (default: {DEFAULT_BINARY.relative_to(REPO_ROOT)})")
    parser.add_argument("--trace", action="store_true",
                        help="collect per-file timings and print the slowest files. "
                             "Combine with --cpus 0: pool workers share an unsynchronized "
                             "stderr, so a parallel run splices and loses trace lines.")
    parser.add_argument("--label", default="", help="tag printed with the results")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    args = parser.parse_args()

    binary = Path(args.binary)
    if not binary.is_file():
        raise SystemExit(f"{binary} not found — build first: cmake --build build -j$(nproc)")
    project = Path(args.project)
    if not (project / "lazyverilog.toml").is_file():
        raise SystemExit(f"{project}/lazyverilog.toml not found")

    cmd = [str(binary), str(project), "1"]
    if args.cpus:
        if not shutil.which("taskset"):
            raise SystemExit("--cpus needs taskset (Linux only)")
        cmd = ["taskset", "-c", args.cpus] + cmd

    env = dict(os.environ)
    if args.trace:
        env["LAZYVERILOG_TRACE_PERF"] = "1"
    # Worker count comes from the CPU slice, so leave scheduler variables alone
    # and let --cpus be the single knob that changes it.

    runs = [run_once(cmd, env) for _ in range(args.repeat)]

    shard_counts = {run["shards"] for run in runs}
    if len(shard_counts) > 1:
        sys.stderr.write(f"warning: shard count varied across runs: {sorted(shard_counts)}\n")

    result = {
        "label": args.label,
        "project": str(project),
        "cpus": args.cpus or "all",
        "repeat": args.repeat,
        "shards": runs[0]["shards"],
        "modules": runs[0]["modules"],
        "index_ms_median": statistics.median(r["index_ms"] for r in runs),
        "index_ms_min": min(r["index_ms"] for r in runs),
        "index_ms_max": max(r["index_ms"] for r in runs),
        "user_s_median": statistics.median(r["user_s"] for r in runs),
        "sys_s_median": statistics.median(r["sys_s"] for r in runs),
        "max_rss_mb": max(r["max_rss_mb"] for r in runs),
    }

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        head = f"startup: {project.name}"
        if args.label:
            head += f" [{args.label}]"
        print(f"{head}  cpus={result['cpus']}  runs={args.repeat}")
        print(f"  shards={result['shards']} modules={result['modules']}")
        print(f"  index   {result['index_ms_median']:8.1f} ms median "
              f"({result['index_ms_min']:.1f}–{result['index_ms_max']:.1f})")
        print(f"  user    {result['user_s_median']:8.2f} s")
        print(f"  sys     {result['sys_s_median']:8.2f} s")
        print(f"  maxRSS  {result['max_rss_mb']:8.0f} MB")

    if args.trace:
        slowest = summarize_trace(runs[-1]["stderr"])
        if slowest:
            print("\n  slowest files:")
            for name, ms in slowest:
                print(f"    {ms:8.1f} ms  {name}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
