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
- `[inlay_hint].enable` and `[folding].enable` each drive one per-keystroke request.
  **Neither turns the capability off any more.**  `foldingRangeProvider` and
  `inlayHintProvider` are advertised as literal `true`, the way clangd advertises
  them, and the options are answered in the handlers by returning nothing.
  Capabilities are exchanged before any file is open, and the root is resolved per
  file, so there is no single config at `initialize` to answer from.
- The cost of that is measured and real: turning a capability off is what stopped the
  client asking at all — 0 requests against 4-6 over five keystrokes in headless
  Neovim.  A disabled feature now pays a round trip per keystroke that returns
  nothing.  clangd has no folding option at all for this reason (`Config.h` has
  `InlayHints.Enabled` and nothing for folding); `[folding].enable` is kept because
  it is what users already configure.  If per-keystroke cost becomes the problem
  again, the answer is a cheaper handler, not a withheld capability.
- The whole dynamic-registration path for these two is **gone**:
  `sync_dynamic_registration()`, the `dynamicRegistration` probe in `initialize`, and
  the fixed registration ids.  Do not reintroduce it to "save" the requests above
  without first re-reading why it was removed — a client that is told statically
  ignores a later unregister (Neovim's `client:supports_method()` answers from
  `server_capabilities` whenever that field is present), so the two mechanisms cannot
  coexist, and the static one is what a per-file config needs.
- A buffer whose filetype is unset gets no LSP features at all, which looks exactly
  like a broken server — check the filetype before the server when hints do not appear.
- Guarded by `ctest --test-dir build -R config-root-cli-smoke`, which pins that both
  providers are advertised unconditionally (config off, dynamic-registration client,
  and no `rootUri` at all), and that the handler still declines — including for a file
  two directories below the config that governs it.  Editor-side switches for both
  features live in `lua/lazyverilog/config.lua` (`folding`, `inlay_hints`).
- Details and prior measured rounds: `docs/dev/edit-perf.md`.

### Project Roots
- **The server decides which project a file belongs to, not the editor.**
  `ProjectRootResolver` (`src/project_root.cpp`) walks up from each file to the
  nearest `lazyverilog.toml`, which is clangd's
  `DirectoryBasedGlobalCompilationDatabase::lookupCDB` with a different marker.
  Per directory, with misses cached as deliberately as hits: a file five directories
  deep stats five directories per lookup and most of that walk is misses, repeated by
  every open buffer and every indexed file.
- **Not finding a config is an answer (`nullopt`), never a guess at the file's own
  directory.**  That guess is what used to put a `.cache/` next to whatever file was
  opened — including in `/tmp`.
- The Neovim plugin sends **no `root_dir`** and has no `root_markers`.  It cannot get
  this right: `vim.fs.root` resolves its marker list by *marker order, not proximity*,
  so `.git` at the top of a monorepo outranked the `lazyverilog.toml` beside the file
  and the config was never read.  A `root_markers` passed to `setup()` is ignored with
  one notification.
- `initialize` still indexes eagerly when a client does send `rootUri` — a warm first
  go-to-definition is worth keeping — but nothing per file depends on it.  `didOpen`
  discovers projects too (`discover_project_for()`).
- **Defines and include directories are per file**, looked up through
  `ProjectParseInputs` (`src/parse_inputs.cpp`) — clangd's
  `GlobalCompilationDatabase::getCompileCommand(File)`, with the same fallback for a
  file under no known project.  There is still **one** `Analyzer`: clangd keeps one
  `BackgroundIndex` too and looks commands up per file, which is what makes a second
  project cost a map entry instead of another set of worker threads and another
  source manager.
- Every parse path asks for the file it is about to parse — `make_state()`, the
  background indexer, `:LintAll`'s synchronous walk, the shard preload.  Do not
  reintroduce a flat `defines_` member; that is what this replaced.
- The shard config digest lives in `ParseInputs` for the same reason: one digest
  across projects would make each launch discard the other project's shards as
  config-stale.  The preload's include-resolution memo is keyed on the including
  project's digest as well as the spelling, or the first project to resolve
  `uvm_macros.svh` answers for every project that spells it the same way.
- **Every per-document request is answered from that file's config**, via
  `config_for(uri)` — formatting, lint, AutoFF, AutoWire, AutoArg, the RTL tree, and
  the two capability switches.  Do not reach for `config_` in a handler that has a
  URI; `config_` is the session's eager-indexing config, not the file's.
- Two things stay session-wide, and are not per-file questions: **which** files to
  index (one index covers every open project, so the filelist is the union), and
  **semantic compilation** (`[compilation]`), which builds a single slang
  `Compilation` and therefore has one preprocessor for all of it.
- Folding several projects is one analyzer transaction: `fold_project_root()` accumulates
  and passes `Analyzer::Reindex::Deferred`, and `apply_project_inputs()` is what schedules
  the burst.  Registering a project *and* scheduling there costs N+1 full reindex
  generations for N projects, each parsing the filelist as it stood before that project
  joined it -- superseded before they can commit, but not before their workers have spent
  the CPU.
