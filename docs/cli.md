# CLI tools

Standalone binaries for scripts and CI, next to the `lazyverilog-lsp` editor server. Each reads
`lazyverilog.toml` by walking up from the target file, like the server does. They are built from
source:

```bash
cmake -B build
cmake --build build -j$(nproc) --target lazyverilog-fmt lazyverilog-lint lazyverilog-rtltree
```

| Tool | Does | Reference |
|------|------|-----------|
| `lazyverilog-fmt` | Formats a file to stdout, or in place with `-i` | [Formatter CLI](formatter/cli.md) |
| `lazyverilog-lint` | Lints a file, or every file in a `-f` filelist | [Linter CLI](linter/cli.md) |
| `lazyverilog-rtltree` | Prints the module hierarchy, or `--reverse` for parents | [RTL tree CLI](rtl-tree/cli.md) |

```bash
./build/lazyverilog-fmt -i rtl/top.sv
./build/lazyverilog-lint -f rtl/vcode.f
./build/lazyverilog-rtltree rtl/top.sv
```

## Startup banner

On a terminal, each binary prints a logo and its version to **stderr**. Piped, redirected, and
editor-started runs print nothing, so stdout is never affected. Set `LAZYVERILOG_NO_BANNER=1` to hide
it, `LAZYVERILOG_FORCE_BANNER=1` to print it in CI logs, or `NO_COLOR=1` for no colors. `--version` and
`--help` never print it.
