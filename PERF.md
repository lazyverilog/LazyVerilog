# Start-up Indexing Performance

## What the server does at start-up

`initialize` → `load_config` + `load_vcode` (cheap, synchronous) →
`Analyzer::set_project_config` clears caches, bumps the generation, and fills
`background_pending_files_` with every filelist entry → a **pool** of
`background_indexers_` threads pops paths concurrently → per file: fresh
`SourceManager`, `SyntaxTree::fromFile`, `SyntaxIndex::build(Declarations)`,
diagnostic collection → shard committed into `extra_cache_` → one debounced
`ProjectIndexSnapshot` publish after the queue drains *and* every worker goes
idle.

(The pool landed in round 4; rounds 1–3 below describe the single-threaded
server they were measured against.)

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

1. **Parallelize the background indexer** — ✅ **DONE (round 4)**. Worker pool
   sized from the CPU slice the process may actually use, capped at 8, nice'd.
   See "Round 4" below.

2. **Persistent on-disk shard cache (clangd-style)** — every launch reparses
   unchanged files. Serialize `SyntaxIndex` per file keyed by
   `(path, mtime+size, hash(defines+incdirs))`; on startup load hits, parse
   only misses. Biggest win for repeated startups on large designs.

3. **Cut redundant include I/O** — partially addressed in round 4. The
   *probe* cost is gone (`setDisableProximatePaths(true)` stopped slang
   canonicalizing every candidate path it tries), but each file still gets a
   fresh `SourceManager`, so shared headers are still opened and read once per
   source file. Caching raw header text once per generation (path→text, seed
   via `assignText`) remains open. Re-preprocessing per file is required for
   correctness; the disk I/O is not.

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

| Now | After | Landed as (round 4) |
|-----|-------|---------------------|
| `std::thread background_indexer_` | `std::vector<std::thread> background_indexers_` | as planned |
| `bool background_index_active_` | `int background_active_workers_{0}` | kept the name `background_index_active_`, changed to `int` |
| — | `std::unordered_set<std::string> background_in_flight_` | **not done** |
| — | `int background_index_threads_` (from config) | **no config**; `available_cpu_count()` at first use |

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

Config: *(superseded in round 4 — no knob was added.)* The pool sizes itself
from `available_cpu_count()` (`src/cpu_budget.cpp`), which is the CPU slice the
process may actually use rather than the size of the machine, capped at 8.
Workers renice themselves through `apply_background_thread_nice()` — the
codebase is deliberately conservative about CPU on shared/HPC machines, and
warm-up should stay a background citizen.
*Verify:* a 1-CPU budget must reproduce the baseline exactly; then measure
2/4/8.

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

---

# Round 2 analysis: where the remaining 3.37M `readlink` calls go

Investigated after the `SourceFileIdResolver` fix landed. Tooling: `strace`
7.0 and `perf` 7.1.6, both fetched as Arch packages and extracted into a
scratch dir (no root; `perf` also needs `libpfm` extracted alongside it).

**Headline: the premise that `collect_include_dependency_uris` is the next big
win is wrong.** It is 0.6% of remaining `readlink` calls and 0.17% of user
time. The real remainder is cache misses inside the resolver added in round 1.

## Measured baseline (post-round-1)

| Metric | Value |
|---|---|
| Wall / user / sys | 5.77 s / 2.90 s / 2.87 s |
| `readlink` | 3,366,123 (835 per file) |
| `openat` | ~1–2.6 per file |

Cost calibration, derived from the round-1 before/after pair (9.53M→3.37M
readlinks, 11.8 s→5.77 s wall): **≈0.97 µs of wall per `readlink`**, of which
≈0.79 µs is sys. Used for every prediction below.

## Per-corpus `readlink` rates (measured)

Three corpora, each run through the real analyzer path by placing a
`tests/rtl/opentitan` tree in the benchmark's cwd:

| Corpus | Files | `readlink`/file | `openat`/file |
|---|---|---|---|
| Synthetic, no macros or includes | 200 | **148** | 1.05 |
| `hw/ip/prim/rtl` (macro-light RTL) | 269 | 305 | 2.6 |
| `otbn/dv/uvm/env` (macro-heavy UVM) | 17 | **30,564** | 2.3 |
| Full OpenTitan | 4031 | 835 | ~1.5 |

`openat` per file is essentially constant across all four while `readlink`
varies 200×. Whatever drives the cost is therefore **not** file or include
count — it scales with something else entirely.

## Root cause (measured, stack-resolved)

That something is **macro-expansion buffers**. slang allocates a fresh
`BufferID` for every macro expansion, and `SourceFileIdResolver` keys its cache
on the raw `location.buffer()`. In macro-heavy code almost every token sits in
a distinct expansion buffer, so the cache misses on nearly every token and
re-canonicalizes a path that resolves to the same file every time.

`strace -k` on a 189-file macro-heavy corpus, background-indexer thread only,
53,599 `readlink` events, frames resolved with `gdb info symbol`:

