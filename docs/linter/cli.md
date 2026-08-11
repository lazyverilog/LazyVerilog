# Linter CLI (`lazyverilog-lint`)

`lazyverilog-lint` is a standalone command-line linter. It parses one file — or every file in a
`-f` filelist — and pretty-prints lint diagnostics and compilation diagnostics (parse errors plus,
when `[compilation] background_compilation` is enabled, semantic diagnostics) to stdout.

## Build

```bash
cmake -B build
cmake --build build -j$(nproc) --target lazyverilog-lint
```

The binary is placed at `build/lazyverilog-lint`.

## Usage

```bash
lazyverilog-lint [-f <filelist>] [--lint-only] [<file>]
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
| `--lint-only` | Print only lint-rule diagnostics; drop parse and semantic (compilation) diagnostics. |
| `-h`, `--help` | Print usage and exit. |

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
