<!-- Generated: 2026-05-28 | Updated: 2026-06-05 -->

# lazyverilog

## Purpose
SystemVerilog LSP server written in C++. Provides language intelligence (formatting, linting, go-to-definition, references, auto-wire, auto-instantiation, etc.) for SystemVerilog/Verilog files via the Language Server Protocol.

## Key Files

| File | Description |
|------|-------------|
| `CMakeLists.txt` | CMake build configuration |
| `lazyverilog.toml` | LSP server config — formatting and linting options |
| `CLAUDE.md` | Project instructions for AI agents |
| `README.md` | Project overview and usage |
| `src/features/` | Individual LSP feature implementations |
| `.gitmodules` | Git submodule configuration |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `src/` | C++ source — server core + all LSP feature implementations (see `src/AGENTS.md`) |
| `tests/` | Unit and integration tests (see `tests/AGENTS.md`) |
| `docs/` | Documentation for formatter options and diagnostics (see `docs/AGENTS.md`) |
| `tools/` | Dev/benchmark utilities (see `tools/AGENTS.md`) |
| `lua/` | Neovim plugin Lua integration (see `lua/AGENTS.md`) |

## For AI Agents

### Build
```bash
cmake -B build
cmake --build build -j$(nproc)
```

### Testing Requirements
```bash
ctest --test-dir build                          # all tests
./build/lazyverilog-tests "[tag]"               # single feature
./build/lazyverilog-tests "test name here"      # single named test
```

### Startup Performance
```bash
tools/startup_bench.py                          # cold project-index startup, 3 runs
tools/startup_bench.py --cpus 0                 # emulate a 1-CPU (HPC/container) slice
tools/startup_bench.py --cpus 0 --trace         # per-file timings, slowest first
```
- Report all four numbers, not just wall time: index ms at full CPU, index ms at
  `--cpus 0`, user CPU, and maxRSS.  A change can leave desktop wall time flat while
  adding user CPU, which is exactly what hurts a node that granted one core.
- Worker count comes from the CPU slice (`src/cpu_budget.cpp`), capped at 8.
- Compare two commits by building `index-bench` in a worktree and pointing
  `--binary` at it.  Use the **same `CMAKE_BUILD_TYPE` on both sides** —
  `Release` vs `RelWithDebInfo` swamps the effect being measured.
- A shared header re-parsed once per includer is correct and expected.  An
  *index* pass that also walks the whole header is not: it multiplies the
  largest file in the project by the number of includers.  Watch `maxRSS` —
  a per-file map keyed on header content shows up there first.
- Guarded in CI by `./build/lazyverilog-tests "[scaling]"`
  (`tests/test_shared_header_scaling.cpp`).  Write scaling guards as a **ratio
  against a structurally identical input, minimum of N runs**, never an absolute
  millisecond budget — that is what survives a shared CI runner.
- Details and prior measured rounds: `docs/dev/startup-perf.md`, `PERF.md`.

### Edit-Path Performance
```bash
tools/edit_latency_bench.py <project-root> <file-in-it>   # steady-state edit loop
tools/edit_latency_bench.py ~/work/chip rtl/alu.sv --cpus 0
```
- Neovim sends a **whole-file `foldingRange` and `inlayHint` on every `didChange`**,
  and requests are answered one at a time (`RemoteEndPoint(..., max_workers = 1)`),
  so an expensive request delays the completion the user is waiting on.
- It sends them **from the notification itself**, so the request lands while the
  parse that notification started is still running and `DocumentState::tree` is
  null.  A handler that gives up there answers *every* editor request with
  nothing — which is both wrong and the fastest possible benchmark result.
- `provide_folding_range()` therefore derives folds from the **token scan and
  nothing else** — no syntax tree, no cache, the same answer whether or not the
  parse has landed.  This is clangd's design (`getFoldingRanges(Code, ...)`, a
  lex the AST never touches).  The cost is real: SystemVerilog cannot tell
  `my_type_t state;` from `my_child u_inst (...);` lexically, so instance folds,
  identifier-led declaration runs and module-header list trimming are gone.  Do
  not reintroduce an AST pass here without also reintroducing the reparse-window
  answer it needs.
- The bench measures round trips only.  The client also pays for the reply on its
  main loop, and Neovim's fold handler walks every row of every range it is handed,
  so that cost tracks the **sum of range spans**, not the range count.  Measured for
  a 13k-line file (3803 ranges, 327 KiB on the wire, 42684 rows covered): ~4 ms to
  `vim.json.decode` and ~2 ms to walk.  Small next to the request itself — do not
  reach for a smaller payload before measuring that it is what hurts.
- Guarded by `./build/lazyverilog-tests "[folding][scaling]"`.  Same rule as the
  startup guards: a **ratio against a structurally identical input at another
  size**, never an absolute millisecond budget.
