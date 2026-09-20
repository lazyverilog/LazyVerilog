# CLI tools

LazyVerilog also ships standalone command-line binaries for scripting and CI, alongside the
`lazyverilog-lsp` editor server. Each reads `lazyverilog.toml` the same way the LSP server does,
walking up from the target file's directory.

## `lazyverilog-fmt`: standalone formatter

Formats a single file to stdout, or in place with `-i`.

```bash
cmake --build build -j$(nproc) --target lazyverilog-fmt
./build/lazyverilog-fmt -i rtl/memory_top.sv
```

| Flag | Description |
|------|-------------|
| `-i`, `--in-place` | Write formatted output back to the source file instead of stdout |
| `--log <log-dir>` | Write internal formatter pass logs to `<log-dir>` for debugging |

Full reference: [Formatter CLI](formatter/cli.md).

## `lazyverilog-lint`: standalone linter

Lints one file, or every file in a `-f` filelist, and pretty-prints lint diagnostics and
compilation diagnostics (`<file>:<line>:<col>: <severity>: <message>`).

```bash
cmake --build build -j$(nproc) --target lazyverilog-lint
./build/lazyverilog-lint rtl/memory_top.sv
./build/lazyverilog-lint -f rtl/vcode.f
```

| Flag | Description |
|------|-------------|
| `-f <filelist>` | Lint every file in a project filelist instead of (or in addition to) `<file>` |
| `--lint-only` | Print only lint-rule diagnostics; drop parse/semantic diagnostics |

Full reference: [Linter CLI](linter/cli.md).

## `lazyverilog-rtltree`: standalone RTL hierarchy viewer

Prints the module instantiation hierarchy rooted at `<file>`'s module as an indented tree:
forward (children) by default, or `--reverse` (parents).

```bash
cmake --build build -j$(nproc) --target lazyverilog-rtltree
./build/lazyverilog-rtltree rtl/memory_top.sv
./build/lazyverilog-rtltree --reverse rtl/memory.sv
```

| Flag | Description |
|------|-------------|
| `-f <filelist>` | Project filelist for cross-file hierarchy resolution |
| `--reverse` | Build the reverse hierarchy instead of the forward one |

Full reference: [RTL tree CLI](rtl-tree/cli.md).
