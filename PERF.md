# Start-up Indexing Performance

## What the server does at start-up

`initialize` → `load_config` + `load_vcode` (cheap, synchronous) →
`Analyzer::set_project_config` clears caches, bumps the generation, and fills
`background_pending_files_` with every filelist entry → a **single**
`background_indexer_` thread pops one path at a time → per file: fresh
`SourceManager`, `SyntaxTree::fromFile`, `SyntaxIndex::build(Declarations)`,
diagnostic collection → shard committed into `extra_cache_` → one debounced
`ProjectIndexSnapshot` publish after the queue drains.

`initialized` only registers file watchers. Semantic background compilation
(`src/background_compiler.cpp`) is off by default; when enabled it parses the
whole design a second time.

## Improvements (ranked)

> **Ranking updated after profiling.** Item 0 below was found by investigating
> the sys-time anomaly and is now the highest-value change — bigger than
> parallelization, and it should land first. Items 1–6 were the original
> static-analysis ranking and are otherwise unchanged.

0. **Memoize `source_file_id_for_token`'s URI derivation** — ✅ **DONE**.
   Confirmed cause of 65% of start-up wall time (9.5M redundant `readlink`
   syscalls). Fixed by routing per-token lookups through the existing
   `SourceFileIdResolver`: **11.8 s → 5.77 s**. See "The sys-time
   investigation" below.