| Share | Path |
|---|---|
| **96.3%** | `SourceFileIdResolver::for_location` → `uri_from_file_name` → `uri_from_path` |
| 1.0% | `collect_parse_diagnostics` |
| 0.6% | `collect_include_dependency_uris` |
| 0.5% | slang `SourceManager::cacheBuffer` |
| 0.3% | slang `SourceManager::openCached` |

The 148/file floor from the synthetic corpus is the fixed per-file cost
(≈8 canonicalizations × ~18 path components). Full-corpus excess over that
floor is 835 − 148 = **687 readlinks/file ≈ 2.77M calls (82% of the
remainder)**.

## Why `collect_include_dependency_uris` is *not* the problem

It does canonicalize without memoization, twice over:

- the first loop scans `sm.getAllBuffers()` calling `uri_from_path` on each
  buffer until it finds the one matching `owning_uri` — a linear URI-compare
  scan just to identify the root buffer;
- the second loop calls `uri_from_path` again for each dependency buffer;
- and because every file gets a fresh `SourceManager`, nothing is reused across
  files even though the same headers recur thousands of times.

But it is cheap in practice: `sm.getFullPath()` returns empty for
macro-expansion buffers, so the `full_path.empty()` guard skips exactly the
buffers that are numerous. Only real file buffers reach `uri_from_path`, ~2–3
per file.

**Estimated saving if fully optimized: 36–54 readlinks/file ≈ 145k–218k calls
(4–6% of the remainder) ≈ 0.14–0.21 s wall, ~0.11–0.17 s sys, ~0.03–0.04 s
user.** Worth doing as cleanup, not as a performance play.

## Recommended fix for the real remainder

Two-level lookup in `SourceFileIdResolver::for_location`:

1. `by_buffer_` — existing integral fast path.
2. On miss, call `sm.getFileName(location)` and consult a new
   `by_name_` map keyed on that name. On hit, backfill `by_buffer_` and return.
3. Only on a name miss, canonicalize via `uri_from_file_name`, intern, and
   populate both maps.

**Why this is the safe design:** the resulting URI is a pure function of the
file name, so output is byte-identical — all expansion buffers of one file
already produce the same name and therefore the same URI. The tempting
alternative, keying on `sm.getFullyExpandedLoc(loc).buffer()`, is **not** safe:
it would change which file a macro-expanded token is attributed to when the
macro is defined in a different file, altering go-to-definition and references.

Store `std::string` keys rather than `string_view` (a few dozen per build) so
the cache cannot outlive SourceManager-owned storage.

**Predicted:** canonicalizations per file fall from `1 + #expansion buffers` to
`1 + #distinct file names`, i.e. toward the measured 148/file floor. Removing
~2.6M calls ≈ **2.5 s wall (≈2.0 s sys, ≈0.5 s user)**, taking 5.77 s → **~3.3 s**.

## Ranked opportunities

Impact is predicted; the evidence column says what it rests on.

| # | Change | Predicted saving | Risk | Confidence | Evidence |
|---|---|---|---|---|---|
| 1 | Name-keyed second level in `SourceFileIdResolver` | **~2.5 s** | Medium — touches every file-id lookup; needs full test run | **High** | 96.3% stack attribution + 148/file floor + µs/call calibration |
| 2 | Process-wide memo inside `normalize_filesystem_path` — ✅ **DONE (round 4)** | ~0.3 s (subsumes 3 and 4) | Medium — process-lifetime staleness if symlinks change mid-session | Medium | Floor decomposition |
| 3 | `collect_include_dependency_uris`: pass the owning `BufferID` in (drop loop 1) and memoize loop 2 | 0.14–0.21 s | **Low** — caller already computes the root buffer | High | 0.6% attribution + 0.17% user time |
| 4 | Drop double canonicalization in `make_file_state_with_options` (`uri_from_path` re-normalizes an already-normalized path) | ~0.07 s | **Very low** — one-line | High | Floor decomposition (1 of ~8 canonicalizations) |
| 5 | slang `setDisableProximatePaths(true)` — ✅ **DONE (round 4)** | ~0.15 s | Medium-high — changes how slang spells buffer names; affects diagnostics and URI mapping | Medium | 0.8% attribution (`cacheBuffer` + `openCached`) |
| 6 | Skip `collect_parse_diagnostics` on the index path | <0.1 s | Low | Medium | 1.0% attribution; absent from user profile above 0.15% |

Items 3–6 together are worth roughly 0.5 s. Item 1 is worth five times all of
them combined and should be done first.

## User-time profile (measured, full corpus)

`perf record -e cpu-clock:u`, 2925 samples. **No dominant hotspot** — the
profile is flat, so after item 1 there is no further single-change win on the
user side:

