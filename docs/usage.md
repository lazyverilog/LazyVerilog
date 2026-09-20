# Usage

## 1. Add a project config to your RTL project root

Create `lazyverilog.toml` in the project root. At minimum, point `design.vcode` to a filelist so
LazyVerilog can index modules, packages, ports, and cross-file references.

**Put it anywhere above your RTL.** The server finds `lazyverilog.toml` by walking up from each
file you open to the nearest one, so opening a subdirectory does not hide the config above it.

```toml
[design]
vcode = "path/to/vcode/file"
define = ["VERILATOR", "MY_DEFINE"]

[compilation]
background_compilation = true   # run semantic compilation in background workers (richer diagnostics)
                                # Caution: can be laggy on slow machines.
                                # Read per project; compilation starts 1.5 s after you stop typing.

[format]
enable_format_on_save = true # auto-formatting on file save.
indent_size = 4

[lint]
enable = true # show lint diagnostics

[lint.naming]
enable = true
severity = "warning"
input_port_pattern = "^i_.*$"  # regex; input ports should start with i_
output_port_pattern = "^o_.*$" # regex; output ports should start with o_

[inlay_hint]
enable = true

[folding]
enable = true
```

Example `vcode.f`:

```text
path/to/rtl1.sv
path/to/rtl2.sv
path/to/rtl3.sv
+incdir+path/to/your/include_dir1
+incdir+path/to/your/include_dir2
```

See [Configuration](configuration.md) for every option, and
[Design & filelist](design/index.md) for how filelists and defines are read.

## 2. Open a project

### Neovim

```bash
nvim path/to/rtl.sv
```

The Neovim plugin downloads the LazyVerilog release on first launch, then starts the server
automatically for `verilog` and `systemverilog` buffers. Use `:LspInfo` to confirm that the
`lazyverilog` client is attached.

LazyVerilog provides the standard Neovim LSP features. Use your existing LSP keymaps, or add
mappings like these:

```lua
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "gr", vim.lsp.buf.references)
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename)
vim.keymap.set({ "n", "v" }, "<leader>ca", vim.lsp.buf.code_action)
```

Code actions include the RTL helpers AutoInst, AutoWire, AutoArg, AutoFunc, and AutoFF when the
cursor is on a supported construct.

| Command | Description |
|---------|-------------|
| `:Format` | Format the current buffer or visual range |
| `:Lint` | Show diagnostics for the current buffer |
| `:LintAll` | Show diagnostics for indexed project files |
| `:RtlTree` | Open the module instantiation hierarchy |
| `:RtlTreeReverse` | Open reverse hierarchy from the current module |
| `:Interface <inst>` | Inspect one instance interface |
| `:Interface <inst1> <inst2>` | Inspect and edit connections between two instances |
| `:Connect <module1> <module2>` | Interactively connect module instances through the hierarchy |

### VS Code

Open a Verilog or SystemVerilog RTL file after installing the extension. The extension starts
LazyVerilog automatically for `verilog` and `systemverilog` buffers.

Use the Command Palette for LazyVerilog commands:

- `LazyVerilog: Format Document`
- `LazyVerilog: Lint Current File`
- `LazyVerilog: Lint All Files`
- `LazyVerilog: Show RTL Hierarchy`
- `LazyVerilog: Show RTL Hierarchy (Reverse)`

See [Installation](installation.md) for installation and the `lazyverilog.serverPath` setting.