1. **Parallelize the background indexer** — `background_indexer_` is one thread
   (`src/analyzer.cpp` `start_background_indexer_locked`). Per-file parse is
   independent; commits are already generation-checked under `map_mutex_`.
   Worker pool like `BackgroundCompiler` (capped ~4, nice'd, HPC-safe).

2. **Persistent on-disk shard cache (clangd-style)** — every launch reparses
   unchanged files. Serialize `SyntaxIndex` per file keyed by
   `(path, mtime+size, hash(defines+incdirs))`; on startup load hits, parse
   only misses. Biggest win for repeated startups on large designs.

3. **Cut redundant include I/O** — each file gets a fresh `SourceManager`, so
   shared headers are opened and read once per source file — O(files ×
   includes) filesystem traffic (painful on NFS). Cache raw header text once
   per generation (path→text, seed via `assignText`). Re-preprocessing per file
   is still required for correctness; the disk I/O is not.

4. **Skip wasted diagnostics in the index path** —
   `make_file_state_with_options` calls `collect_parse_diagnostics` (formats
   every diagnostic message), but the background loop commits only
   `state->index`; the diagnostics are discarded. Add a flag to skip.

5. **Free micro-wins** —
   - `SyntaxIndex committed_index = state->index;` in `background_index_loop`
     copies the whole index; `state` is discarded right after →
     `std::move(state->index)`.
   - The loop copies `defines_`/`include_dirs_` and rebuilds the open-overlay
     vector per file; snapshot once per generation.

6. **Queue priority for perceived latency** — the startup queue is filelist
   order. Index the open file's include dependencies and same-directory files
   first so definition/hover works on the user's file before the whole design
   finishes.

## Baseline

Measured 2026-08-07 with the existing hidden benchmark:

```bash
./build/lazyverilog-tests "benchmark: OpenTitan initial parse/index wall time"
```

It calls `Analyzer::set_extra_files()` then `wait_for_background_index_idle()`
with the project-index publish debounce set to 0, so the number is pure
parse/index work.

- Corpus: `tests/rtl/opentitan`, 4031 `.sv/.svh/.v/.vh` files, 3999 shards
  indexed (32 files fail to parse and commit no shard).
- Machine: AMD Ryzen 5 5600X (6C/12T), ext4 on NVMe, Release build (`-O3`),
  page cache warm.
- No defines and no include directories are configured by this benchmark, so
  the corpus's 3148 `` `include `` directives go unresolved. A real `+incdir+`
  setup does strictly more work, and improvement #3 is **not** exercised here.
  (For scale: `prim_assert.sv` is included 569 times, `uvm_macros.svh` 232,
  `dv_macros.svh` 223.)

| Run | Wall | user | sys | maxRSS | CPU |
|-----|------|------|-----|--------|-----|
| 1 | 11.72 s | 4.04 s | 7.75 s | 446 MB | 99% |
| 2 | 11.97 s | 4.21 s | 7.82 s | 446 MB | 99% |
| 3 | 11.78 s | 4.09 s | 7.75 s | 448 MB | 99% |

Run-to-run spread is under 2.5%, so treat anything under ~0.3 s as noise.

### Where the time goes

- `make_file_state_with_options` accounts for 11.19 s of the 11.56 s traced run
  (97%) — start-up is per-file parse + index and essentially nothing else.
- CPU stays at 99%: one core. Eleven of twelve hardware threads are idle for
  the entire warm-up, confirming the single indexer thread.
- Per-file distribution (n=4032): mean 2.78 ms, p50 0.61 ms, p90 4.10 ms,
  p99 26.3 ms, max 627 ms.
- The tail dominates: the 100 slowest files are **53%** of total time, the 10
  slowest are **29%**. The worst are UVM env packages (`otbn_env_cov.sv`
  627 ms, `otbn_env_pkg.sv` 565 ms) and generated register files
  (`*_reg_top.sv`, 110–394 ms).

## The sys-time investigation — SOLVED

sys was 65% of wall (7.7 s vs 4.1 s user), which is not what a parse-bound
workload should look like.

### Root cause (CONFIRMED)

**`uri_from_path()` is called once per indexed token, and each call runs
`std::filesystem::weakly_canonical()`, which issues one `readlink()` syscall
per path component.**

The full run makes **9,532,009 `readlink` calls, 9,531,977 of which fail**
(99.9997% error rate) — about **2364 per indexed file**. At the measured
~0.81 µs per call that is ≈7.7 s, which accounts for the entire sys time.

The call chain, confirmed by stack-unwinding traces:

```
source_file_id_for_token()          src/syntax_index_shared.cpp:32
  → uri_from_source_location()      src/syntax_index_shared.cpp:24
  → uri_from_file_name()            src/syntax_index_shared.cpp:15
  → uri_from_path()                 src/string_utils.hpp:202
  → normalize_filesystem_path()     src/string_utils.hpp:103
  → std::filesystem::weakly_canonical → realpath() → ~18 × readlink()
```

The corpus paths are ~18 components deep, which is exactly the observed
readlink-per-canonicalization ratio.

The killer detail: `source_file_id_for_token` ends with
`index.intern_source_file(std::move(uri))`, and `SyntaxIndex::source_file_ids`
is already an interning map. **The cache exists, but it is consulted only
after the expensive URI has been built.** Every token in a buffer re-derives
the identical URI from scratch. (slang has the same bug shape in
`SourceManager::openCached`, which canonicalizes before its `lookupCache`
lookup — but that is only 7% of the calls here.)

Attribution from resolved stacks (5-file slang-only run, 2084 sampled stacks):

| Share | Caller |
|-------|--------|
| 60% | `source_file_id_for_token` ← `build_dynamic_file_index` |
| 13% | `collect_include_dependency_uris` |
| 10% | `source_file_id_for_token` ← `process_class` |
| 7% | slang `SourceManager::cacheBuffer` / `openCached` |

`source_file_id_for_token` is used by **both** index builders
(`src/syntax_index.cpp:92,207,…` and `src/dynamic_file_index.cpp:98,…`), so
the analyzer's background path (`SyntaxIndex::build`) pays the same cost —
consistent with the 2364/file measured through the analyzer and 3672/file
measured through pure slang in `parse-bench`.

### Ruled out (each tested, not assumed)

- **Disk I/O** — `majflt=0`, `inblock=0`; corpus entirely page-cache warm.
- **glibc trim/mmap churn** — `MALLOC_MMAP_THRESHOLD_`, `MALLOC_TRIM_THRESHOLD_`
  and `MALLOC_TOP_PAD_` raised to 1 GB changed nothing (11.66 s, sys 7.58 s).
- **Transparent huge pages** — THP is `always`, but only 26 `thp_fault_alloc`
  events during the run.
- **Page-fault handling** (the earlier leading hypothesis) — only ~159k minor
  faults, two orders of magnitude too few to explain 7.7 s.
- **Allocator mmap churn** — 34 `mmap`, 3 `munmap` in the entire run.

### Tooling

`perf`, `strace`, `ltrace`, `bpftrace` were all absent and `sudo` requires a
password, so nothing could be installed system-wide. Resolution: download the
Arch `strace` package and extract it into the scratchpad as an unprivileged
user — no root needed, since `kernel.yama.ptrace_scope=1` still permits
tracing one's own descendants.

```bash
curl -sL -o strace.pkg.tar.zst https://archlinux.org/packages/extra/x86_64/strace/download/
bsdtar -xf strace.pkg.tar.zst usr/bin/strace
./usr/bin/strace -c -f -o summary.txt ./build/lazyverilog-tests "benchmark: …"
./usr/bin/strace -k -f -e trace=readlink -o stacks.txt ./build/parse-bench files.f
```

`strace -c -f` gave the syscall histogram; `strace -k` gave stacks, resolved to
symbols with `nm` (the Release build has no `-g`, so `addr2line` returns
nothing and nearest-symbol lookup is required).

Two dead ends worth recording: an `LD_PRELOAD` syscall shim **undercounts
badly** (it saw ~13k calls out of 9.5M) because glibc-internal calls do not go
through the PLT; and a 45 s `strace -k` sample only covered
`Analyzer::set_extra_files`, because stack unwinding slows the run ~15×. Trace
a small corpus to completion rather than sampling the head of a big one.

### Fix — IMPLEMENTED and measured

The codebase already contained the right mechanism: `SourceFileIdResolver`
(`src/syntax_index_shared.hpp`), a per-build `BufferID -> SourceFileID` cache
whose doc comment describes this exact problem. **It had zero users** — all 36
call sites went through the uncached free function instead.

The change threads a build-local `SourceFileIdResolver` through both index
builders and routes every per-token lookup through it:

- `SyntaxIndex::build` and `build_current_ast_structural_index` /
  `build_dynamic_file_index` each create one resolver.
- Every helper that already took `(SyntaxIndex& index, const SourceManager& sm)`
  now also takes `SourceFileIdResolver& resolver`; local visitor structs
  (`LocalVariableVisitor`, `ImportVisitor`, `InstanceScan`, the `collect_imports`
  visitor) carry it as a member.
- `source_file_id_for_token` became unused and was deleted.

The resolver is deliberately **not** stored in `SyntaxIndex`: `SyntaxIndex::merge`
(`src/syntax_index.cpp`) remaps `SourceFileID`s, so a persisted buffer→id cache
would silently return remapped-away ids after a merge.

Measured on the same corpus and machine:

| | Before | After |
|---|---|---|
| Wall | 11.72 / 11.97 / 11.78 s | **5.77 / 5.80 / 5.75 s** |
| user | 4.1 s | 2.9 s |
| sys | 7.75 s | 2.87 s |
| `readlink` calls | 9,532,009 | 3,366,123 |
| maxRSS | 446 MB | 439 MB |
| indexed shards | 3999 | 3999 (unchanged) |

**2.04× faster**, sys time down 63%. All 454 tests pass; shard count and
benchmark output are identical.

### Remaining `readlink` traffic (deferred)

3.37M calls remain, from callers deliberately left alone for now:

- `collect_include_dependency_uris` — one `uri_from_path` per include
  dependency per file.
- slang's `SourceManager::openCached` / `isCached`, which canonicalize *before*
  their own `lookupCache` lookup. Addressable via
  `setDisableProximatePaths(true)`, but that changes how slang spells buffer
  names, so it needs test validation.
- `Analyzer::set_extra_files`, which normalizes every filelist path once at
  startup (~4031 canonicalizations — small in comparison).

A memo cache inside `normalize_filesystem_path` would cover all of these at
once, at the cost of process-wide staleness if symlinks change during the
session.

### Reproduce

```bash
cmake --build build -j$(nproc)
./build/lazyverilog-tests "benchmark: OpenTitan initial parse/index wall time"

# per-file timings
LAZYVERILOG_TRACE_PERF=1 ./build/lazyverilog-tests \
  "benchmark: OpenTitan initial parse/index wall time" 2>trace.txt
```

---

# Plan: parallelize the per-file parse/index pipeline

Do this **after** the `readlink` fix. Order matters: the current workload is
65% kernel time from a redundant syscall, and parallelizing that first would
just spend more cores on work that should not exist.

## Why it is safe

- **Per-file work is already independent.** `make_file_state_with_options`
  builds its own `SourceManager` per call and touches no shared mutable state.
- **slang supports it.** `SourceManager` is documented "thread safe unless
  otherwise noted" and guards itself with a `shared_mutex`. (`getAllBuffers()`
  is explicitly *not* thread-safe — the background path does not call it.)