| Share | Symbol |
|---|---|
| 4.1% | `mi_new` (mimalloc) |
| 3.4% | `add_reference_entry` |
| 3.0% / 1.7% / 1.7% | `readlink` / `realpath` / `path::_M_split_cmpts` — 6.4% combined, the user-side half of canonicalization |
| 3.4% / 2.8% / 2.5% | slang `visitSyntaxNode` / `getChildCount` / `Lexer::lexToken` |
| 1.9% / 1.6% | slang `getRawLineNumber` / `getColumnNumber` |

Roughly 15% is slang lex/parse/visit and ~5% is our reference indexing. Beyond
item 1, the next real lever on user time is parallelization (deferred), not
micro-optimization.

DWARF call-graph unwinding was attempted for full-corpus caller attribution and
**failed** — the Release build yields ~1.4 usable frames per sample. The
attribution above therefore comes from `strace -k` on a smaller corpus; treat
the 96.3% as measured on macro-heavy code and the full-corpus split as inferred
from the floor arithmetic.

## Recommended next steps

1. Implement item 1; re-measure wall/user/sys and `readlink` count; confirm
   `indexed_shards=3999` and all 454 tests still pass.
2. Then items 4 and 3 (cheap, low risk) as a single cleanup commit.
3. Re-evaluate items 2 and 5 only if the numbers after step 1 justify the risk.
4. Revisit parse/index parallelization once canonicalization is no longer
   distorting the profile — at ~3.3 s with a flat user profile, the thread pool
   becomes the dominant remaining lever.

Note for that work: `collect_include_dependency_uris` calls
`sm.getAllBuffers()`, which slang documents as **not thread safe**. It is safe
today only because each worker owns its `SourceManager`; any design that shares
one across threads must revisit this.

---

# Round 3: item 1 applied — measured result and a corrected prediction

## What changed

`SourceFileIdResolver` now does a two-level lookup: `by_buffer_` (integral fast
path) in front of a new `by_name_` keyed on `sm.getFileName(location)` with a
transparent hash, so the name lookup needs no per-miss allocation.
Canonicalization runs only on a name miss. Output is unchanged by construction
— the URI is a pure function of the file name.

## Measured

| Metric | Round 1 | Round 2 (item 1) | Change |
|---|---|---|---|
| Wall | 5.77 s | **4.78 s** | −17% |
| user | 2.90 s | 2.71 s | −0.19 s |
| sys | 2.87 s | 2.06 s | −0.81 s |
| `readlink` | 3,366,123 | 2,319,634 | −31% |
| maxRSS | 439 MB | 440 MB | — |
| shards | 3999 | 3999 | identical |

454/454 tests pass. Cumulative from the original baseline: **11.8 s → 4.78 s,
2.47× faster**, sys 7.75 s → 2.06 s.

## The prediction was wrong, and why

I predicted ~2.5 s saved; the actual figure is ~1.0 s. The mechanism was right
but the extrapolation was not.

On the macro-heavy corpus the fix does exactly what was predicted:

| Corpus | Before | After |
|---|---|---|
| 189-file macro-heavy | 745,533 readlinks / 13.64 s | **60,113 / 1.58 s** (−92%, 8.6× faster) |
| Full OpenTitan | 3,366,123 / 5.77 s | 2,319,634 / 4.78 s (−31%) |

**The error:** I measured caller shares on a macro-heavy corpus (where
`for_location` was 96.3%) and applied that ratio to the whole corpus. The full
corpus is mostly *not* macro-heavy, so the resolver was never 96% of its
readlinks. The 148/file floor arithmetic was sound; the attribution transfer
was not. Lesson for the next round: attribute on the corpus you intend to
predict for, even when tracing it is more expensive.

## Corrected attribution (full corpus, post-fix)

`strace -k`, background-indexer thread, 51,110 sampled events, frames resolved
with `gdb info symbol`:

| Share | Caller |
|---|---|
| **46%** | `collect_parse_diagnostics` → `uri_from_file_name` → `uri_from_path` |
| 12% | `SourceFileIdResolver::for_location` (genuine first-time misses) |
| 12% | `collect_include_dependency_uris` |
| 9.5% | slang `SourceManager::cacheBuffer` |
| 3.5% | slang `SourceManager::openCached` |
| 3.5% | `make_file_state_with_options` → `normalize_filesystem_path` |

Separately, the main thread spends ~78k readlinks (3% of the total) in
`Analyzer::set_extra_files` normalizing 4031 filelist paths once at startup.

## Revised ranking of what is left

| # | Change | Predicted saving | Risk | Confidence |
|---|---|---|---|---|
| A | Skip `collect_parse_diagnostics` on the index path (results are discarded — the background loop commits only `state->index`) | **~0.9 s** | Low | Medium-high |
| B | `collect_include_dependency_uris`: pass the owning `BufferID` in, memoize loop 2 | ~0.25 s | Low | High |
| C | slang `setDisableProximatePaths(true)` (`cacheBuffer` + `openCached` = 13%) — ✅ **DONE (round 4)** | ~0.25 s | Med-high | Medium |
| D | Drop double canonicalization in `make_file_state_with_options` | ~0.07 s | Very low | High |

