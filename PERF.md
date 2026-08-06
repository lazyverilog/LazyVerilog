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
| 2 | Process-wide memo inside `normalize_filesystem_path` | ~0.3 s (subsumes 3 and 4) | Medium — process-lifetime staleness if symlinks change mid-session | Medium | Floor decomposition |
| 3 | `collect_include_dependency_uris`: pass the owning `BufferID` in (drop loop 1) and memoize loop 2 | 0.14–0.21 s | **Low** — caller already computes the root buffer | High | 0.6% attribution + 0.17% user time |
| 4 | Drop double canonicalization in `make_file_state_with_options` (`uri_from_path` re-normalizes an already-normalized path) | ~0.07 s | **Very low** — one-line | High | Floor decomposition (1 of ~8 canonicalizations) |
| 5 | slang `setDisableProximatePaths(true)` | ~0.15 s | Medium-high — changes how slang spells buffer names; affects diagnostics and URI mapping | Medium | 0.8% attribution (`cacheBuffer` + `openCached`) |
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
| C | slang `setDisableProximatePaths(true)` (`cacheBuffer` + `openCached` = 13%) | ~0.25 s | Med-high | Medium |
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