- **Index builders hold no shared statics.** All state lives in the
  per-call `SyntaxIndex`; `source_file_ids` is a member, not a global.
- **`get_dynamic_index()` is already `std::call_once`-guarded**
  (`src/dynamic_file_index.cpp:701`), so two workers touching the same
  `DocumentState` cannot double-build.
- **Commits are already generation-checked** under `map_mutex_`, so a slow
  parse from an old config cannot overwrite newer state.

## State changes (`src/analyzer.hpp`)

| Now | After |
|-----|-------|
| `std::thread background_indexer_` | `std::vector<std::thread> background_indexers_` |
| `bool background_index_active_` | `int background_active_workers_{0}` |
| — | `std::unordered_set<std::string> background_in_flight_` |
| — | `int background_index_threads_` (from config) |

## Steps

**1. Replace the active flag with a counter.** `background_index_active_` is a
bool because there is exactly one worker. Change it to a count, and update
`wait_for_background_index_idle()`'s predicate to
`pending.empty() && active_workers_ == 0 && !publish_requested`.
*Verify:* `ctest` green, benchmark unchanged (~11.8 s, still one thread).

**2. Fix the publish condition — do this before adding threads.**
`background_index_loop` currently publishes when
`background_pending_files_.empty()` (`src/analyzer.cpp:4143`). With N workers
the queue empties while other workers are still parsing, so the project index
would publish a partially-warmed snapshot. It must become
`pending.empty() && active_workers_ == 0`, with the committing worker
decrementing its own count first.
*Verify:* new test asserting the publish callback fires exactly once for a
cold warm-up of a multi-file filelist.