**Important caveat on A:** this benchmark configures no include directories, so
all 3148 `` `include `` directives fail and generate a diagnostic each. A real
`+incdir+` setup produces far fewer diagnostics, so A's real-world saving is
likely smaller than 0.9 s. The underlying waste is real either way — the
background index path formats and canonicalizes diagnostics it then throws
away — but the measured number here is an upper bound, not a typical case.

Ranking A–D by confidence-adjusted value, A is still first, but it should be
measured with include directories configured before being treated as a ~0.9 s
win.

---

# Round 4: parallel pool + the rest of the canonicalization tail

Branch `perf/project-index-startup`, commits `000d603`, `56d1dbd`, `528a9cc`.
Measured against `f0ccb75` (the round-3 endpoint) on the same machine.

Motivating report: ~60 RTL files took **11 s** to index on a shared HPC node,
with a single `+incdir+` and every module `` `include ``-ing two large headers.
Not reproducible on the dev workstation, so the work was driven by a synthetic
corpus of that shape plus syscall counting.

## What changed

**1. The indexer is a worker pool.** `background_index_active_` became a count,
the publish condition became `pending.empty() && active == 0`, and
`start_background_indexer_locked()` spawns up to `available_cpu_count()`
workers (cap 8), growing but never shrinking. Steps 1, 2 and 4 of the plan
above; step 3 (in-flight set) and step 5 (longest-first) are **not** done.

**2. Worker count comes from the CPU slice, not the machine.**
`std::thread::hardware_concurrency()` is `sysconf(_SC_NPROCESSORS_ONLN)` on
libstdc++ and `GetMaximumProcessorCount()` on MSVC — neither honours affinity
masks or cgroup quotas, so on a batch-scheduled or containerised node it
over-reports badly. `available_cpu_count()` (`src/cpu_budget.cpp`) takes the
minimum of every restriction that exists: Linux `sched_getaffinity`, cgroup v2
`cpu.max` / v1 `cpu.cfs_quota_us` walked from the leaf to the mount root,
Windows `GetProcessAffinityMask`, and the `SLURM_CPUS_PER_TASK` /
`LSB_DJOB_NUMPROC` / `NCPUS` / `PBS_NUM_PPN` / `OMP_NUM_THREADS` family.
Absent signals are skipped, so an unconstrained desktop lands on
`hardware_concurrency()` exactly as before.

Verified by counting live threads (peak = 1 main + N workers):

| Constraint | Workers |
|---|---|
| unconstrained (12 CPUs) | 8 (cap) |
| `taskset -c 0,1` | 2 |
| systemd `CPUQuota=200%` | 2 |
| systemd `CPUQuota=400%` | 4 |
| `SLURM_CPUS_PER_TASK=3` | 3 |
| `OMP_NUM_THREADS=99` | 8 (clamped to hardware) |

**3. `normalize_filesystem_path` memoizes** by input spelling (round-2 item 2).

**4. `setDisableProximatePaths(true)`** on every parse (round-2 item 5 /
round-3 item C). This is the one that matters for include-heavy designs: slang
runs `weakly_canonical()` on *each candidate path it probes* while resolving an
`` `include ``, and a probe that misses walks the entire directory prefix
through `canonical()` — about 10 `stat`s per miss on an 8-deep path, more on
the deeper trees typical of HPC checkouts. With N include directories that is
N canonicalisations per include per file.

Because the flag also makes `getFileName()` return a bare filename, URI
derivation moved onto buffer full paths (`uri_from_source_buffer()`), with
`getFileName()` still preferred when it already yields a `file://` URI so the
current document keeps its exact client spelling.

**5. Two config knobs retired** — `[compilation].background_compilation_threads`
and `[compilation].nice_value`, both fixed at their former defaults. Worker
nice is now one shared `apply_background_thread_nice()` that only ever *raises*
the value; the old code asked for a lower nice than a `nice`-started server
already had, failing with `EACCES` and logging once per worker.

## Measured

Ryzen 5 5600X (6C/12T), ext4 on NVMe, RelWithDebInfo, page cache warm, idle
machine, 3 runs each.

**OpenTitan corpus — the same benchmark rounds 1–3 used:**

| | f0ccb75 | 528a9cc |
|---|---|---|
| Wall | 4.92 / 4.94 / 4.90 s | **0.654 / 0.648 / 0.661 s** |
| indexed shards | 3999 | 3999 (identical) |

**7.5× faster.** Cumulative from the original 11.8 s baseline: **18×**.

**Synthetic 60-file corpus modelling the HPC report** — every module includes
`params.svh` + `func.svh`; `stat` counted with an `LD_PRELOAD` interposer:

