# Formatter CLI

`lazyverilog-fmt` formats one SystemVerilog file. See [CLI tools](../cli.md) to build it.

```bash
lazyverilog-fmt [-i|--in-place] [--log <log-dir>] <file>
```

| Flag | Description |
|------|-------------|
| `-i`, `--in-place` | Write the result back to the file instead of stdout |
| `--log <log-dir>` | Write per-pass logs to `<log-dir>`, for debugging |
| `--version` | Print the version |

It reads `[format]` from the nearest `lazyverilog.toml` above the file, and uses defaults if there is
none. See [Formatter options](options.md).

```bash
./build/lazyverilog-fmt rtl/m_alu.sv        # to stdout
./build/lazyverilog-fmt -i rtl/m_alu.sv     # in place
```

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | File not found, or cannot be read or written |
| `2` | Safety check failed: formatting would have changed more than whitespace |