- `[inlay_hint].enable` and `[folding].enable` each drive one per-keystroke request,
  and turning the capability off is what stops the client asking at all — measured 0
  requests against 4-6 over five keystrokes in headless Neovim.  Both are read from
  `<root>/lazyverilog.toml`, which is why `initialize` has to find the real one.
- **Advertise a capability statically or register it dynamically, never both.**
  Neovim's `client:supports_method()` answers from `server_capabilities` whenever
  that field is present, so a later `client/unregisterCapability` changes nothing and
  the client keeps requesting for the rest of the session — measured: unregister
  accepted (`dynamic_capabilities:get()` → nil) while
  `server_capabilities.inlayHintProvider` stayed `true` and requests kept coming.  So
  when the client advertises `dynamicRegistration` for one of these, `initialize`
  **omits** the provider field and the `initialized` handler registers instead.
- `sync_dynamic_registration()` sends `client/registerCapability` /
  `client/unregisterCapability` on `didChangeConfiguration`, so an edit to either
  option takes effect mid-session with no restart — measured both directions, 4 → 0
  and 0 → 4.  Neovim 0.12.5 opts in for `inlayHint` but **not** for `foldingRange`
  (`dynamicRegistration = false`), so a `[folding].enable` edit there still needs a
  restart, and the server logs that rather than sending a request the client may
  ignore.  Registration ids are fixed (`kFoldingRegistrationId`,
  `kInlayHintRegistrationId`) because the unregister has to name what the register used.
- A registration carries a `documentSelector` of `systemverilog`/`verilog`.  A buffer
  whose filetype is unset matches nothing, which looks exactly like a broken server —
  check the filetype before the server when hints do not appear.
- Guarded by `ctest --test-dir build -R config-root-cli-smoke`, which pins both the
  static replies and the withheld-when-dynamic case.  Editor-side switches for both
  features live in `lua/lazyverilog/config.lua` (`folding`, `inlay_hints`).
- Details and prior measured rounds: `docs/dev/edit-perf.md`.

### Index Shard Cache
- Per-file shards persist in `<project_root>/.cache/lazyverilog/index`; `[index].cache`
  turns it off.  Keyed on **content digests** of the file, its `include`s, and the
  defines/incdirs — never mtime, which is unusable on a shared filesystem.
- A digest answers "did what I read change".  It cannot answer "would I read the same
  file", so the key also records **how each `include` resolved**, unresolved ones
  included: creating a header that satisfies an `include` for the first time, or
  shadowing one from an earlier `+incdir+`, changes no file the key hashes.  The
  preload re-runs slang's search (`SourceManager::readHeader`'s order) against a memo.
- Digests come from **the bytes the parse read**, never a re-read of the file — see
  `DocumentState::parsed_digests`.  Hashing a `SourceManager` buffer means
  `IndexCache::digest_source_buffer()`, which drops the `'\0'` slang appends; hashing
  `getSourceText()` directly compares against `digest_file()` and never matches.
- Adding a field to any entry in `src/syntax_index.hpp` requires updating the codec in
  `src/index_cache.cpp` and bumping `kFormatVersion`.  A `static_assert` on each struct's
  size makes forgetting a compile error rather than a shard that silently drops the field.
  Renaming shards (`shard_path()`) needs a bump too, or the old names are stranded.
- The sweep (`IndexCache::prune_missing_sources()`, queued by the preload onto the
  writer thread) removes shards whose source file is gone and shards of any other
  format version.  Files without our magic are left alone.
- Benchmark all three halves: `tools/startup_bench.py` clears the shard cache before
  each run (**cold**), `--warm` keeps it, `--no-cache` turns the cache off entirely.
  Report them separately — a change can improve warm and wreck cold.
- A warm test must be able to tell a hit from a reparse.  Asserting the second launch
  produces the right index does not: so does a launch that silently reparsed
  everything, which is how a dead cache went unnoticed.  See "an unchanged project is
  served from the shards" in `tests/test_index_cache.cpp` for the shape that works —
  edit the stored shard, keep its key, assert the edit comes back.

### Releasing a New Version
```bash
ctest --test-dir build                          # test gate — must pass first
./tools/release.sh --version vX.Y.Z              # commits release note, pushes, dispatches CI
```
- Write release notes at `docs/releases/vX.Y.Z.md` before running the script (script warns and
  asks to confirm if missing). Follow the format of prior files in that directory.
- `tools/release.sh` is interactive (confirms before push/dispatch); it commits the release note,
  pushes the current branch, triggers `.github/workflows/release.yml` via `workflow_dispatch`, and
  watches the run. CI builds all platform binaries, computes checksums, bumps
  `lua/lazyverilog/{version,checksums}.lua` and `vscode/{package.json,package-lock.json,src/version.ts,src/checksums.ts}`,
  commits that metadata, tags, and publishes the GitHub Release.