| Corpus | f0ccb75 | 528a9cc | Speedup | `stat` before | `stat` after |
|---|---|---|---|---|---|
| 2 small headers, 1 incdir | 477 ms | 96 ms | 5.0× | 3,002 | 185 |
| 29k header lines, 1 incdir | 5,651 ms | 1,055 ms | 5.4× | 3,002 | 185 |
| 2 small headers, 31 incdirs | 554 ms | 103 ms | 5.4× | 46,232 | 3,815 |

## Which fix mattered for which cost

The two levers are close to orthogonal, and conflating them is easy:

- **Wall time on a warm local filesystem is all parallelism.** The
  normalization memo alone moved 60 files from 470 ms to 468 ms — 30% fewer
  `stat`s, ~0% wall — because a warm `stat` is ~1 µs, so 3,000 of them are 3 ms
  out of 5.2 s. Every second of local speedup came from the pool.
- **`stat` count is what a network filesystem charges for.** At NFS latencies
  the 46,232→3,815 reduction is the dominant term, and it is invisible here.

For the reported HPC case specifically — one `+incdir+`, huge headers — the
CPU term dominates: 5.65 s for that shape on a fast desktop core maps
plausibly onto 11 s on a slower shared one, at only ~3,000 `stat`s. The
syscall work pays off for wider `+incdir+` lists and deeper paths.

## Open risk: the resolver's second cache level

Round 2 argued that keying `SourceFileIdResolver` on
`getFullyExpandedLoc(loc).buffer()` is unsafe, and chose a name-keyed level
instead. Round 4 replaced that name key with an expanded-buffer key, because
`setDisableProximatePaths(true)` makes `getFileName()` return a bare filename —
which collides across two same-named headers in different include directories.

That trade swapped one hazard for another. `getFileName()` consults
`` `line `` directives, so it is a function of *(buffer, line)*, not of buffer
alone: a file carrying its own `` `line `` directive mid-file can legitimately
report different names on different lines, and an expanded-buffer key would
serve the first-seen URI for all of them. Generated RTL does emit `` `line ``.
The 457-test suite does not cover it.

The clean fix is to drop the second level entirely and rely on `by_buffer_`
plus the now-memoized `normalize_filesystem_path` — the memo removes the very
`weakly_canonical` cost the name level existed to avoid, so the second level
has become largely redundant. Not yet done.

## Still open from earlier rounds

- Round-3 item A (skip `collect_parse_diagnostics` on the index path, ~0.9 s
  upper bound), item B (`collect_include_dependency_uris`), item D (double
  canonicalization in `make_file_state_with_options`).
- Plan step 3: an in-flight path set. Two workers can now parse the same path
  concurrently when it is queued twice; the generation and `doc->second ==
  live_doc` checks make the second commit a no-op, so it is correctness-neutral
  but wasteful.
- Plan step 5: longest-first scheduling. Still FIFO, so the 627 ms tail file
  can strand one worker while the rest idle.
- Plan step 6: no ThreadSanitizer run has been done on the pool.
- Improvement 3's header text cache (each file still re-reads shared headers).

## Reproduce

```bash
cmake --build build -j$(nproc)
./build/lazyverilog-tests "benchmark: OpenTitan initial parse/index wall time"

# arbitrary project, without an editor
./build/index-bench <project-root-containing-lazyverilog.toml> 3
LAZYVERILOG_TRACE_PERF=1 ./build/index-bench <project-root> 1
```

# Round 5: the round-4 header cache had a scaling cost, plus four smaller leaks

Branch `perf/project-index-startup`, commits `bda38ec`, `2b4cf18`, `2991fb5`,
`3f43c92`, `f2b3b26`, `6a06623`.  Measured against `5b3a363` (the round-4
endpoint) on the same machine.  Per-commit numbers are in `PERF_COMP.md`.

This round started from a review of the branch rather than from a user report,
so the first job was building a corpus that could show what OpenTitan hides.

## The corpus OpenTitan was hiding

OpenTitan has 39 `.svh` files totalling 732 KB, so anything whose cost scales
with *total header bytes* is invisible there.  `hdr100` and `hdr200` are 300
generated modules that each `` `include `` two of 100 (9.7 MB) or 200 (19.6 MB)
distinct headers.  Same file count, same shard count; only the header
population differs — which makes a header-bytes term show up as the difference
between the two.

## 1. Seeding offered every cached header to every file (`3f43c92`)

Round 4's improvement 3 (`910eb6d`) caches header text for the duration of an
indexing burst so a shared header is read once instead of once per including
file.  `preload_cached_header_texts()` offered the **whole** cache to **every**
parse, and `slang::SourceManager::assignText` copies the text into that parse's
SourceManager.  Cost was therefore `O(files x total cached header bytes)`,
bounded only by the cache's 32 MB cap, while the benefit only ever applied to
the headers a file actually includes.

Measured by disabling the preload behind a temporary env flag:

