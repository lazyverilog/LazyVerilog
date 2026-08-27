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
  shard generally does not carry them; an open buffer's does, and so do the few
  files that were already parsing when the projection was installed.  Resolve
  header symbols through the header's shard, never by assuming either.  See
  `docs/dev/indexing.md` ("Shared `include`d headers") and `PERF.md` round 6.
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
