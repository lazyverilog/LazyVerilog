<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-05-28 | Updated: 2026-05-28 -->

# src

## Purpose
C++ source for the lazyverilog LSP server. Split into server core (root of this directory) and feature implementations (`features/`). All files communicate via shared analyzer, syntax index, config, and document state infrastructure.

## Key Files

| File | Description |
|------|-------------|
| `main.cpp` | Entry point — initializes and runs the LSP server |
| `server.cpp` | JSON-RPC dispatch — routes LSP requests to feature handlers |
| `server.hpp` | Server class interface |
| `analyzer.cpp` | AST construction and traversal over SystemVerilog source |
| `analyzer.hpp` | Analyzer interface |
| `syntax_index.cpp` | Fast symbol lookup index built from parsed AST |
| `syntax_index.hpp` | Syntax index interface |
| `background_compiler.cpp` | Incremental background parsing service |
| `background_compiler.hpp` | Background compiler interface |
| `document_state.hpp` | Header-only document state tracking (no .cpp) |
| `config.cpp` | Loads and parses `lazyverilog.toml` configuration |
| `config.hpp` | Configuration structs |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `features/` | One file per LSP feature/command (see `features/AGENTS.md`) |

## For AI Agents

### Dependency Flow
1. `main.cpp` → initializes server, loads config
2. `server.cpp` → dispatches JSON-RPC to feature modules
3. Features depend on: `analyzer`, `syntax_index`, `background_compiler`, `config`

### Working In This Directory
- Add new LSP commands in `features/`, register them in `server.cpp`
- `document_state.hpp` is header-only; keep it so
- Config changes: update `config.hpp` struct + `config.cpp` parser + `lazyverilog.toml` + `docs/formatter/options.md`

### Testing Requirements
- Feature tests live in `../tests/test_<feature>.cpp`
- Run: `ctest --test-dir ../build`

### Common Patterns
- JSON-RPC over stdin/stdout
- Config reloaded on every `workspace/didChangeConfiguration`
- Config search: walk up from the opened file to the nearest `lazyverilog.toml`
  (`ProjectRootResolver`).  `rootUri` is only an eager-indexing hint; nothing per file
  depends on it, and the Neovim plugin no longer sends one.

## Dependencies

### Internal
- All features depend on `analyzer.hpp`, `syntax_index.hpp`, `config.hpp`, `document_state.hpp`

### External
- slang — SystemVerilog parser
- nlohmann/json — JSON-RPC messages

<!-- MANUAL: -->