| corpus | preload on | preload off |
|---|---|---|
| opentitan | 296 ms, 1.68 s user, 392 MB | 295 ms, 1.62 s user, 386 MB |
| hdr100 | 435 ms, 3.18 s user, 389 MB | 343 ms, 2.51 s user, 281 MB |
| hdr200 | 537 ms, 3.84 s user, 508 MB | 350 ms, 2.55 s user, 279 MB |

Doubling header bytes left the preload-off column flat and cost the preload-on
column +100 ms and +120 MB.  PERF_COMP.md's reading of `910eb6d` as "free, and
not visible here" was right for the corpora it was measured on and wrong in
general.

The fix keeps the cache and narrows the offer.  `HeaderTextCache` now counts
parses per burst and hits per header, and `seed_candidates()` returns only
headers that at least half the burst's parses included.  A header every file
includes stays on offer, which is the HPC shape the cache was built for; a
header that one file of hundreds included drops off, which is the fan-out that
cost the copies.  Deciding this per file from the file's own prior shard was the
other option, but that seeds nothing on a cold burst, which is exactly when the
saved reads matter.

The result beats both extremes — hdr200 lands at 342 ms against 350 ms with the
cache disabled outright — because shared headers are still seeded.

## 2. Shards were deep-copied under the global lock (`bda38ec`)

`background_index_loop()` did `SyntaxIndex committed_index = state->index;`
while holding `map_mutex_`.  `state` is a local `shared_ptr` dropped on the next
line, so this copied every vector and hash map in the shard — the largest object
the indexer produces — inside the one mutex every worker and every request
handler shares.  It is now moved out of the dying `DocumentState` and wrapped
*before* the lock is taken.

Worth 20-40 ms and 8-20 MB on every corpus, which makes it the second largest
item of the round despite being a one-line change.

## 3. Include fanout re-queued dependents per keystroke (`2991fb5`)

`change()` and `parse_worker_loop()` push every dependent open buffer and every
dependent shard onto `background_pending_files_` on each parse commit, and the
deque had no membership test.  A header included by 300 files queued 300
reparses per keystroke; ten characters queued 3000.  The same commit path bumps
`background_generation_` — discarding in-flight worker results and the header
cache — and re-arms the publish debounce, so a fast typist could keep the
project index from ever republishing.

`queue_background_file_locked()` now keeps a membership set beside the deque.  A
path leaves the set when a worker *pops* it rather than when the parse commits,
so an edit that lands mid-parse still re-queues the file.

This is the round-4 plan's step 3, recorded there as "correctness-neutral but
wasteful"; the per-keystroke amplification is the larger half of it.

## 4. A conditional expression that deep-copied a vector (`2b4cf18`)

Both dependent-scan loops read:

```cpp
entry.index ? entry.index->include_dependencies : std::vector<std::string>{}
```

The operands are a `const vector&` and a prvalue `vector`, so the common type is
a prvalue: every shard's dependency list was copied on every commit, under
`map_mutex_`.  1082 copies per edit on OpenTitan.  Now a null check and an
in-place read.

## 5. Diagnostics formatted for files that discard them (`f2b3b26`)

Round-3 item A, open until now.  `make_file_state_with_options()` always ran
`collect_parse_diagnostics()`, which renders every message through
`DiagnosticEngine::formatMessage` and resolves its line — and the background
index path keeps only `state->index` and drops the rest.  Now gated behind a
`collect_diagnostics` parameter, false from the indexer.

Round 3 estimated a 0.9 s upper bound.  The real figure depends entirely on how
many diagnostics the project produces: OpenTitan parses clean and moves ~9 ms,
while `hdr200` moves 385 -> 344 ms and 2.86 -> 2.56 s of user CPU.  A checkout
with missing defines or `+incdir+` entries is the case that pays.

## 6. Parse config re-copied per file (`6a06623`)

The dequeue block copied `defines_` and `include_dirs_` for each of the
thousands of filelist entries, inside `map_mutex_`.  Every writer of those
vectors bumps the background generation before any later work can be queued, so
the worker now keeps one copy per generation.  Flat on these corpora, which
configure 0 defines and 1 include dir; it scales with config size, not with file
count.

## Measured

Ryzen 5 5600X (6C/12T), ext4 on NVMe, RelWithDebInfo, page cache warm, idle
machine, 3 runs each, `5b3a363` -> `6a06623`:

| Corpus | all CPUs | 1 CPU | user | maxRSS |
|---|---|---|---|---|
| opentitan (1082 shards) | 305 -> **279 ms** | 1340 -> **1301 ms** | 1.67 -> **1.61 s** | 384 -> **374 MB** |
| hdr100 (300 shards) | 443 -> **340 ms** | 1990 -> **1781 ms** | 3.21 -> **2.52 s** | 391 -> **275 MB** |
| hdr200 (300 shards) | 571 -> **342 ms** | 2243 -> **1795 ms** | 4.07 -> **2.54 s** | 509 -> **285 MB** |
| shared2 (60 shards) | 206 -> **190 ms** | 948 -> **954 ms** | 1.45 -> **1.34 s** | 269 -> **250 MB** |

