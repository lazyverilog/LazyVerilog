# RTL Tree CLI (`lazyverilog-rtltree`)

`lazyverilog-rtltree` is a standalone command-line RTL hierarchy viewer. It builds the module
instantiation tree rooted at `<file>`'s module — forward (children) by default, or reverse
(parents) with `--reverse` — and prints it to stdout as an indented tree.

## Build

```bash
cmake -B build
cmake --build build -j$(nproc) --target lazyverilog-rtltree
```

The binary is placed at `build/lazyverilog-rtltree`.

## Usage

```bash
lazyverilog-rtltree [-f <filelist>] [--reverse] <file>
```

## Arguments

| Argument | Description |
|----------|--------------|
| `<file>` | Path to the `.sv` / `.svh` file whose module roots the tree |

## Options

| Flag | Description |
|------|-------------|
| `-f <filelist>`, `--filelist <filelist>` | Project filelist (`.f`) to index for cross-file hierarchy resolution. Overrides `lazyverilog.toml`'s `[design] vcode`. |
| `--reverse` | Build the reverse hierarchy (where `<file>`'s module is instantiated) instead of the forward hierarchy (what it instantiates). |
| `-h`, `--help` | Print usage and exit. |

## Configuration

`lazyverilog-rtltree` automatically finds `lazyverilog.toml` by walking up from `<file>`'s
directory. `[design]` and `[rtltree]` options apply exactly as they do in the LSP server's
`lazyverilog.rtlTree` / `lazyverilog.rtlTreeReverse` commands — see
[index.md](index.md#configuration) for `show_instance_name` / `show_file`.

## Examples

Forward hierarchy (what `memory_top.sv`'s module instantiates):

```bash
./build/lazyverilog-rtltree rtl/memory_top.sv
```

```text
memory_top [rtl/memory_top.sv]
  memory (u_mem0) [rtl/memory.sv]
  memory (u_mem1) [rtl/memory.sv]
```

Reverse hierarchy (where `memory.sv`'s module is instantiated), using an explicit filelist:

```bash
./build/lazyverilog-rtltree -f rtl/vcode.f --reverse rtl/memory.sv
```

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | Usage error, `<file>` could not be opened, or no module was found in `<file>` |
