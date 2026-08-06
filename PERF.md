# Start-up Indexing Performance Improvements

Startup cost today ≈ sequential slang parse of the whole filelist on a single
background thread, cold on every launch.

Flow: `initialize` → `load_config` + `load_vcode` (cheap, sync) →
`Analyzer::set_project_config` fills `background_pending_files_` → single
`background_indexer_` thread pops one path at a time → per file: fresh
`SourceManager`, `SyntaxTree::fromFile`, `SyntaxIndex::build(Declarations)`,
diag collection → shard committed to `extra_cache_` → one debounced
`ProjectIndexSnapshot` publish after queue drains.

## Improvements (ranked)

1. **Parallelize the background indexer** — `background_indexer_` is one thread
   (`src/analyzer.cpp` `start_background_indexer_locked`). Per-file parse is
   independent; commits are already generation-checked under `map_mutex_`.
   Worker pool like `BackgroundCompiler` (capped ~4, nice'd, HPC-safe) →
   near-linear warm-up speedup.

2. **Persistent on-disk shard cache (clangd-style)** — every launch reparses
   unchanged files. Serialize `SyntaxIndex` per file keyed by
   `(path, mtime+size, hash(defines+incdirs))`; on startup load hits, parse
   only misses. Biggest win for repeated startups on large designs.

3. **Cut redundant include I/O** — each file gets a fresh `SourceManager`, so
   shared headers (uvm_macros, package headers) are open+stat+read once per
   source file — O(files × includes) filesystem traffic (painful on NFS).
   Cache raw header text once per generation (path→text, seed via
   `assignText`). Re-preprocessing per file still required for correctness;
   the disk I/O is not.

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

6. **Queue priority for perceived latency** — startup queue is filelist order.
   Index the open file's include deps + same-directory files first so
   definition/hover works on the user's file before the whole design finishes.

## Baseline (to fill)

- Corpus:
- Machine:
- Metric: wall time from `set_project_config` to background index idle; per-file
  `make_file_state_with_options` times via `log_perf`.
