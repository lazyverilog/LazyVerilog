<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-05-28 | Updated: 2026-05-28 -->

# src/features

## Purpose
One C++ file per LSP feature or command. Each file implements the handler logic for a specific LSP capability or `executeCommand` command. All features are registered in `../server.cpp`.

## Key Files

| File | Description |
|------|-------------|
| `formatter.cpp/.hpp` | Token-based, idempotent source formatter — core logic in `format_source()` |
| `lint.cpp/.hpp` | Linting/diagnostics engine — publishes `textDocument/publishDiagnostics` |
| `autoinst.cpp/.hpp` | Auto-instantiate module instances (`lazyverilog.autoinst*`) |
| `autowire.cpp/.hpp` | Auto-connect wires (`lazyverilog.autowire`, `lazyverilog.autowirepreview`) |
| `autoff.cpp/.hpp` | Auto-connect flip-flops (`lazyverilog.autoffPreview`, `lazyverilog.autoffApply`, etc.) |
| `autofunc.cpp/.hpp` | Auto-generate functions |
| `autoarg.cpp/.hpp` | Auto-generate arguments |
| `connect.cpp/.hpp` | Port connection utilities (`lazyverilog.connectInfo`, `lazyverilog.connectApply`, etc.) |
| `definition.cpp/.hpp` | `textDocument/definition` — go-to-definition |
| `references.cpp/.hpp` | `textDocument/references` — find all references |
| `hover.cpp/.hpp` | `textDocument/hover` — hover documentation |
| `workspace_symbols.cpp/.hpp` | `workspace/symbol` — workspace-wide symbol search |
| `completion.cpp/.hpp` | `textDocument/completion` — code completion |
| `signature_help.cpp/.hpp` | `textDocument/signatureHelp` — function signature hints |
| `inlay_hints.cpp/.hpp` | `textDocument/inlayHint` — inline type/value hints |
| `code_action.cpp/.hpp` | `textDocument/codeAction` — quick fixes and refactoring |
| `rename.cpp/.hpp` | `textDocument/rename` — symbol rename across workspace |
| `folding_range.cpp/.hpp` | `textDocument/foldingRange` — fold regions; re-requested on every edit |

## For AI Agents

### Formatter Architecture
`formatter.cpp` → `format_source()` runs sequential passes:
1. `pass0_populate_metadata` — builds token metadata
2. `*_pass_v2` — named formatting passes (alignment, indentation, spacing, etc.)

Formatting is **token-based** and **idempotent**: formatting twice must yield the same result.
Config options: `../../docs/formatter/options.md`

### Registered LSP executeCommand Commands
`lazyverilog.rtlTree`, `lazyverilog.rtlTreeReverse`, `lazyverilog.autowire`, `lazyverilog.autowirepreview`, `lazyverilog.connectInfo`, `lazyverilog.connectApply`, `lazyverilog.connectApplyPreview`, `lazyverilog.autoffPreview`, `lazyverilog.autoffApply`, `lazyverilog.autoffAllPreview`, `lazyverilog.autoffAllApply`, `lazyverilog.interface`, `lazyverilog.interfaceConnect`, `lazyverilog.interfaceDisconnect`, `lazyverilog.singleInterface`, `lazyverilog.lint`

### Working In This Directory
- New feature: add `feature.cpp` + `feature.hpp`, register handler in `../server.cpp`, add `../../tests/test_feature.cpp`
- All features may use `../analyzer.hpp`, `../syntax_index.hpp`, `../config.hpp`, `../document_state.hpp`
- Hot path: `formatter.cpp` (highest call frequency — optimize carefully)
- Also hot: `folding_range.cpp` and `inlay_hints.cpp`.  Neovim re-requests both for the
  whole file on every `didChange`, and requests are answered one at a time, so work here
  lands between the user's keystroke and their completion popup.  See
  `../../docs/dev/edit-perf.md`
- A request can arrive while the buffer's parse is still running, which is exactly when an
  editor sends one: `DocumentState::tree` is null and only `text` is available.  Returning
  nothing there is not a safe default — the client applies the empty answer.  Serve the
  previous answer, or one derived from the text alone

### Testing Requirements
- Each feature has a corresponding `../../tests/test_<feature>.cpp`
- Formatter tests must verify idempotency
- Run: `ctest --test-dir ../../build`

### Common Patterns
- Feature function signature: takes document URI + position (or range) + shared state refs
- Return LSP response structs serialized to JSON by `server.cpp`

## Dependencies

### Internal
- `../analyzer.hpp` — AST access
- `../syntax_index.hpp` — symbol lookup
- `../background_compiler.hpp` — incremental parse results
- `../config.hpp` — user settings from `lazyverilog.toml`
- `../document_state.hpp` — current document text and version

<!-- MANUAL: -->
