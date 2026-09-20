# Configuration

Everything is set in `lazyverilog.toml`. The server uses the nearest one above each file you open, so
two projects open side by side each use their own.

## Minimal example

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]

[format]
enable_format_on_save = true
indent_size = 4

[lint]
enable = true
```

## Complete example

The `lazyverilog.toml` from the repository root, imported here so it cannot drift from the file the
project tests against.

<<< ../lazyverilog.toml

## Option reference

| Topic | Page |
|-------|------|
| Filelist and defines | [Design & filelist](design/index.md) |
| Formatter | [Options](formatter/options.md), [macro policy](formatter/macros.md) |
| Linter | [Linter options](linter/options.md) |
| Semantic diagnostics | [Background compilation](diagnostics/background-compilation.md) |
| Folding and inlay hints | [LSP features](lsp/index.md) |
| RTL tree | [RTL tree](rtl-tree/index.md) |
| Code generation | [AutoInst](autoinst/index.md), [AutoWire](autowire/index.md), [AutoArg](autoarg/index.md), [AutoFunc](autofunc/index.md), [AutoFF](autoff/index.md) |
