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
lazyverilog-lint [-f <filelist>] [--lint-only] [--compile-only] [--maxerror <n>]
                 [--nowarn <code>]... [<file>]
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
| `--nowarn <code>` | Do not report diagnostics with this code. May be repeated. See [Diagnostic codes](#diagnostic-codes). |
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

`--nowarn MissingTimeScale` hides them from the report, but note that it does
*not* give the error budget back — see
[What `--nowarn` does and does not do](#what---nowarn-does-and-does-not-do).

This flag applies to `lazyverilog-lint` only. The LSP server keeps slang's
default of 64.

## Diagnostic codes

Every diagnostic is printed with its code in brackets, and that code is exactly what `--nowarn`
takes:

```text
$ lazyverilog-lint rtl/alu.sv
rtl/alu.sv:3:18: info: [naming] input port 'address' does not match pattern '^i_.*$' [lint-naming-input-port]
rtl/alu.sv:12:5: warning: implicit conversion truncates from 8 to 4 bits [width-trunc]

$ lazyverilog-lint --nowarn lint-naming-input-port --nowarn width-trunc rtl/alu.sv
```

`--nowarn` may be given more than once. `lazyverilog-lint --help` prints the full list of
lint-rule codes.

### Lint-rule codes

lazyverilog's own rules are named `lint-<section>-<rule>`, and `--nowarn` matches on `-`
boundaries, so a parent silences its children:

| Value | Silences |
|-------|----------|
| `lint` | every lint rule |
| `lint-naming` | every rule under `[lint.naming]` |
| `lint-naming-module` | just that rule |

A value that matches no rule is a typo, and is rejected rather than silently matching nothing —
`lint-nam` is an error, not a prefix of `lint-naming`.

The full set, and the `lazyverilog.toml` key that turns each rule on, is in
[options.md](options.md#diagnostic-codes).

### Compilation codes

Compilation diagnostics come from slang and keep slang's own names.

Warnings carry slang's `-W` option name (`width-trunc`, `implicit-conv`, `unused-net`, …), and
slang's warning groups work too — `default`, `extra`, `pedantic`, `conversion`, `unused`,
`parentheses`. A group stands for its members, so `--nowarn conversion` silences `width-trunc`
along with the rest of that group.

Slang **errors** have no `-W` name, because an error is not something slang expects to be turned
off. They are named by their slang diagnostic name instead — `MissingTimeScale`,
`UnknownDirective`, `UnknownModule` — again, exactly as printed in brackets.

slang offers no way to look those CamelCase names up, so they are taken as written: a misspelled
one silences nothing rather than being reported. Every lowercase code *is* checked against slang
and rejected if unknown.

### What `--nowarn` does and does not do

A suppressed diagnostic is dropped before anything else looks at it, so **a suppressed error also
stops setting exit code 2**. That is the point of suppressing it, and it is what makes the flag
usable in CI.

Suppression happens on the finished diagnostic list, not inside slang's `DiagnosticEngine`. The
engine can only silence slang warnings — it has nothing to say about lazyverilog's lint rules or
about slang errors, which is where the need usually starts. One mechanism covering all three is
worth more than a second, narrower one that behaves differently. The cost is that a suppressed
diagnostic is still produced, so `--nowarn`:

- does not make elaboration faster, and
- does not change what counts against [`--maxerror`](#error-limit).

That second point matters for the timescale case below: `--nowarn MissingTimeScale` hides those
errors from the report, but they still consume the error budget that cut elaboration short. Raise
`--maxerror` as well, or the diagnostics hidden behind the limit stay hidden.

## Startup banner

Every lazyverilog binary prints the project logo and its version when it starts:

```text
<ascii logo>
                              lazyverilog-lint  v2.1.0
```

It goes to **stderr**, never stdout, and only when stderr is a terminal. A piped,
redirected or editor-spawned run prints nothing at all, so `lazyverilog-lint`'s
stdout is byte-for-byte what it was before -- scripts, `ctest`, and
`lazyverilog-lsp`'s JSON-RPC stream are unaffected.

When the terminal is narrower than the logo, a single line carrying the same two
facts is printed instead of a drawing wrapped into nonsense.

| Variable | Effect |
|----------|--------|
| `LAZYVERILOG_NO_BANNER` | Set to anything but `0` to suppress the banner even on a terminal. |
| `LAZYVERILOG_FORCE_BANNER` | Set to anything but `0` to print it even when stderr is not a terminal -- useful for stamping the version into a captured CI log. `LAZYVERILOG_NO_BANNER` still wins. |
| `NO_COLOR` | Set to anything but `0` to print the banner without ANSI colour ([no-color.org](https://no-color.org)). |

`--version` and `--help` never print the banner; they stay machine-readable.

## Configuration

`lazyverilog-lint` automatically finds `lazyverilog.toml` by walking up from `<file>`'s directory
(or the current directory in `-f`-only mode). `[design]`, `[compilation]`, and `[lint]` options
apply exactly as they do in the LSP server. See [options.md](options.md) for the full list of
`[lint]` options.

## Output format

One line per diagnostic:

```text
<file>:<line>:<col>: <severity>: <message> [<code>]
```

`<line>` and `<col>` are 1-based. `<severity>` is one of `error`, `warning`, `info`, or `hint`.
`<code>` is the identifier of the rule or slang diagnostic that produced the line, and is exactly
what `--nowarn` takes — see [Diagnostic codes](#diagnostic-codes).

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