- `reload_all_projects()` folds in a **deterministic order** (the session root, then the
  discovered roots, then the open buffers' roots, the last two sorted).  Fold order decides
  the order of the merged defines and `+incdir+` entries, which are the analyzer's
  *defaults* -- their digest keys every shard of a file under no project, and their order
  is the header search order.  Iterating a hash container there would invalidate a
  different arbitrary subset of those shards on each save.
- Because the index is a union, **a name two projects both declare is disambiguated
  at the lookup, not by splitting the index** — `ProjectIndexSnapshot::find_module()`
  takes the asking file's path and ranks the candidates by path proximity.  clangd
  does the same thing (`FuzzyFindRequest::ProximityPaths`, scored by the directory-tree
  edit distance in `FileDistance.h`); its `LookupRequest` is a set of `SymbolID` and
  carries no path filter at all, and `mergeSymbol()` collapses two same-ID symbols
  into one without ever asking which project they came from.
- It **ranks, it does not filter**, and that distinction is the whole design.  A filter
  returns nothing when the asking file is in no project, or when the module lives in
  shared IP outside either root — both routine in hardware, where a `common_ip/` with
  no `lazyverilog.toml` is normal.  A filter would also mean paying the union's memory
  and indexing cost while getting split-index behaviour, which is the one thing the
  union exists to avoid.
- Only names with more than one declaration cost anything: `module_duplicates` holds
  those alone, so a project of thousands of uniquely-named modules does not allocate a
  vector per name to say "there is exactly one of these".  `ProjectIndexModuleRef`
  stores a `shard_slot`, not a path string, for the same reason — resolve it with
  `module_path()`.
- Ties keep first-indexed order, so the answer is stable across requests.  Like clangd
  we ignore semantic roots (`FileDistance.h` says so outright), so two files equally
  far from the asker are not distinguishable and the first one wins.
- Guarded by `./build/lazyverilog-tests "[module-proximity]"`, including end to end
  through AutoInst: the two projects' modules differ in their ports, so the ports that
  come back name which project answered.
- **A saved config rebuilds every known project, not just the one that changed**
  (`reload_all_projects()`).  Reloading only the saved config replaced the merged
  filelist with that project's own, which unindexed every other open project until
  one of its buffers was opened again.  The rebuild also re-folds the open buffers,
  because a `lazyverilog.toml` created just now makes a project no recorded root
  names and the buffer that now belongs to it sent its `didOpen` long ago.
  `fold_project_root()` accumulates and `apply_project_inputs()` applies, so folding
  several projects still schedules one reindex generation.
- Guarded by `./build/lazyverilog-tests "[parse-inputs]"`.  Those tests are written
  so a session-wide set cannot pass them — each project's source only yields a module
  under its own define, or resolves a same-spelled header through its own `+incdir+`.
- Guarded by `./build/lazyverilog-tests "[project-root]"` and
  `ctest --test-dir build -R config-root-cli-smoke`.

### Index Shard Cache
- Per-file shards persist in `<project_root>/.cache/lazyverilog/index`, where
  `project_root` is **that file's own** — resolved as above, not a session-wide root.
  `IndexCacheStorage` picks the directory per file, which is clangd's
  `DiskBackedIndexStorageManager`; keeping the *storage* per file rather than the
  *indexer* is what makes several projects cheap.
- A file in no project falls back to `user_cache_directory()/lazyverilog/index`
  (`$XDG_CACHE_HOME` or `~/.cache` on Unix, `~/Library/Caches` on macOS,
  `%LOCALAPPDATA%` on Windows — the same convention as LLVM's
  `llvm::sys::path::cache_directory()`).  No `.gitignore` is written there; that
  directory is in no repository.
- An `include`d header's shard lives beside **its** project's config, not the
  includer's.  A verification header shared by two designs is one file in one project.
- **There is no switch.**  `[index].cache` is gone -- the config key, the struct, and the
  parse -- and `index_cache_storage()` always builds a storage.  clangd has no such option
  either.  What it used to protect, a project that must have nothing written into it, is
  answered by *where* shards go: a file under no project caches outside the tree, and a
  directory that cannot be created runs uncached on its own.  Making it per project first
  showed why it should not exist: gated on the server's `config_` the switch depended on
  which directory the server was launched from (with no `rootUri` that config is whatever
  sits above the working directory), and per project it was a second answer to a question
  the shard *location* already answers.  A `[index]` table still in a config is reported
  once on stderr, not silently ignored.  `index-bench --cache off` stays, for
  `startup_bench.py --no-cache`; it is a bench knob, not a setting.
- Keyed on **content digests** of the file, its
  `include`s, and the defines/incdirs — never mtime, which is unusable on a shared
  filesystem.
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
  format version, and runs over **every** directory the burst wrote into.  Files
  without our magic are left alone.
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
