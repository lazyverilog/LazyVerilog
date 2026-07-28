<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-05-28 | Updated: 2026-07-29 -->

# tests

## Purpose
Unit and integration tests for all lazyverilog LSP features. Uses Catch2 framework. Each feature has its own test file. `test_main.cpp` is a placeholder entry point — do not add test cases to it.

## Key Files

| File | Description |
|------|-------------|
| `test_main.cpp` | Catch2 placeholder entry point (no test cases here) |
| `test_formatter.cpp` | Formatter pass tests — idempotency and correctness |
| `test_lint.cpp` | Linter/diagnostics tests |
| `test_config.cpp` | Config loading and defaults tests |
| `test_autoff.cpp` | Auto flip-flop connection feature tests |
| `test_definition.cpp` | Go-to-definition tests |
| `test_references.cpp` | Find-all-references tests |
| `test_rename.cpp` | Symbol rename tests |
| `test_syntax_index.cpp` | Syntax index lookup tests |
| `test_completion.cpp` | Completion tests |
| `test_connect.cpp` | Auto-wire / auto-connect tests |
| `test_folding_ranges.cpp` | Folding range tests |
| `test_filelist.cpp` | Filelist (`.f`) parsing and resolution tests |
| `test_path_uri.cpp` | Path / URI normalization tests |
| `test_document_sync.cpp` | Document sync / incremental update tests |
| `test_lsp_features.cpp` | General LSP feature integration tests |
| `test_inlay_hints.cpp` | Inlay hints tests |
| `test_opentitan_benchmark.cpp` | OpenTitan corpus parse/index benchmark (hidden `[.benchmark]` tag) |
| `rtl_format_sweep.cpp` | Large-scale formatter performance sweep against real RTL |
| `test.sv` | SystemVerilog fixture used by tests |

## For AI Agents

### Running Tests
```bash
# All tests
ctest --test-dir ../build

# Single feature (by tag)
../build/lazyverilog-tests "[config]"

# Single named test
../build/lazyverilog-tests "config: missing file returns defaults"
```

### Working In This Directory
- New feature → add `test_<feature>.cpp`, register in `../CMakeLists.txt`
- Mirror `src/features/<feature>.cpp` with `tests/test_<feature>.cpp`
- Tags: use `[<feature>]` Catch2 tag on each test case
- Do NOT add cases to `test_main.cpp`

### Testing Requirements
- Tests must pass before merging
- Formatter tests must verify idempotency (format twice, result unchanged)

### Common Patterns
- Catch2 `TEST_CASE("name", "[tag]")` structure
- SV fixture strings inline or loaded from `test.sv`

## Dependencies

### Internal
- All test files link against the lazyverilog static library built from `../src/`

### External
- Catch2 — test framework

<!-- MANUAL: -->