Shard counts were identical at every commit and configuration.

hdr200 is the headline: 1.7x faster and 1.8x smaller, and it no longer costs
more than hdr100, so the header-bytes term is gone rather than reduced.

Items 3 and 4 are edit-path costs.  A cold-startup benchmark queues each file
once and applies no edits, so both measure flat here by construction; their
evidence is the code path, not these numbers.

## What was investigated and left alone

`normalize_filesystem_path()` memoizes behind one process-global mutex and
allocates twice per hit, and it is called from the per-file dequeue while
`map_mutex_` is already held.  It looked like a candidate for why eight workers
buy 4.5x rather than 8x.  Counting the calls with a temporary atomic settled it:
**31,216 calls for a whole OpenTitan run**, about 29 per file, so at most ~5% of
user CPU including the allocations, and negligible contention at that rate.
Left unchanged.

## Still open from earlier rounds

- Round-3 item B (`collect_include_dependency_uris`), item D (double
  canonicalization in `make_file_state_with_options`).
- The resolver's second cache level and its `` `line `` hazard (round 4).
- Plan step 5: longest-first scheduling.  Still FIFO, so the tail file can
  strand one worker while the rest idle.
- Plan step 6: no ThreadSanitizer run has been done on the pool.

## Reproduce

```bash
cmake --build build -j$(nproc)
tools/startup_bench.py                      # opentitan, full CPU
tools/startup_bench.py --cpus 0             # one-CPU slice
```

The `hdr100` / `hdr200` / `shared2` corpora are generated, not checked in: N
modules each including two of M headers, one `+incdir+`.  Any generator of that
shape reproduces the header-bytes term.  The property that matters is many
distinct headers with low per-header fan-in, which is what separates hdr100 and
hdr200 from `shared2`.

# Round 6 plan: a file's header shards cost one whole-tree walk each

Not a regression — the cost has been there since header shards were introduced.
It is invisible on every corpus rounds 1–5 used, and it dominates any project
that carries UVM.  Found while looking for the next startup win; measured on
this branch, and reproduced identically on `feat/uvm-support`, so it is not
specific to either.

## Symptom

A one-entry filelist containing only `demo/uvm-core/src/uvm_pkg.sv`:

| | |
|---|---|
| index time | **15.2 s** |
| user / sys | 15.03 s / 0.42 s |
| maxRSS | 441 MB |
| shards | 166 |

`LAZYVERILOG_TRACE_PERF=1` attributes only ~190 ms of that to
`make_file_state_with_options` — the parse and the file's own shard.  The rest
is untraced.

