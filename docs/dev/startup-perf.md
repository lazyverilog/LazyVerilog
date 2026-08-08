# Startup performance

Cold project-index startup is the work between reading `lazyverilog.toml` and
having a usable `ProjectIndexSnapshot`: parse every filelist entry, build one
`SyntaxIndex` shard per file, publish the merged snapshot.  It is the cost users
feel as "the editor is not answering yet".

## Measuring

```bash
cmake --build build -j$(nproc)
tools/startup_bench.py                      # default corpus, 3 runs
tools/startup_bench.py <project-root> -r 5  # any project with lazyverilog.toml
tools/startup_bench.py --cpus 0             # emulate a 1-CPU slice
tools/startup_bench.py --cpus 0 --trace     # per-file timings, slowest first
tools/startup_bench.py --json               # machine-readable
```

The script wraps `index-bench` and reports median index time, user/sys CPU, and
peak RSS across runs.  It also warns when the shard count varies between runs,
which is the cheapest available correctness canary: a change that speeds up
indexing by dropping work shows up here as a changed shard count.

## What to watch

Four numbers, not one:

| Metric | Why it matters |
|---|---|
| index ms at full CPU | what a developer workstation feels |
| index ms at `--cpus 0` | what a batch-scheduled or container-limited node feels |
| user CPU | total work done; unaffected by how many cores are available |
| maxRSS | grows with shared-header fan-in, and a memory-capped node thrashes |

Wall time alone hides regressions.  A change can leave wall time flat on a
12-core desktop while adding 10% user CPU, which lands squarely on a node that
granted the process one core.

## Properties worth knowing before optimizing

- **Worker count comes from the CPU slice**, not the machine: `available_cpu_count()`
  takes the minimum of the affinity mask, cgroup quota, and Slurm/LSF/PBS
  variables, capped at 8.  `--cpus` is the knob that reproduces a constrained
  node.  `OMP_NUM_THREADS` is deliberately *not* consulted; see `src/cpu_budget.hpp`.
- **File count is not the driver.**  Whole OpenTitan (1082 shards) costs about
  the same as 60 files that each `` `include `` one large shared header, because
  every including file re-preprocesses and re-indexes that header.
- **The tail dominates.**  A handful of generated `*_reg_top.sv` files are a large
  share of total time, so FIFO scheduling can strand one worker while others idle.
  `--trace` ranks them.
- **Shards duplicate header content by design.**  A header is a textual fragment,
  not a translation unit: it can be syntactically incomplete on its own
  (`module foo` with the port list in the includer) and its expansion depends on
  the includer's macro state.  Indexing it once standalone is therefore not
  correct.  See `PERF.md`.

## Related

- `PERF.md` — measured optimization rounds and their evidence.
- `docs/dev/indexing.md` — the AST-vs-index architecture the startup path serves.
- `tools/index_bench.cpp` — the binary this script wraps.
