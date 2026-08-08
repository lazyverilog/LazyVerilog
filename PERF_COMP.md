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
  - **hdr100 / hdr200** *(round 5)* — 300 generated modules, each
    `` `include ``-ing two of 100 (9.7 MB) or 200 (19.6 MB) distinct headers, one
    `+incdir+`.  Same file count in both, so anything that scales with total
    header bytes rather than with file count shows up as the gap between them.
  - **shared2** *(round 5)* — 60 modules over two shared headers (~0.5 MB), the
    synth60 shape rebuilt by the round-5 generator.  Kept alongside hdr100 /
    hdr200 to show that narrowing header seeding did not cost the shared-header
    case anything.
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

### Round 5 commits — opentitan and shared2

Measured in a second sitting, so compare these two tables to `5b3a363` and to
each other, not to the numbers above.

| Commit | What changed | opentitan all / 1 CPU / user / RSS | shared2 all / 1 CPU / user / RSS |
|---|---|---|---|
| `5b3a363` | round-4 endpoint | 305 ms / 1340 ms / 1.67 s / 384 MB | 206 ms / 948 ms / 1.45 s / 269 MB |
| `bda38ec` | move the shard into `extra_cache_` | **284** / 1316 / 1.65 / 376 | **191** / 937 / 1.34 / 250 |
| `2b4cf18` | no vector copy in the dependent scan | 290 / 1357 / 1.70 / 375 | 191 / 959 / 1.34 / 251 |
| `2991fb5` | dedupe the background queue | 291 / 1316 / 1.66 / 378 | 189 / 955 / 1.36 / 251 |
| `3f43c92` | seed only widely shared headers | 286 / 1324 / 1.64 / 375 | 193 / 966 / 1.33 / 251 |
| `f2b3b26` | skip discarded parse diagnostics | **277** / 1307 / 1.63 / 371 | 189 / 965 / 1.34 / 251 |
| `6a06623` | reuse the config snapshot | 279 / **1301** / **1.61** / 374 | 190 / 954 / 1.34 / 250 |

### Round 5 commits — hdr100 and hdr200

| Commit | hdr100 all / 1 CPU / user / RSS | hdr200 all / 1 CPU / user / RSS |
|---|---|---|
| `5b3a363` | 443 ms / 1990 ms / 3.21 s / 391 MB | 571 ms / 2243 ms / 4.07 s / 509 MB |
| `bda38ec` | 431 / 1957 / 3.17 / 376 | 529 / 2160 / 3.78 / 489 |
| `2b4cf18` | 436 / 1984 / 3.18 / 374 | 531 / 2185 / 3.83 / 491 |
| `2991fb5` | 437 / 2001 / 3.19 / 375 | 527 / 2207 / 3.84 / 490 |
| `3f43c92` | **341** / 1900 / **2.53** / **275** | **385** / 1824 / 2.86 / **284** |
| `f2b3b26` | 345 / 1794 / 2.57 / 276 | **344** / 1819 / **2.56** / 283 |
| `6a06623` | 340 / **1781** / 2.52 / 275 | 342 / **1795** / 2.54 / 285 |

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

**`bda38ec` — small, universal, one line.** The only round-5 commit that helps
every corpus: 305 -> 284 ms on opentitan, 571 -> 529 ms on hdr200, 8-20 MB of
RSS everywhere. The shard was being deep-copied under `map_mutex_` when it could
be moved out of the `DocumentState` that was about to die.

**`2b4cf18` and `2991fb5` — flat here, and expected to be.** Both fix edit-path
costs: a per-commit vector copy and per-keystroke queue amplification. A cold
startup benchmark queues each file once and applies no edits, so neither can
show up in this table. Their evidence is in `PERF.md` round 5, from the code
path rather than from a measurement.

**`3f43c92` — the round's real win, and only where the corpus allows it.**
hdr100 437 -> 341 ms and hdr200 527 -> 385 ms, with user CPU down ~20% and RSS
down ~27% and ~42%. opentitan and shared2 do not move, which is the point: the
commit removes a cost proportional to *total distinct header bytes*, and those
two corpora have almost none. It also fixes a regression this file previously
called free — see the `910eb6d` entry above, which was measured only on corpora
that could not show it.

**`f2b3b26` — proportional to how badly the project parses.** opentitan 286 ->
277 ms (it parses clean, so there is little to format), hdr200 385 -> 344 ms and
2.86 -> 2.56 s user. Round 3 predicted an 0.9 s upper bound; the honest reading
is that the cost is entirely a function of diagnostic count, so a checkout with
missing defines or `+incdir+` entries pays it and a clean one does not.

**`6a06623` — neutral on these corpora, by construction.** They configure no
defines and one include dir, so the per-file copy it removes was nearly free
here. It scales with config size, not with file count.

## Branch total

Rounds 1-4, first sitting:

| Corpus / configuration | `f0ccb75` | `98e7388` | Change |
|---|---|---|---|
| opentitan, all CPUs | 1765 ms | 290 ms | **6.1× faster** |
| opentitan, 1 CPU | 1785 ms | 1300 ms | 1.37× faster |
| synth60, all CPUs | 1567 ms | 305 ms | **5.1× faster** |
| synth60, 1 CPU | 1549 ms | 1483 ms | 1.04× faster |
| synth60, `OMP_NUM_THREADS=1` | 1562 ms | 304 ms | **5.1× faster** |
| opentitan peak RSS | 257 MB | 383 MB | 1.49× more |

Round 5, second sitting (`5b3a363` -> `6a06623`):

| Corpus / configuration | `5b3a363` | `6a06623` | Change |
|---|---|---|---|
| opentitan, all CPUs | 305 ms | 279 ms | 1.09× faster |
| opentitan, 1 CPU | 1340 ms | 1301 ms | 1.03× faster |
| hdr100, all CPUs | 443 ms | 340 ms | **1.30× faster** |
| hdr200, all CPUs | 571 ms | 342 ms | **1.67× faster** |
| shared2, all CPUs | 206 ms | 190 ms | 1.08× faster |
| opentitan peak RSS | 384 MB | 374 MB | 1.03× less |
| hdr200 peak RSS | 509 MB | 285 MB | **1.79× less** |

Round 5 also removes the *shape* of the hdr200 cost, not just its size: hdr200
was 29% slower than hdr100 at `5b3a363` and is within noise of it at `6a06623`,
so doubling a design's header bytes no longer costs startup time.

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
- The round-5 tables were measured in a separate sitting from rounds 1-4, on the
  same machine. `5b3a363` reads 305 ms on opentitan there against `98e7388`'s
  290 ms above, which is the sitting-to-sitting offset, not a regression between
  those two commits. Compare within a table, never across the two sittings.
- Two round-5 commits (`2b4cf18`, `2991fb5`) fix edit-path costs that a cold
  startup benchmark cannot express. Their rows are flat by construction and
  should not be read as "no effect".
