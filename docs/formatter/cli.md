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
| `2` | `safe_mode` triggered — formatting would have changed non-whitespace content |
