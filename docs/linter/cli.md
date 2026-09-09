# Linter CLI (`lazyverilog-lint`)

`lazyverilog-lint` is a standalone command-line linter. It parses one file — or every file in a
`-f` filelist — and pretty-prints lint diagnostics and compilation diagnostics (parse errors plus
semantic diagnostics) to stdout.

## Build

```bash
cmake -B build
cmake --build build -j$(nproc) --target lazyverilog-lint
```

The binary is placed at `build/lazyverilog-lint`.

## Usage

```bash
lazyverilog-lint [-f <filelist>] [--lint-only] [--compile-only] [--maxerror <n>] [<file>]
```

At least one of `-f <filelist>` or `<file>` is required.

## Arguments

| Argument | Description |
|----------|--------------|
| `<file>` | Path to a `.sv` / `.svh` file. Report diagnostics for this file only. |

## Options

| Flag | Description |
|------|-------------|
| `-f <filelist>`, `--filelist <filelist>` | Project filelist (`.f`) to index. With no `<file>`, lint every file it lists. With `<file>`, index the filelist for cross-file/semantic context but report only `<file>`'s diagnostics. Overrides `lazyverilog.toml`'s `[design] vcode`. |
| `--lint-only` | Print only lint-rule diagnostics; skip compilation and drop parse/semantic diagnostics. |
| `--compile-only` | Print only compilation diagnostics (parse + semantic); skip lint rules. |
| `--maxerror <n>` | Maximum compilation errors before slang stops elaborating. Default `64`; `0` means unlimited. See [Error limit](#error-limit). |
| `--version` | Print the version and exit. |
| `-h`, `--help` | Print usage and exit. |

## Error limit

Slang stops elaborating once a compilation has produced more than `--maxerror`
errors. When that happens it abandons the rest of the pass, so **every
diagnostic it had not reached yet is silently never reported** — no truncation
notice is printed. The result looks like a clean design when it is not.

The default of `64` is slang's own
`CompilationOptions::errorLimit`, and it is easy to reach by accident. The
common trigger is an inconsistent timescale: as soon as one design element
carries a `` `timescale ``, slang emits `MissingTimeScale` for *every* module
and package that lacks one. A single vendor IP or testbench file is enough to
blow past 64 errors on that diagnostic alone, at which point the rest of the
design goes unchecked.

The symptom is a run that reports nothing but timescale errors:

```text
$ lazyverilog-lint -f rtl/vcode.f rtl/dut.sv
rtl/dut.sv:4:9: error: design element does not have a time scale defined but others in the design do
rtl/dut.sv:9:8: error: design element does not have a time scale defined but others in the design do
```

Lift the limit to see what was hidden behind it:

```text
$ lazyverilog-lint --maxerror 0 -f rtl/vcode.f rtl/dut.sv
rtl/dut.sv:4:9: error: design element does not have a time scale defined but others in the design do
rtl/dut.sv:9:8: error: design element does not have a time scale defined but others in the design do
rtl/dut.sv:14:12: warning: implicit conversion from 'type_b' to 'type_a'
```

`--maxerror` sets the elaboration cutoff only; it does not cap how many
diagnostics are printed, and there is no matching warning limit because slang
does not have one — warnings are always unlimited.

If a run reports only timescale errors, raise the limit before concluding the
design is clean. The durable fix is to make the timescale consistent (add one
everywhere, or drop the outlier) so the errors are not produced at all.

This flag applies to `lazyverilog-lint` only. The LSP server keeps slang's
default of 64.

## Configuration

`lazyverilog-lint` automatically finds `lazyverilog.toml` by walking up from `<file>`'s directory
(or the current directory in `-f`-only mode). `[design]`, `[compilation]`, and `[lint]` options
apply exactly as they do in the LSP server. See [options.md](options.md) for the full list of
`[lint]` options.

## Output format

One line per diagnostic:

```text
<file>:<line>:<col>: <severity>: <message>
```

`<line>` and `<col>` are 1-based. `<severity>` is one of `error`, `warning`, `info`, or `hint`.

## Examples

Lint a single file (compilation + lint diagnostics):

```bash
./build/lazyverilog-lint rtl/memory_top.sv
```

Lint a single file, lint rules only:

```bash
./build/lazyverilog-lint --lint-only rtl/memory_top.sv
```

Lint every file in a project filelist:

```bash
./build/lazyverilog-lint -f rtl/vcode.f
```

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success — no error-severity diagnostics |
| `1` | Usage error, or `<file>` could not be opened |
| `2` | At least one error-severity diagnostic was reported |