- The VS Code Marketplace upload is **not** part of CI — publish `lazyverilog-vscode` manually by
  downloading the `.vsix` asset from the GitHub Release and uploading it at
  https://marketplace.visualstudio.com/manage/publishers/lazyverilog (web upload; no Azure
  DevOps org or PAT needed — that's a separate, unrelated flow, don't go down that path).
- Windows CI build has no POSIX libc — don't add POSIX-only calls (e.g. `setenv`) to code that
  compiles there (tests included) without an `#ifdef _WIN32` guard.

### Working In This Directory
- Core formatting logic: `src/features/formatter.cpp` → `format_source()`
- Config options documented: `docs/formatter/options.md`
- Each LSP feature lives in its own `src/features/*.cpp` file
- Tests mirror feature files: `tests/test_formatter.cpp` tests `src/features/formatter.cpp`

### Common Patterns
- Token-based, idempotent formatting via sequential passes
- Config loaded from `lazyverilog.toml`, walked up from the opened file's directory
- JSON-RPC over stdin/stdout

### AST / Index Architecture Philosophy
- Current/open buffers are represented by live slang `SyntaxTree` AST snapshots.
  This keeps cursor-sensitive features precise for unsaved edits, diagnostics,
  local syntax context, and operations that need exact source structure.
- Other files should generally be represented by compact `SyntaxIndex` shards,
  not by retained full ASTs.  Project/filelist files may number in the hundreds
  or thousands, so keeping full ASTs for every file would increase memory use,
  allocator/source-manager lifetime complexity, and request-path contention.
- In short:
  - current file: AST is authoritative;
  - other/project/closed files: index is authoritative;
  - avoid designing features that require closed project files to keep ASTs.
- If a current/open file needs index-shaped facts such as modules, instances,
  symbol IDs, or reference occurrences, derive those facts from the current
  file AST.  Prefer caching such AST-derived indexes per immutable
  `DocumentState` snapshot when they are reused across requests; invalidate by
  replacing the `DocumentState` on `didChange`.
- An `include`d header's declarations live in **the header's own shard**, not in
  its includers'.  Once a header parses standalone, the background indexer serves
  the rest of the burst only its preprocessor directives, so a closed includer's
  shard generally does not carry them; an open buffer's does, and so does the one
  file whose parse proved the header stands alone (a burst holds its other
  workers at a warmup gate until that file has installed the projection, so it is
  one file and not a scheduling race).  Resolve header symbols through the
  header's shard, never by assuming either.  See `docs/dev/indexing.md`
  ("Shared `include`d headers") and `PERF.md` rounds 6 and 7.
- Do not move expensive whole-project merges or closed-file AST walks onto hot
  request paths.  Background indexing should publish reusable index snapshots,
  and request handlers should consume those snapshots without reparsing or
  rebuilding project-wide state.
- Other-open-buffer dynamic indexes are also request-path data.  They may be
  merged into a reusable cache keyed by the current URI, but handlers should not
  rebuild the same all-open-buffer merge for every completion/code-action edit
  cycle.

### Formatter Pass Ownership and Idempotency
- Formatter flow is: lexer lexes source into Tokenflow, then `SyntaxPass`,
  `MacroPass`, `WrapPass`, `IndentPass`, `AlignPass`, `CommentPass`,
  `SpacingPass`, `BlankLinePass`, then rendering.
- Token data is split into immutable facts and mutable formatting metadata.
- Each pass has exclusive write ownership of its metadata family:
  - `TokenCollector` (lexer) writes `LexemeFacts` and `InputTriviaFacts`
  - `SyntaxPass` writes `SyntaxFacts`, `TopologyFacts`, and `CommentFacts`
  - `MacroPass` writes `MacroMetadata`
  - `WrapPass` writes `WrapMetadata`
  - `IndentPass` writes `IndentMetadata`
  - `AlignPass` writes `AlignMetadata`
  - `CommentPass` writes `CommentMetadata`
  - `SpacingPass` writes `SpaceMetadata`
  - `BlankLinePass` writes `BlankLineMetadata`
- Prevent non-idempotency:
  - Prefer `slang::parsing::TokenKind` facts from the lexer.
  - Do not use regex, string search, or string comparison for syntax decisions
    when a `TokenKind`-based check is available.
  - Do not write formatter pass logic that depends on how the original source
    looked. Referencing `input_trivia` inside a pass is not desired because it
    can make formatting depend on pre-format whitespace.
  - Exception: comment role classification may reference original source
    positioning. This is unavoidable for distinguishing own-line comments from
    trailing comments, and must be handled carefully so it does not create
    non-idempotent formatting behavior.

## Dependencies

### External
- slang (SystemVerilog parser) — via git submodule
- nlohmann/json — JSON-RPC message handling

<!-- MANUAL: -->
