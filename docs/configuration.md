# Configuration

LazyVerilog finds `lazyverilog.toml` by walking up from each file you open to the nearest one.
Opening a subdirectory does not hide the config above it, and two projects open side by side each
use their own file.

The config controls design inputs, semantic compilation, lint rules, formatter policy, RTL tree
display, inlay hints, and automation helpers.

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

This is the `lazyverilog.toml` from the repository root, included here so it cannot drift from
the file the project actually tests against.

<<< ../lazyverilog.toml

## Option reference

| Topic | Page |
|-------|------|
| Design filelist and preprocessor defines | [Design & filelist](design/index.md) |
| Formatter options | [Formatter options](formatter/options.md) |
| Formatter macro policy | [Formatter macros](formatter/macros.md) |
| Linter options with RTL examples | [Linter options](linter/options.md) |
| Background semantic diagnostics | [Background compilation](diagnostics/background-compilation.md) |
| Folding ranges | [Folding](folding/index.md) |
| RTL tree display | [RTL tree](rtl-tree/index.md) |
| Automation helpers | [AutoInst](autoinst/index.md), [AutoWire](autowire/index.md), [AutoArg](autoarg/index.md), [AutoFunc](autofunc/index.md), [AutoFF](autoff/index.md) |
