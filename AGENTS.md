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
| `README.md` | Demo, comparison, feature list, and a Getting Started link to the documentation site (https://lazyverilog.github.io) |
| `.gitmodules` | Git submodule configuration |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `src/` | C++ source — server core + all LSP feature implementations (see `src/AGENTS.md`) |
| `tests/` | Unit and integration tests (see `tests/AGENTS.md`) |
| `docs/` | Documentation source, built into https://lazyverilog.github.io with VitePress (see `docs/AGENTS.md`) |
| `tools/` | Dev/benchmark utilities (see `tools/AGENTS.md`) |
| `lua/` | Neovim plugin Lua integration (see `lua/AGENTS.md`) |
| `scripts/` | Repository-maintenance scripts, e.g. README translation (see `scripts/AGENTS.md`) |

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
- In short, requests have three layers:
  - current request file: live AST / `DocumentState` is authoritative;
  - other open buffers: use cached `SyntaxIndex` summaries derived from their
    live ASTs so unsaved cross-file edits are visible;
  - closed/project files: use background `SyntaxIndex` shards.
- `opened_files_index_cache_` is keyed by the current request URI and excludes
  that URI. For example, while handling a request in `top.sv`, the opened-files
  index may include `memory.sv` and `pkg.sv`, but not `top.sv`; `top.sv` must be
  inspected from its live AST to avoid stale or duplicate current-file facts.
- Avoid designing features that require closed project files to keep ASTs.
- If a current/open file needs index-shaped facts such as modules, instances,
  symbol IDs, or reference occurrences, derive those facts from the current
  file AST.  Prefer caching such AST-derived indexes per immutable
  `DocumentState` snapshot when they are reused across requests; invalidate by
  replacing the `DocumentState` on `didChange`.
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
  - `SyntaxPass` writes `ImmutableData`
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

## Release Notes Style

When writing `docs/releases/vX.Y.Z.md`, keep the note precise and simple:
- Use a short one-line release theme after the title.
- Summarize large feature areas in 2–5 sentences instead of listing every
  internal commit.
- Keep only user-visible fixes as concise bullets.
- Avoid noisy implementation details unless they explain a user-visible behavior
  change.