A 20-file UVM testbench (each file `` `include "uvm_macros.svh" `` and
`import uvm_pkg::*`, plus `uvm_pkg.sv` on the filelist) costs **13.3 s** with
user ≈ wall: one worker sits on `uvm_pkg.sv` for the whole startup while the
other cores idle.

## Cause

`background_index_loop()` builds one shard per `` `include ``d header, and each
one re-walks the *entire includer tree* (`src/analyzer.cpp:4351`):

```cpp
for (const auto& header_uri : headers_to_build) {
    auto header_index = std::make_shared<SyntaxIndex>(
        SyntaxIndex::build(*state->tree, header_source_text(...),
                           IndexDepth::Declarations, header_uri));
    ...
}
```

`restrict_to_uri` decides which entries are *kept*, not how much tree is
*visited*.  The filter is five gate points — `accepts()` at
`src/syntax_index_shared.cpp:641,773,1425` and `wants_declaration()` at
`src/syntax_index.cpp:510,560` — and everything else in `build()` runs in full
regardless.  So a file that includes N headers is walked N+1 times.

Cost is therefore **O(tree_size × include_count)** for a single file.
`uvm_pkg.sv` has 165 include dependencies, so its AST is walked 166 times.

### Confirmed on both axes

Synthetic corpus: one package that `` `include ``s M one-line headers and
declares C classes.  Header bytes are negligible, so only the *count* varies:

| headers | classes | index time |
|---|---|---|
| 10 | 3000 | 281 ms |
| 100 | 3000 | **2349 ms** |
| 100 | 500 | 332 ms |
| 100 | 6000 | **5230 ms** |

10× the headers on an unchanged body: 8.4×.  12× the body at an unchanged
header count: 15.8×.  The cost is the product, which is the signature of a
per-header whole-tree walk.

### Why rounds 1–5 missed it

Every corpus used so far has low include *fan-out per file*: `hpc60` is one
header per file, `hdr100`/`hdr200` are two per file, OpenTitan averages under
one resolved include per file with no `+incdir+` configured.  Rounds 4–5
studied header **fan-in** (one header included by many files) and fixed it
properly.  Fan-out — one file including many headers — was never on the bench.
`uvm_pkg.sv` at 165 is the shape that exposes it, and it is the single most
common file in the SystemVerilog verification world.

This is the rule `CLAUDE.md` already states, in the transposed direction: an
index pass that walks the whole tree per header multiplies the largest file in
the project by that file's include count.

## The prize (measured, not predicted)

A probe that parses `uvm_pkg.sv` once and then times the two strategies over
the same tree:

```
parse:            309 ms
own restricted:   120 ms   (deps=165)
one unrestricted: 254 ms   (covers all 163 files)
header loop x165: 13782 ms
```

- current index work: 120 + 13782 = **13902 ms**
- one walk covering the same 163 files: **254 ms**

**55× on the index phase**, ~25× end-to-end for this file once the 309 ms parse
is counted.

## Fix: scope the walk to a set, then partition

One walk that collects every in-scope file's entries, split afterwards into
per-file shards.  Two small pieces:

**1. `SourceFileIdResolver` takes a scope set instead of one URI.**
`only_file_id_` becomes a set of `SourceFileID`, and `accepts()` becomes a
membership test instead of an equality test.  Three call sites, all mechanical.
`wants_declaration()` keeps the mentions filter for the *owner* file only —
headers keep everything they declare, which is the existing rule.

**2. Partition the resulting index by `file_id`.**  Every source-backed entry
already stores one (`src/syntax_index.hpp:302`), so this is a pure data
transform over finished tables — no walk changes.  Each shard gets its entries,
a remapped `source_files` table, and its own `include_dependencies`.
`SyntaxIndex::merge()` already remaps `SourceFileID`s, so the remap machinery
exists and is tested.

The indexer flow becomes: parse → read include dependencies off the tree →
claim unbuilt headers under `map_mutex_` (unchanged) → **one** walk scoped to
{own file} ∪ {claimed headers} → partition → commit.  Claiming still happens
before the walk, so headers another worker already owns are never collected,
and the walk stays cheaper than the unrestricted probe above.

`make_file_state_with_options()` keeps its current single-shard behaviour for
open buffers; only the background indexer asks for the multi-shard form.

### Why this preserves output

- A header shard built by the current code keeps every declaration in that
  header, because `scoped_text` is empty for it and the mentions filter never
  engages.  Partitioning by `file_id` gives exactly the same set.
- The owner's shard keeps its own entries plus foreign declarations whose names
  it mentions.  That subset is still decided by `wants_declaration()`, on the
  same tokens, during the same walk.
- Shard *count* is the cheapest correctness canary and `startup_bench.py`
  already warns on it; it must stay identical on every corpus.

### Risks

- **Transient memory.** One scoped walk holds every in-scope file's entries at
  once, where the loop held one file's at a time and released it.  The probe's
  unrestricted build covers 163 files; measure `maxRSS` before and after, since
  a memory-capped node is exactly the environment round 4 was written for.
- **Owner-vs-header ambiguity for macro-expanded declarations.** `for_token()`
  and `for_declaration_token()` deliberately disagree about which file a
  macro-expanded token belongs to.  Partitioning must use the same resolver the
  gate points use, or a `` `uvm_component_utils `` member could land in the
  wrong shard.  This is the one place the change can be subtly wrong, and UVM
  is built entirely out of such macros — cover it with a test that indexes a
  class whose members come from a macro defined in another header.

## After this lands

Both remaining open items get their teeth back, because neither can matter
while one file costs 14 s:

- **Longest-first scheduling** (plan step 5, still open).  The UVM testbench
  above is one enormous file plus 20 small ones; FIFO strands it.  Once
  `uvm_pkg.sv` drops to ~0.6 s the queue is worth ordering.
- **Persistent on-disk shard cache** (improvement 2, still open).  UVM never
  changes between sessions and is the largest thing a verification project
  indexes, so it is the ideal cache hit.

## Reproduce

```bash
cmake --build build --target index-bench -j$(nproc)

mkdir -p /tmp/uvm1 && cd /tmp/uvm1
printf '[design]\nvcode = "lazyverilog.f"\ndefine = []\n\n[compilation]\nbackground_compilation = false\n' > lazyverilog.toml
UVM=<repo>/demo/uvm-core/src
printf "+incdir+$UVM\n$UVM/uvm_pkg.sv\n" > lazyverilog.f
<repo>/tools/startup_bench.py /tmp/uvm1 -r 3
LAZYVERILOG_TRACE_PERF=1 <repo>/build/index-bench /tmp/uvm1 1
```

The header-count and body-size tables come from a generated corpus: one package
including M one-line headers and declaring C classes, one `+incdir+`.  Any
generator of that shape reproduces the product term.

The probe is ~40 lines against `SyntaxIndex::build`: parse the file once, then
time (a) one restricted build on the file's own URI, (b) one unrestricted
build, and (c) a loop of one restricted build per entry of
`include_dependencies`.  Build it as a temporary `add_executable` alongside
`parse-bench` so it picks up the same flags; comparing (b) against (c) is the
whole argument.
