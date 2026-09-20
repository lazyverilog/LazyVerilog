# Usage

## 1. Add a `lazyverilog.toml`

Put it in your project, anywhere above your RTL. The server uses the nearest one above each file
you open, so opening a subdirectory still finds it.

```toml
[design]
vcode = "vcode.f"
define = ["MY_DEFINE"]

[format]
enable_format_on_save = true

[lint]
enable = true
```

`vcode` points to a filelist. It lets LazyVerilog index modules, ports, and cross-file references:

```text
rtl/top.sv
rtl/alu.sv
+incdir+rtl/include
```

See [Design & filelist](../design/index.md) for the filelist syntax and
[Configuration](../configuration.md) for every option.

## 2. Open a file

| Editor | Guide |
|--------|-------|
| Neovim | [Usage in Neovim](neovim.md) |
| VS Code | [Usage in VS Code](vscode.md) |
