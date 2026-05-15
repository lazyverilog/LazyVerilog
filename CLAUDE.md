# CLAUDE.md
## Goal of this project
This is a C++ rewrite of the previous Python LSP server.
**Behavior must be identical** — same LSP commands, same feature semantics, same `lazyverilog.toml` config schema.

## Build
```bash
# Configure (run once; slang must already be installed system-wide)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build server + tests
cmake --build build -j$(nproc)
```
`slang` is found via `find_package(slang REQUIRED)` — it must be installed before configuring.
`fmt` v11 is fetched by CMake (do not use system fmt; ABI mismatch with slang).
`LspCpp` lives as a submodule at `external/LspCpp` with standalone Asio (no Boost).
## Run tests
```bash
# All tests
ctest --test-dir build
# Single test file (by tag)
./build/lazyverilog-tests "[config]"
# Single named test
./build/lazyverilog-tests "config: missing file returns defaults"
```
Tests use Catch2 v3 (`REQUIRE`/`CHECK`, tag-based filtering). `tests/test_main.cpp` is a placeholder; each feature has its own `test_*.cpp`.
## Run server
```bash
./build/lazyverilog-lsp   # communicates via stdin/stdout JSON-RPC
```
Config is loaded from `lazyverilog.toml` by walking up from the opened file's directory. Falls back to `rootUri` from the LSP `initialize` request, then `current_path()`. Reloaded on every `workspace/didChangeConfiguration` notification.

## End-to-end LSP test via nvim
Test that the server attaches and publishes diagnostics without crashing:
```bash
# Prints vim.diagnostic.get(0) after 3 s.
nvim --headless demo/memory_top.sv \
  "+lua vim.defer_fn(function() print(vim.inspect(vim.diagnostic.get(0))) end, 3000)" \
  "+sleep 4" \
  +qa

# Check server logs written by nvim's LSP client
cat /tmp/lsp-cpp.log         # JSON-RPC traffic
cat /tmp/lsp-cpp.log.stderr  # server stderr / crash output
```

### Analyzer internals (ported from Python)

- `set_extra_files(paths)` parses all extra files immediately; resets the filelist mtime cache.
- `autoinst`: uses only `body.portList`; empty `()` header → no ports returned.
- `autoarg`: text-based; scans for `module`/`endmodule`, extracts ports, returns `(...)` header range.
- `_find_instance_at_line(state, line)` finds Instance by line number (handles non-ANSI Verilog).

### Compilation guard

**Rule:** Never use `state.compilation` in any feature when `perf.background_compilation` is `false`.
All features must degrade gracefully to `SyntaxTree`-only when compilation is disabled.

### LSP commands

Server registers these `executeCommand` commands:
`lazyverilog.rtlTree`, `lazyverilog.rtlTreeReverse`, `lazyverilog.autowire`, `lazyverilog.autowirepreview`, `lazyverilog.connectInfo`, `lazyverilog.connectApply`, `lazyverilog.connectApplyPreview`, `lazyverilog.autoffPreview`, `lazyverilog.autoffApply`, `lazyverilog.autoffAllPreview`, `lazyverilog.autoffAllApply`, `lazyverilog.interface`, `lazyverilog.interfaceConnect`, `lazyverilog.interfaceDisconnect`, `lazyverilog.singleInterface`, `lazyverilog.lint`.

### Formatter

