# CLAUDE.md
## Goal of this project
This is a SystemVerilog LSP server written in C++.
## Build
```bash
# Configure (run once)
cmake -B build

# Build server + tests
cmake --build build -j$(nproc)
```
## Run tests
```bash
# All tests
ctest --test-dir build
# Single test file (by tag)
./build/lazyverilog-tests "[config]"
# Single named test
./build/lazyverilog-tests "config: missing file returns defaults"
```
`tests/test_main.cpp` is a placeholder; each feature has its own `test_*.cpp`.
## Run server
```bash
./build/lazyverilog-lsp   # communicates via stdin/stdout JSON-RPC
```
Config is loaded from `lazyverilog.toml` by walking up from the opened file's directory. Falls back to `rootUri` from the LSP `initialize` request, then `current_path()`. Reloaded on every `workspace/didChangeConfiguration` notification.

## End-to-end LSP test via nvim
Test that the server attaches and publishes diagnostics without crashing:
```bash
nvim --headless demo/memory_top.sv \
  +qa
```
## Design description
### Formatter
formatting core logic is implemented in `src/features/formatter.cpp` function format_source()
- Goes through Passes
1. pass0_populate_metadata
2. *_pass_v2
- Token-based; idempotent;
- lazyverilog.toml located at repo root contains customizable formatting options.
- Description of each option is documented at docs/formatter/options.md
### Lint
Linting logic is implemented in `src/features/lint.cpp`
### LSP commands
Server registers these `executeCommand` commands:
`lazyverilog.rtlTree`, `lazyverilog.rtlTreeReverse`, `lazyverilog.autowire`, `lazyverilog.autowirepreview`, `lazyverilog.connectInfo`, `lazyverilog.connectApply`, `lazyverilog.connectApplyPreview`, `lazyverilog.autoffPreview`, `lazyverilog.autoffApply`, `lazyverilog.autoffAllPreview`, `lazyverilog.autoffAllApply`, `lazyverilog.interface`, `lazyverilog.interfaceConnect`, `lazyverilog.interfaceDisconnect`, `lazyverilog.singleInterface`, `lazyverilog.lint`.
### Configuration (`lazyverilog.toml`)
- customizable formatting options
- customizable linting options
- some LSP commands customization
