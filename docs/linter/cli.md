# Linter CLI

`lazyverilog-lint` lints one file, or every file in a filelist, and prints lint and compilation
diagnostics. See [CLI tools](../cli.md) to build it.

```bash
lazyverilog-lint [-f <filelist>] [--lint-only] [--compile-only] [--maxerror <n>] [--nowarn <code>]... [<file>]
```

Give at least `-f <filelist>` or `<file>`.

| Flag | Description |
|------|-------------|
| `-f <filelist>` | With no `<file>`, lint every listed file. With `<file>`, use the filelist for context and report only that file. Overrides `[design].vcode` |
| `--lint-only` | Lint rules only; skip compilation |
| `--compile-only` | Compilation diagnostics only; skip lint rules |
| `--maxerror <n>` | Stop elaborating after `<n>` errors. Default `64`, `0` for no limit |
| `--nowarn <code>` | Hide diagnostics with this code. Repeatable |
| `--version` | Print the version |

Options come from the nearest `lazyverilog.toml`: `[design]`, `[compilation]`, and `[lint]` (see
[Linter options](options.md)).

```bash
./build/lazyverilog-lint rtl/top.sv
./build/lazyverilog-lint --lint-only rtl/top.sv
./build/lazyverilog-lint -f rtl/vcode.f
```

## Output

One line per diagnostic, with 1-based line and column:

```text
<file>:<line>:<col>: <severity>: <message> [<code>]
```

Severity is `error`, `warning`, `info`, or `hint`.

## Error limit

Once a compilation passes `--maxerror` errors, slang stops and **the rest of the design is never
checked, with no notice**. A run that prints only `MissingTimeScale` errors (common when one file has a
`` `timescale `` and others do not) may hide real problems behind them. Rerun with `--maxerror 0`. The
lasting fix is a consistent timescale.

## Diagnostic codes

Every diagnostic prints its code in brackets, and `--nowarn` takes that code:

```bash
lazyverilog-lint --nowarn lint-naming-input-port --nowarn width-trunc rtl/alu.sv
```

- **Lint rules** are named `lint-<section>-<rule>`. A prefix silences its children:
  `lint` is every rule, `lint-naming` every naming rule. A value that matches nothing is rejected.
  `lazyverilog-lint --help` lists them all.
- **Compilation warnings** use slang's names (`width-trunc`, `unused-net`) and groups (`conversion`,
  `unused`, `pedantic`).
- **Compilation errors** use slang's CamelCase names (`MissingTimeScale`, `UnknownModule`). These
  are taken as written, so a misspelling silences nothing.

A hidden error no longer sets exit code 2. Hiding does not free the `--maxerror` budget: hidden errors
still count toward it.

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | No error-severity diagnostics |
| `1` | Usage error, or file could not be opened |
| `2` | At least one error-severity diagnostic |
