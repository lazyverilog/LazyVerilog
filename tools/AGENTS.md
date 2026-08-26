<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-05-28 | Updated: 2026-05-28 -->

# tools

## Purpose
Developer utilities for benchmarking and debugging the lazyverilog server. Not part of the production build.

## Key Files

| File | Description |
|------|-------------|
| `lsp_proxy.py` | Python LSP proxy for manual testing and debugging of JSON-RPC messages |
| `parse_bench.cpp` | C++ parser benchmarking tool |
| `index_bench.cpp` | Cold project-index startup benchmark (`index-bench <project-root> [repeat]`) |
| `startup_bench.py` | Wraps `index-bench` with repeats, CPU-slice restriction, RSS/CPU accounting (`tools/startup_bench.py [project] [--cpus 0] [--trace] [--json]`) |
| `run_parse_bench_opentitan.sh` | Runs formatter performance sweep against OpenTitan RTL corpus |
| `diff_once_twice` | Idempotency checker — formats a `.sv` file once and twice, then diffs pass-by-pass logs to find the first non-idempotent pass |
| `clk_file.cpp` | `lazyverilog-clk` — per-port clock domains for one instance (`lazyverilog-clk -f <filelist> <instance>`); see `docs/clock-domain/cli.md` |

## For AI Agents

### Working In This Directory
- `lsp_proxy.py` is useful for tracing raw LSP traffic during development
- Benchmark scripts require the OpenTitan RTL submodule (`tests/rtl/opentitan/`)
- `index-bench` reproduces LSP startup cost without an editor: it loads `lazyverilog.toml`,
  runs one full project index, and waits for the published snapshot.  Combine with
  `LAZYVERILOG_TRACE_PERF=1` for per-file timings.
- Prefer `startup_bench.py` over calling `index-bench` directly: it repeats runs, reports
  user/sys CPU and peak RSS alongside wall time, and can restrict the CPU slice with
  `--cpus` to reproduce batch-scheduled or containerised nodes.  See `docs/dev/startup-perf.md`.
- Per-file tracing is only reliable single-threaded (`--cpus 0`); pool workers share an
  unsynchronized `cerr`, so parallel runs splice trace lines.
- `lazyverilog-clk` is the one tool that elaborates **without**
  `CompilationFlags::LintMode`.  Lint mode makes every module its own top, which flattens
  the hierarchy that hierarchical instance paths depend on.  The cost is that an
  incomplete filelist fails loudly here instead of being papered over.
- Its trace logic lives in `src/features/clock_domain.cpp` and is unit-tested separately
  from the CLI: `./build/lazyverilog-tests "[clkdomain]"` for clock extraction,
  `ctest --test-dir build -R clk-cli-smoke` for the end-to-end table.

<!-- MANUAL: -->
