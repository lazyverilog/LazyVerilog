# RTL Tree CLI

`lazyverilog-rtltree` prints the module hierarchy rooted at a file's module as an indented tree. See
[CLI tools](../cli.md) to build it.

```bash
lazyverilog-rtltree [-f <filelist>] [--reverse] <file>
```

| Flag | Description |
|------|-------------|
| `-f <filelist>` | Filelist to index for cross-file resolution. Overrides `[design].vcode` |
| `--reverse` | Show where the module is instantiated, instead of what it instantiates |
| `--version` | Print the version |

It reads `[design]` and `[rtltree]` from the nearest `lazyverilog.toml`, as the editor commands do.
See [RTL tree](index.md) for `show_instance_name` and `show_file`.

```bash
./build/lazyverilog-rtltree rtl/memory_top.sv
```

```text
memory_top [rtl/memory_top.sv]
  memory (u_mem0) [rtl/memory.sv]
  memory (u_mem1) [rtl/memory.sv]
```

```bash
./build/lazyverilog-rtltree -f rtl/vcode.f --reverse rtl/memory.sv
```

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | Usage error, unreadable file, or no module in the file |
