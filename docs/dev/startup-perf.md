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
tools/startup_bench.py --warm               # keep the shard cache between runs
tools/startup_bench.py --no-cache           # shard cache off entirely
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

Runs are **cold** by default: the on-disk shard cache is cleared before each one,
which is what makes repeated runs comparable.  Leaving it in place makes run 1
cold and every run after it warm, and the median of that mixture is a warm number
wearing a cold label.  Add `--warm` to measure reuse deliberately and `--no-cache`
to measure the parse with the cache out of the picture; the mode is printed with
the results and carried in `--json`.

Use the same `CMAKE_BUILD_TYPE` on both sides — `Release` and `RelWithDebInfo`
differ enough to swamp the effect being measured — and read `maxRSS` alongside
the times: a per-file map that grows with header size shows up in memory first.

## What `--trace` cannot see

`LAZYVERILOG_TRACE_PERF=1` instruments `make_file_state_with_options()` — one
file's parse and index build.  Work the background loop does *around* that is
untraced, and `build_header_shards()` is the big one: on a UVM-shaped project it
was 90% of start-up while every trace line summed to a tenth of the wall time
(PERF.md round 8).  When the traced sum and the reported index time disagree,
the answer is not in the trace.  Sample the workers instead:

```bash
taskset -c 0 ./build/index-bench <corpus> 1 & pid=$!
sleep 1
for i in $(seq 1 25); do gdb -p $pid -batch -ex "thread apply all bt 25"; done \
  | grep '^#' | sed 's/^#[0-9]* *//; s/^0x[0-9a-f]* in //; s/ (.*//' \
  | sort | uniq -c | sort -rn | head -20
```

A `Release` build keeps its symbols, so this needs no rebuild, and one CPU keeps
the attribution clean.  `perf` is the better tool where it is available; in a
container without `perf_event` access this is what there is.

## Sampling says where, timers say how much

Use the stacks above to find out *what is running*, then time it before
believing a number.  Sampling attributes to whichever frame is on top, and a
short leaf function called very often collects samples out of all proportion to
the time it holds.  Twice on this code that produced a confident wrong answer:

| what sampling said | what a timer said |
|---|---|
| 13% of a cold start in slang's `SourceManager` lock, from the per-token line/column conversion | caching every one of those away — 434 345 hits against 4 351 misses — moved a single-core cold start from 822 ms to 821 ms |
| 25% in `collect_include_dependency_uris` | 4 ms of 542 |

The first is the trap in its purest form: an uncontended `std::shared_lock` is a
handful of instructions, so the samples were real and the conclusion was not.

`LAZYVERILOG_TRACE_PERF=1` therefore also reports phase totals, summed over
every worker, alongside the per-file lines:

```bash
LAZYVERILOG_TRACE_PERF=1 taskset -c 0 ./build/index-bench <corpus> 1 --cache off
# [lazyverilog][perf] index_build: 513ms
# [lazyverilog][perf] occurrences: 435ms
# [lazyverilog][perf] include_dependency_uris: 4ms
```

The phases nest — `occurrences` is inside `index_build` — and both are summed
across threads, so on a multi-core slice they exceed the wall time.  Add one in
`src/perf_trace.hpp` (an enum entry, a name, and a `ScopedPhase` where the work
is); with tracing off a timer reads the clock twice and adds, which does not
show up against the work it brackets.

## Where a cold start actually goes

Measured this way on 953 files of CVA6 and Ibex RTL, one core, uncached, so the
numbers are not divided across workers:

| | ms | share |
|---|---|---|
| whole cold start | 880 | |
| `SyntaxIndex::build` | 482 | 55% |
| — of which the occurrence collector | 410 | 47% |
| — — traversal and classification | 373 | 42% |
| — — building and recording the entries | 91 | 10% |
| slang's parse and preprocess | ~340 | 39% |

Two things follow.  The index build costs *more than the parse*, which is not
where the earlier rounds were looking.  And four fifths of it is the classifier
walking tokens and deciding what each identifier refers to — not the recording,
and not any single call that can be cached away.  The 417 568 reference entries
it produces cost about 220 ns each to construct, most of it in the canonical id
the classifier builds before `add_reference_entry()` ever sees it, so clearing
the strings at the point of storage recovers only 16 ms of the 91.

Splitting those numbers further needs another `ScopedPhase`, not another guess.

## Related

- `PERF.md` — measured optimization rounds and their evidence.
- `docs/dev/indexing.md` — the AST-vs-index architecture the startup path serves.
- `tools/index_bench.cpp` — the binary this script wraps.