`src/features/formatter.cpp` produces output identical to the previous Python formatter:
- Token-based; idempotent; semantics-neutral (whitespace only).
- Disable regions: `// verilog_format: off` … `// verilog_format: on`; `` `define `` macro bodies passed verbatim.
- Passes (in order): `align_port_pass`, `align_assign_pass`, `align_var_pass`, `expand_instances_pass`, `format_portlist_pass`.
- `FormatOptions` has nested structs: `StatementOptions`, `PortDeclarationOptions`, `VarDeclarationOptions`, `InstanceOptions`, `PortOptions` — mirrors Python `FormatOptions` exactly.

### Lint

`src/features/lint.cpp` — `LintVisitor` (slang `SyntaxVisitor` CRTP):

| Rule | Config key | Trigger |
|---|---|---|
| case_missing_default | `[lint.statement]` | `CaseStatementSyntax` missing default item |
| explicit_begin | `[lint.statement]` | (stub — visitor hook present, check not yet wired) |
| functions_automatic | `[lint.function]` | `FunctionDeclarationSyntax` not automatic |
| explicit_function_lifetime | `[lint.function]` | `FunctionDeclarationSyntax` missing lifetime |
| explicit_task_lifetime | `[lint.function]` | `FunctionDeclarationSyntax` (task) missing lifetime |
| latch_inference_detection | `[lint.statement]` | `always_comb` with incomplete if |
| module_instantiation_style | `[lint.module]` | `HierarchyInstantiationSyntax` positional connections |
| module_pattern | `[lint.naming]` | `ModuleDeclarationSyntax` name vs regex |
| input_port_pattern | `[lint.naming]` | `ImplicitAnsiPortSyntax` / `PortDeclarationSyntax` input name vs regex |
| output_port_pattern | `[lint.naming]` | same, output direction |
| signal_pattern | `[lint.naming]` | `DataDeclarationSyntax` / `NetDeclarationSyntax` declarator names |
| register_pattern | `[lint.naming]` | nonblocking assignment LHS in `always_ff` vs regex |

`NamingConfig` (nested in `LintConfig`) holds all pattern strings; regexes compiled at `LintVisitor` construction. `[lint.naming].enable` must be `true` for naming rules to run.

## Architecture

### Data flow

```
stdin JSON-RPC
  → LspCpp RemoteEndPoint (protocol/transport)
    → LazyVerilogServer::register_handlers() (server.cpp)
      → Analyzer (per-document state management)
        → DocumentState { SyntaxTree, optional<Compilation> }
          → SyntaxIndex::build() (structural extraction)
            → feature handlers (src/features/*.cpp)
              → stdout JSON-RPC response
```

### Key types

| Type | File | Role |
|---|---|---|
| `LazyVerilogServer` | `src/server.{hpp,cpp}` | Owns `Analyzer` + `Config`; wires all LSP handlers via LspCpp lambdas |
| `Analyzer` | `src/analyzer.{hpp,cpp}` | Thread-safe document cache; `open/change/close` atomically swap `shared_ptr<const DocumentState>` |
| `DocumentState` | `src/document_state.hpp` | Immutable snapshot: raw text + slang `SyntaxTree` + optional `Compilation` |
| `SyntaxIndex` | `src/syntax_index.{hpp,cpp}` | Lightweight structural scan of a `SyntaxTree` → modules, ports, instances |
| `Config` | `src/config.{hpp,cpp}` | Parsed from `lazyverilog.toml` via toml++ |

### Thread safety

`Analyzer::map_mutex_` guards the `docs_` map. Handlers take a `shared_ptr<const DocumentState>` snapshot — reads need no lock after that. `didChange` creates a **new** `DocumentState` (immutable snapshot pattern) rather than mutating in place.

### Feature files (`src/features/`)

Each file implements one LSP capability. Most are self-contained functions called from `server.cpp` handlers. Several are stubs (`autoff`, `autofunc`, etc.). New features follow the same pattern: free function(s) in `src/features/<name>.cpp`, declared in a matching `.hpp`, linked into both `lazyverilog-lsp` and `lazyverilog-tests` targets in `CMakeLists.txt`.

### Configuration (`lazyverilog.toml`)

All lint rules default **off**; enable individually under `[lint.*]`. `[perf].background_compilation` gates whether `Compilation` is built alongside `SyntaxTree` — most features run on `SyntaxTree` only for speed. Unknown TOML keys are silently ignored; malformed TOML falls back to defaults.

### slang usage

`SyntaxTree` (parse only, no elaboration) is always built.
`Compilation` (full elaboration) is optional and controlled by `perf.background_compilation`.
Prefer `SyntaxTree`-only paths unless semantic information is strictly required.
it is not part of the server.
