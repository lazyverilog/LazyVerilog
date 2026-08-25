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

- **A shared header must cost O(header) per file, never O(header) *extra* per file.**
  Re-parsing it once per includer is the price of correctness (previous bullet).
  Any *index* pass that also walks the whole header — the macro table, a
  per-declaration map key — is multiplied by the number of includers on the
  largest file in the project.  Two such passes regressed this way and are now
  pinned by tests; see below.

## Regression tests

Wall-clock benchmarks need a baseline to compare against and a quiet machine, so
they cannot gate a pull request.  These do, and run in CI as part of `ctest`:

```bash
./build/lazyverilog-tests "[scaling]"        # tests/test_shared_header_scaling.cpp
```

- *a module-scoped typedef adds no scoped-lookup key* — structural, deterministic.
- *a macro-spelled type still resolves to its alias* — the behaviour the lazy
  alias table must keep.
- *index build cost is flat in unreferenced macro count* — a timing check written
  as a **ratio between two headers that differ only in the shape of their macro
  bodies**: same byte count, same macro count, same declarations, so parsing and
  every other pass cancel.  It reports the fastest of 11 `SyntaxIndex::build()`
  runs per side, because the cost being measured is a raised floor and runner
  noise only ever adds time.  Threshold is 2x; the healthy ratio is ~1.0 and the
  regression it was written for measured ~2.9.

Write new scaling guards the same way — a ratio against a structurally identical
input, minimum of N runs — rather than an absolute millisecond budget, which is
what makes them safe on a shared CI runner.

## Reproducing a shared-header project

`tests/rtl/hpc60` is the checked-in corpus for this shape (60 modules, one large
header).  For a variant — the header inside the module body, thousands of
`` `define ``s, a typedef-dominated header — generate one into a scratch
directory with a `lazyverilog.toml` pointing at its `.f` file and run
`startup_bench.py` against it, comparing two builds of `index-bench`:

```bash
git worktree add --detach /tmp/base <known-good-commit>
cmake -S /tmp/base -B /tmp/base/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/base/build --target index-bench -j$(nproc)

tools/startup_bench.py <corpus> -r 3 --cpus 0 --binary /tmp/base/build/index-bench --label base
tools/startup_bench.py <corpus> -r 3 --cpus 0 --label head
```

Use the same `CMAKE_BUILD_TYPE` on both sides — `Release` and `RelWithDebInfo`
differ enough to swamp the effect being measured — and read `maxRSS` alongside
the times: a per-file map that grows with header size shows up in memory first.

## Related

- `PERF.md` — measured optimization rounds and their evidence.
- `docs/dev/indexing.md` — the AST-vs-index architecture the startup path serves.
- `tools/index_bench.cpp` — the binary this script wraps.