**3. Add an in-flight set.** The same path can be queued twice (bulk warm-up
plus a `push_front` from dependent reindexing). Today the single worker
serializes that; with a pool two workers can parse the same file concurrently.
The generation + `doc->second == live_doc` checks make the second commit a
no-op, so it is correctness-neutral but wasteful. Skip paths already in
`background_in_flight_`; erase on commit.
*Verify:* test that queues one path twice and asserts it is parsed once.

**4. Spawn the pool.** `start_background_indexer_locked()` becomes idempotent
over N threads (spawn only the missing ones). Follow the existing
`BackgroundCompiler::WorkerSlot` pattern (`src/background_compiler.cpp:108`)
if graceful shrink-on-config-reload is wanted; a fixed pool sized at first use
is enough for v1. Destructor joins all; `background_stop_` + `notify_all`
already wake every waiter.

Config: add `[design] index_threads`, defaulting to
`min(4, max(1, hardware_concurrency / 2))`, and apply the existing
`nice_value` treatment from `apply_background_worker_priority()` — the codebase
is deliberately conservative about CPU on shared/HPC machines, and warm-up
should stay a background citizen.
*Verify:* `index_threads = 1` must reproduce the baseline exactly; then
measure 2/4/8.

**5. Schedule longest-first.** The tail is the whole game: 100 files are 53% of
the time and the worst single file is 627 ms. FIFO order will strand one
worker on `otbn_env_cov.sv` while the others idle.

Sort the queue by descending `std::filesystem::file_size()` when filling it.
Measured predictive power on this corpus: Spearman 0.78, and 58 of the
top-100-by-time files are also top-100-by-size. Imperfect —
`otbn_env_pkg.sv` is 5 KB but costs 565 ms because it pulls in siblings via
relative `` `include `` — but far better than filelist order. Cost is one
`stat` per entry at queue-fill time (~4031 stats, single-digit ms), which is a
background path and does not violate the no-metadata-on-request-path rule.

Keep `push_front` for interactive dependent-reindex requests so user edits
still preempt bulk warm-up.

Later, improvement #2's persistent cache can store the previous run's per-file
duration and make the ordering exact.

**6. Race validation.** Build with `-fsanitize=thread` and run the OpenTitan
benchmark plus the feature tests. This is the real acceptance gate — the
argument above is a code review, not proof.

## Expected results (predictions, to be measured)

| Configuration | Predicted wall |
|---|---|
| baseline today | 11.8 s |
| readlink fix only | ~5 s |
| readlink fix + 4 threads | ~1.5–2 s |
| 4 threads without readlink fix | ~3–4 s |

Speedup is capped by the 627 ms worst file and by Amdahl on the tail, so
expect noticeably less than 4× from 4 threads.

## Risks

- **Memory** — N× the transient per-file peak on top of the ~446 MB of
  accumulated shards. Measure maxRSS at each thread count; the biggest files
  (1.3 MB `pinmux_reg_top.sv`) dominate the transient.
- **`map_mutex_` contention** — commits are short (move a `shared_ptr`,
  invalidate snapshot caches), but every worker takes the same lock per file.
  If it shows up, batch commits.
- **CPU politeness** — a shared workstation or HPC login node should not see
  the LSP grab 4 cores unannounced; hence the conservative default and `nice`.
