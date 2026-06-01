# Developer Guide

## Build

```bash
cmake -B build
cmake --build build -j$(nproc)
```

To build only specific targets:

```bash
cmake --build build -j$(nproc) --target lazyverilog-lsp
cmake --build build -j$(nproc) --target lazyverilog-fmt
cmake --build build -j$(nproc) --target lazyverilog-rtl-format-test
cmake --build build -j$(nproc) --target lazyverilog-tests
```

---

## Running Tests

```bash
ctest --test-dir build
```

Run a single tag:

```bash
./build/lazyverilog-tests "[formatter]"
./build/lazyverilog-tests "[lint]"
./build/lazyverilog-tests "[hover]"
```

Run a single named test:

```bash
./build/lazyverilog-tests "formatter: function calls support block layout"
```

### Test files and what they cover

| File | Tag | Coverage |
|------|-----|----------|
| `test_formatter.cpp` | `[formatter]` | Formatter pass correctness and idempotency — function call layout, module headers, macro handling, comment stability, alignment, spacing, and safe-mode edge cases |
| `test_lint.cpp` | `[lint]` | All lint rule categories — trailing whitespace, naming patterns, module rules, statement rules, multi-file autoinst diagnostics |
| `test_lsp_features.cpp` | `[hover]` | Hover markdown output — symbol kind, type signature, unpacked dimensions, scope disambiguation |
| `test_definition.cpp` | `[definition]` | Go-to-definition — instance → module, named port → declaration, macro → define, cross-file via vcode filelist |
| `test_references.cpp` | `[references]` | Find-references — token resolution against syntax-tree definition |
| `test_rename.cpp` | `[rename]` | Rename — identifier range preparation and workspace-edit generation across all references |
| `test_inlay_hints.cpp` | `[inlay]` | Inlay hints — port coverage counts, per-port metadata, visible-range filtering, cross-file module resolution, non-ANSI ports |
| `test_document_sync.cpp` | `[sync]` | Document sync — open/change/close lifecycle, incremental update, concurrent read safety |
| `test_syntax_index.cpp` | `[index]` | Syntax index — module and port discovery, instantiation lookup |
| `test_autoff.cpp` | `[autoff]` `[autowire]` | AutoFF assignment insertion; AutoWire — cached extra-file modules, scoping across multiple modules |
| `test_config.cpp` | `[config]` | Config parsing — defaults, unknown keys, all sections, malformed TOML, macro role conflict reporting |

---

## RTL Format Sweep

The RTL format sweep runs `lazyverilog-fmt` against a directory tree of `.sv`/`.svh` files. For each file it:

1. Formats once and checks for a non-zero exit code (fail).
2. Formats the output a second time and checks that the result is identical (idempotency).

### Build

```bash
cmake --build build -j$(nproc) --target lazyverilog-rtl-format-test
```

### Usage

```bash
./build/lazyverilog-rtl-format-test [--perf] <formatter-binary> <rtl-root>
```

### Arguments

| Argument | Description |
|----------|-------------|
| `<formatter-binary>` | Path to `lazyverilog-fmt` binary |
| `<rtl-root>` | Root directory to search for `.sv` / `.svh` files recursively |

### Options

| Flag | Description |
|------|-------------|
| `--perf` | Print per-file timing in ms and report all files that took more than 1000 ms |
| `-h` / `--help` | Print usage |

### Examples

Basic sweep — checks for crashes and idempotency:

```bash
./build/lazyverilog-rtl-format-test ./build/lazyverilog-fmt ./tests/rtl
```

With performance profiling — also logs slow files:

```bash
./build/lazyverilog-rtl-format-test --perf ./build/lazyverilog-fmt ./tests/rtl | tee all.log
```

### Output

Each processed file prints a progress line:

```
  [42/1000] pass=41 fail=0 not-idempotent=0 time=12ms path/to/file.sv
```

At the end:

```
  result: pass       # all files passed
  result: fail       # one or more failures
  fail: 2            # crash/error count
  not-idempotent: 1  # idempotency failure count
```

Exit codes: `0` = all pass, `1` = failures found, `2` = bad arguments, `130` = interrupted (Ctrl-C).
