# Commit-by-commit startup comparison — `perf/project-index-startup`

Every perf-relevant commit on the branch, rebuilt and re-measured on one machine
in one sitting, so the numbers are comparable to each other.  `PERF.md` records
how each change was found; this file records what each one is worth.

## Method

```bash
# per commit: checkout, build only the bench target, measure
cmake --build build --target index-bench -j12
tools/startup_bench.py <corpus> -r 3 [--cpus 0] --json
```

- Harness: `tools/startup_bench.py`, 3 runs per configuration, median reported.
- `000d603^` predates `tools/index_bench.cpp`, so the harness was grafted in from
  `000d603` for that measurement.  It only calls public `Analyzer` API and links
  no product code of its own, so it does not alter what is being measured.
- Machine: AMD Ryzen 5 5600X (6C/12T), 62 GB RAM, RelWithDebInfo (`-O2 -g`), page
  cache warm, otherwise idle.
- Corpora:
  - **opentitan** — `tests/rtl/opentitan`, 1082 shards, no `+incdir+` configured.
  - **synth60** — 60 generated modules, each `` `include ``-ing the same two headers
    totalling ~660 KB, one `+incdir+`, 8-deep paths.  Models the shape of the
    original HPC report.
- `shards` was identical at every commit and configuration (1082 / 60), which is
  the correctness canary: no speedup here comes from indexing less.

Read the four columns together.  Wall time at full CPU is what a workstation
feels; wall time at `--cpus 0` is what a one-core batch slice feels; user CPU is
total work regardless of cores; RSS decides whether a memory-capped node copes.

## Results

### opentitan (1082 shards)

| Commit | What changed | all CPUs | 1 CPU | user | maxRSS |
|---|---|---|---|---|---|
| `f0ccb75` | baseline (main tip) | 1765 ms | 1785 ms | 1.24 s | 257 MB |
| `000d603` | parallel pool, `setDisableProximatePaths`, normalize memo | **289 ms** | 1293 ms | 1.63 s | 378 MB |
| `56d1dbd` | retire config knobs | 291 ms | 1310 ms | 1.65 s | 387 MB |
| `fe97dfe` | drop `OMP_NUM_THREADS` from CPU budget | 285 ms | 1278 ms | 1.60 s | 389 MB |
| `910eb6d` | header text cache per burst | 292 ms | 1305 ms | 1.59 s | 386 MB |
| `8b057da` | intern reference strings *(reverted)* | 309 ms | 1399 ms | 1.77 s | **165 MB** |
| `98e7388` | revert interning | 290 ms | 1300 ms | 1.61 s | 383 MB |

### synth60 (60 files, ~660 KB shared header)

| Commit | all CPUs | 1 CPU | `OMP_NUM_THREADS=1` | user | maxRSS |
|---|---|---|---|---|---|
| `f0ccb75` | 1567 ms | 1549 ms | 1562 ms | 1.46 s | 227 MB |
| `000d603` | **304 ms** | 1510 ms | 1536 ms | 2.09 s | 436 MB |
| `56d1dbd` | 309 ms | 1547 ms | 1518 ms | 2.11 s | 436 MB |
| `fe97dfe` | 305 ms | 1499 ms | **305 ms** | 2.09 s | 436 MB |
| `910eb6d` | 302 ms | 1491 ms | 305 ms | 2.07 s | 438 MB |
| `8b057da` | 322 ms | 1687 ms | 322 ms | 2.29 s | **111 MB** |
| `98e7388` | 305 ms | 1483 ms | 304 ms | 2.07 s | 438 MB |

## What each commit actually bought

**`000d603` — the whole local speedup.** 6.1× on opentitan and 5.2× on synth60 at
full CPU.  At one CPU the same commit is only 1.36× (opentitan) and ~1.03×
(synth60), which separates its two halves cleanly: the pool is the wall-clock
win, and the canonicalization fixes are worth little on a warm local filesystem.
Their value is in syscall count, which this machine does not charge for.

Cost: peak RSS rose 257 → 378 MB on opentitan and 227 → 436 MB on synth60. That
is N workers each holding a transient AST. On a memory-capped node this is the
one regression in the branch, and it is the price of the 6× wall-clock win.

**`56d1dbd` — neutral**, as intended. Config removal, within run-to-run noise
(±2%) on every configuration.

**`fe97dfe` — invisible except where it matters.** Identical everywhere except
the `OMP_NUM_THREADS=1` column, where synth60 goes 1536 ms → 305 ms (**5.0×**).
That variable is set in HPC shell profiles for unrelated tools and inherited by
the editor, so before this commit those sites silently got a one-worker indexer
and none of `000d603`'s win.

**`910eb6d` — free, and not visible here.** Wall and CPU are unchanged within
noise; RSS +2 MB. The change removes filesystem work, not compute: successful
header opens on synth60 drop **120 → 2** single-threaded (measured with
`strace -e openat`). A warm local `open` costs ~1 µs, so 118 of them do not
register. On a network filesystem each is a round trip. Correct read: this
commit buys nothing on a workstation and is intended for NFS/Lustre.

**`8b057da` — real memory win, real CPU loss, reverted.** RSS 263 → 165 MB on
opentitan and 228 → 111 MB (−51%) on synth60, at +8% wall and +10% user CPU. It
was reverted on the maintainer's call that startup CPU is the metric that
matters. Kept in history because the measurement is the useful part: reference
strings are ~87% of shard bytes at ~8× duplication, so a future attempt should
target that storage without paying a hash per occurrence.

**`98e7388` — clean revert.** Every column returns to `910eb6d` within noise,
confirming the revert left nothing behind.

## Branch total

| Corpus / configuration | `f0ccb75` | `98e7388` | Change |
|---|---|---|---|
| opentitan, all CPUs | 1765 ms | 290 ms | **6.1× faster** |
| opentitan, 1 CPU | 1785 ms | 1300 ms | 1.37× faster |
| synth60, all CPUs | 1567 ms | 305 ms | **5.1× faster** |
| synth60, 1 CPU | 1549 ms | 1483 ms | 1.04× faster |
| synth60, `OMP_NUM_THREADS=1` | 1562 ms | 304 ms | **5.1× faster** |
| opentitan peak RSS | 257 MB | 383 MB | 1.49× more |

## Caveats

- One machine, warm page cache, idle. Nothing here measures network-filesystem
  latency, which is the environment two of these commits target; their value is
  argued from syscall counts, not from these wall times.
- Medians of 3 runs. Run-to-run spread was under ~3%, so treat differences below
  that as noise — in particular `56d1dbd`, `910eb6d` and `98e7388` are
  indistinguishable from their predecessors on this hardware.
- User CPU rises with worker count (2.07 s at 8 workers vs 1.41 s at 1 on
  synth60): parallelism costs total CPU to buy latency. Relevant on a shared
  node where the cost is charged to other users.
- `--cpus 0` emulates a one-core slice via affinity. It reproduces the pool
  sizing and the serialization, but not a slower core or a loaded scheduler.
