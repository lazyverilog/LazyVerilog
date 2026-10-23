<!-- Generated: 2026-05-28 | Updated: 2026-10-23 -->

# lazyverilog

SystemVerilog LSP server in C++ (formatting, lint, go-to-def, references, AutoWire/AutoInst, …). JSON-RPC over stdin/stdout. Config: `lazyverilog.toml`.

## Layout
- `src/` core + `src/features/*.cpp` (one feature per file) — see `src/AGENTS.md`
- `tests/` mirrors features (`tests/test_formatter.cpp` ↔ `src/features/formatter.cpp`) — see `tests/AGENTS.md`
- `docs/` VitePress site (https://lazyverilog.github.io); formatter options in `docs/formatter/options.md`
- `tools/` benches/utilities, `lua/` Neovim plugin, `scripts/` repo maintenance, `vscode/` extension
- Deps: slang (submodule), nlohmann/json

## Build / Test
```bash
cmake -B build && cmake --build build -j$(nproc)
ctest --test-dir build                     # all
./build/lazyverilog-tests "[tag]"          # one feature / "test name"
```
- Windows CI has no POSIX libc: guard POSIX-only calls (e.g. `setenv`) with `#ifdef _WIN32`, tests included.

## Performance
- Startup: `tools/startup_bench.py [--cpus 0] [--trace] [--warm|--no-cache]`. Report index ms (full CPU and `--cpus 0`), user CPU, maxRSS; report cold/warm/no-cache separately. Compare commits via `index-bench` built in a worktree (`--binary`), same `CMAKE_BUILD_TYPE` both sides. Workers = CPU slice (`src/cpu_budget.cpp`), max 8.
- Edit path: `tools/edit_latency_bench.py <root> <file> [--cpus 0]`. Neovim sends whole-file `foldingRange` + `inlayHint` on every `didChange`, while the parse is still running (`DocumentState::tree` null); requests are serialized (`max_workers = 1`).
- `provide_folding_range()` uses the token scan only — no AST/cache. Don't add an AST pass. Fold columns are UTF-16 units, excluding `\r\n`.
- `foldingRangeProvider`/`inlayHintProvider` are always advertised `true`; `[folding].enable`/`[inlay_hint].enable` are honored in the handler by returning nothing. Dynamic registration was removed on purpose — don't reintroduce it.
- No AST walks / whole-project merges on request paths; consume published snapshots. Cache merged open-buffer indexes per URI.
- Scaling guards are ratios vs. a structurally identical input (min of N runs), never absolute ms. Guards: `[scaling]`, `[folding][scaling]`, `-R config-root-cli-smoke`.
- Details: `docs/dev/startup-perf.md`, `docs/dev/edit-perf.md`, `PERF.md`.

## Project Roots (multi-project)
- Server owns root resolution: `ProjectRootResolver` (`src/project_root.cpp`) walks up to nearest `lazyverilog.toml` (misses cached). No config ⇒ `nullopt`, never guess. Neovim plugin sends no `root_dir`/`root_markers`.
- Per-file inputs: defines/incdirs via `ProjectParseInputs` (`src/parse_inputs.cpp`); every parse path asks per file. No flat `defines_`. One `Analyzer` for all projects.
- Handlers with a URI use `config_for(uri)`, never `config_` (session eager-index config only). `root_` is just a prefetch hint; never repoint it.
- Index filelist = union of all projects. `[compilation]` = one slang `Compilation` per project filelist (`CompilationSnapshot::groups`, `compile_group()`), compiled sequentially; `CompilationGroup::files` are indices into `CompilationSnapshot::files`. Gate on `config_for(uri)` (publish + `:LintAll`). Debounce is `kCompilationDebounce`; non-debounced callers use `compile_now()`.
- No registered project ⇒ one empty-root fallback group (per-project `SourceLibrary`, all `isDefault`, sorted root order). Fallback chosen only by "did any project register what it compiles". Buffers under no toml join no group.
- Folding projects: `fold_project_root()` accumulates (`Reindex::Deferred`), `apply_project_inputs()` schedules once. `reload_all_projects()` rebuilds all projects in deterministic sorted order on any config save.
- Same-name modules: rank, don't filter. `find_module()` / `by_path_proximity()` share `path_proximity_score()` (shared prefix, then depth; ties by path via `Analyzer::sorted_by_path()`, headers first). Scans iterate an ordered list (`nearest_shard_answer()` etc.) — don't add per-scan ranking.
- References/rename: `file_can_mean_target()` filters per file; files in no project and the asker's project are always admitted. Never put the project in `SymbolID`.
- `normalize_filesystem_path()` memo: invalidate only on config invalidation and watcher create/delete, never on change.
- Guards: `[module-proximity]` (assert both directions), `[parse-inputs]`, `[project-root]`.

## Index Shard Cache
- Shards in `<file's own project root>/.cache/lazyverilog/index`; no project ⇒ `user_cache_directory()/lazyverilog/index`. Header shard lives in the header's project. No on/off switch (`[index].cache` removed).
- Key = content digests (file, includes, defines/incdirs) + how each `include` resolved; never mtime. Digest the bytes the parse read (`DocumentState::parsed_digests`, `IndexCache::digest_source_buffer()`).
- `read_file_text_optional()`: single open (`O_NONBLOCK`) + `fstat`, read `st_size + 1`.
- Validate `file_id`s at read via `Reader::file_id()`, not `r.u32()`.
- Adding a field in `src/syntax_index.hpp` ⇒ update codec in `src/index_cache.cpp` + bump `kFormatVersion` (also for `shard_path()` renames).
- `prune_missing_sources()` runs once per full reindex only (`background_full_reindex_generation_`).
- Warm tests must distinguish hit from reparse (see "an unchanged project is served from the shards" in `tests/test_index_cache.cpp`).

## AST vs Index
- Open buffer: live slang `SyntaxTree` is authoritative; derive index facts from it, cache per `DocumentState`.
- Closed/project files: `SyntaxIndex` shards only — no retained ASTs.
- Header declarations live in the header's own shard; resolve through it (`docs/dev/indexing.md`).

## Formatter
- Entry: `src/features/formatter.cpp` → `format_source()`. Token-based, idempotent.
- Passes: lexer → Syntax → Macro → Wrap → Indent → Align → Comment → Spacing → BlankLine → render. Each pass writes only its own metadata (`SyntaxPass`: Syntax/Topology/CommentFacts; `XPass`: `XMetadata`; lexer: Lexeme/InputTriviaFacts).
- Use `TokenKind`, not regex/string compares. Don't read `input_trivia` in passes (exception: comment own-line vs trailing classification).
- Intended, don't "fix": `begin_newline` also moves `struct`/`union` `{` (not `enum`); `line_length` includes indent — WrapPass estimates via `TopologyFacts::scope_depth`, never reads `IndentMetadata`.

## Hand-Built JSON
- One escaper: `json_quoted()` / `append_json_string()` in `src/string_utils.hpp`. Escape all < U+0020 (cast to `unsigned char`). Guard: `[json]`.

## Release
```bash
ctest --test-dir build
./tools/release.sh --version vX.Y.Z   # interactive; needs docs/releases/vX.Y.Z.md first
```
- CI builds binaries, bumps lua/vscode version+checksums, tags, publishes. VS Code Marketplace: manual web upload of the `.vsix` at https://marketplace.visualstudio.com/manage/publishers/lazyverilog (no Azure DevOps/PAT).

<!-- MANUAL: -->
