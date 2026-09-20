# Installation

LazyVerilog ships an editor integration for Neovim and one for VS Code. Both start the
`lazyverilog-lsp` server for Verilog and SystemVerilog buffers and download the matching release
binary when it is missing.

## Neovim

Using [`lazy.nvim`](https://github.com/folke/lazy.nvim):

```lua
{
  "lazyverilog/LazyVerilog",
  submodules = false,
  ft = { "systemverilog", "verilog" },
  config = function()
    require("lazyverilog").setup()
  end,
}
```

Examples:

```lua
-- Use auto-install / PATH / managed-binary resolution.
require("lazyverilog").setup()

-- Use a local build explicitly.
require("lazyverilog").setup({
  cmd = "/path/to/lazyverilog-lsp",
})

-- Editor-side features that cost something on every edit.  Both shown at their
-- defaults; set either to false for very large RTL files or on a machine with
-- little CPU to spare, such as a shared HPC node.
require("lazyverilog").setup({
  -- 'foldmethod=expr' driven by the server's folding ranges.  Neovim
  -- re-requests the whole file's folds from every change.
  folding     = true,
  -- Inlay hints.  Neovim requests them on every change even when the server is
  -- configured to return none; this is the editor half of the switch, and
  -- `[inlay_hint].enable` in lazyverilog.toml is the server half.
  inlay_hints = true,
})
```

## VS Code

### Marketplace (recommended)

Install LazyVerilog from the
[Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=lazyverilog.lazyverilog-vscode).

Then open a `.sv`, `.svh`, `.v`, or `.vh` file. The extension starts LazyVerilog automatically
for Verilog and SystemVerilog buffers and installs the matching `lazyverilog-lsp` release binary
when needed.

If you already have a locally built server binary, set it explicitly in VS Code settings:

```json
{
  "lazyverilog.serverPath": "/path/to/lazyverilog-lsp"
}
```

### Manual install from GitHub Releases

If the Marketplace version is not available yet, download the VSIX from the latest
[GitHub Release](https://github.com/lazyverilog/LazyVerilog/releases/latest).

Then install it from VS Code:

1. Press `Ctrl+Shift+P` to open the Command Palette.
2. Run `Extensions: Install from VSIX...`.
3. Select the downloaded `lazyverilog-<version>.vsix` file.

## Server binary resolution

Editors resolve the `lazyverilog-lsp` server in this order:

1. **Explicit config path**
   - VS Code: set `lazyverilog.serverPath`.
   - Neovim: pass `cmd` to `require("lazyverilog").setup({ ... })`.
   - Use this for a locally built or custom binary that LazyVerilog should never replace.
2. **PATH binary**
   - If `lazyverilog-lsp` is available on `PATH` (`lazyverilog-lsp.exe` on Windows), the editor uses it as a user-owned binary.
   - PATH binaries are not checksum-checked or auto-updated by LazyVerilog.
3. **Managed binary**
   - If no explicit config path or PATH binary is found, the editor uses its managed storage directory and can download the matching release binary there.
   - Managed binaries are release-owned: when the plugin or VS Code extension updates, stale managed binaries can be replaced with the current checked release binary.
   - Do not put custom builds in the managed binary directory. Use an explicit config path or PATH instead.

Supported platforms, checksums, and the static Linux fallback are described in
[Binary installation](lsp/install.md).

## Build from source

Requirements: CMake and a C++20-capable compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc) --target lazyverilog-lsp
```

Next: [Usage](usage.md) and [Configuration](configuration.md).
