# Formatter CLI (`lazyverilog-fmt`)

`lazyverilog-fmt` is a standalone command-line formatter. It reads a single SystemVerilog file, applies the formatter, and writes the result to stdout (or in-place).

## Build

```bash
cmake -B build
cmake --build build -j$(nproc) --target lazyverilog-fmt
```

The binary is placed at `build/lazyverilog-fmt`.

## Usage

```bash
lazyverilog-fmt [-i|--in-place] [--log <log-dir>] <file>
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<file>` | Path to the `.sv` / `.svh` file to format |

## Options

| Flag | Description |
|------|-------------|
| `-i`, `--in-place` | Write formatted output back to the source file instead of stdout |
| `--log <log-dir>` | Write internal formatter pass logs to `<log-dir>` for debugging |
| `--version` | Print the version and exit. |

## Startup banner

Every lazyverilog binary prints the project logo and its version when it starts:

```text
<ascii logo>
                              lazyverilog-fmt  v2.1.0
```

It goes to **stderr**, never stdout, and only when stderr is a terminal. A piped,
redirected or editor-spawned run prints nothing at all, so `lazyverilog-fmt`'s
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

`lazyverilog-fmt` automatically finds `lazyverilog.toml` by walking up from the file's directory. All `[format]` options in that file apply. If no config file is found, formatter defaults are used.

See [options.md](options.md) for the full list of `[format]` options.

## Examples

Format to stdout:

```bash
./build/lazyverilog-fmt rtl/m_alu.sv
```

Format in-place:

```bash
./build/lazyverilog-fmt -i rtl/m_alu.sv
```

Format and write pass logs to a directory:

```bash
mkdir -p /tmp/fmt-log
./build/lazyverilog-fmt --log /tmp/fmt-log rtl/m_alu.sv
```

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | File not found or cannot be read/written |
| `2` | Formatter safety check failed — formatting would have changed non-whitespace content or the token stream |
