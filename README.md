<p align="center">
  <img src="assets/lazyverilog_logo.png" alt="LazyVerilog logo" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>A fast, practical SystemVerilog LSP for RTL coding.</b>
</p>

<p align="center">
  <a href="https://github.com/lazyverilog/LazyVerilog/actions/workflows/release.yml"><img src="https://github.com/lazyverilog/LazyVerilog/actions/workflows/release.yml/badge.svg" alt="Release"></a>
</p>

<p align="center">
  <a href="https://github.com/lazyverilog/LazyVerilog/stargazers">
    <img alt="GitHub stars" src="https://img.shields.io/github/stars/lazyverilog/LazyVerilog?style=for-the-badge&logo=starship&label=%E2%AD%90%20Stars&color=ffd166&labelColor=2b2d42">
  </a>
  <a href="https://github.com/lazyverilog/LazyVerilog/releases/latest">
    <img alt="Latest release" src="https://img.shields.io/github/v/release/lazyverilog/LazyVerilog?style=for-the-badge&logo=github">
  </a>
  <a href="https://github.com/lazyverilog/LazyVerilog/issues">
    <img alt="GitHub issues" src="https://img.shields.io/github/issues/lazyverilog/LazyVerilog?style=for-the-badge">
  </a>
  <a href="https://github.com/MikePopoloski/slang">
    <img alt="Powered by slang" src="https://img.shields.io/badge/parser-slang-blueviolet?style=for-the-badge">
  </a>
</p>

<p align="center">
  <a href="#-demo">Demo</a>
  ·
  <a href="#-why-lazyverilog">Why</a>
  ·
  <a href="#-features">Features</a>
  ·
  <a href="#-installation">Installation</a>
  ·
  <a href="#-usage">Usage</a>
  ·
  <a href="#-configuration">Configuration</a>
  ·
  <a href="#-build">Build</a>
</p>


<p align="center">
  LazyVerilog is a C++ Language Server Protocol server with a companion Neovim plugin.
  It provides formatting, linting, navigation, hover, completion, inlay hints, and RTL code actions for real SystemVerilog projects.
</p>

&nbsp;

## 🎬 Demo

<details>
<summary><b>🎨 Formatting</b></summary>

![Formatting](assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Lint diagnostics</b></summary>

![Lint diagnostics](assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ Auto-complete</b></summary>

![Auto-complete](assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 RTL tree</b></summary>

![RTL tree](assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 Folding</b></summary>

![Folding](assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 Go to definition</b></summary>

![Go to definition](assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 Interface connect</b></summary>

![Interface connect](assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 Auto-instantiation</b></summary>

![Auto-instantiation](assets/videos/AutoInst.gif)

</details>

<details>
<summary><b>🔌 Auto-wire</b></summary>

![Auto-wire](assets/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ Auto-arg</b></summary>

![Auto-arg](assets/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 Hover</b></summary>

![Hover](assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 Inlay hints</b></summary>

![Inlay hints](assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 Find references</b></summary>

![Find references](assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ Rename</b></summary>

![Rename](assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 Workspace symbols</b></summary>

![Workspace symbols](assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 Signature help</b></summary>

![Signature help](assets/videos/sig_help.gif)

</details>

&nbsp;

## ✨ Why LazyVerilog?

<table>
  <tr>
    <td>🎯</td>
    <td><b>Accurate parsing</b></td>
    <td>SystemVerilog syntax is parsed by <a href="https://github.com/MikePopoloski/slang">slang</a>.</td>
  </tr>
  <tr>
    <td>🧠</td>
    <td><b>Rich LSP features</b></td>
    <td>Inlay hints, references, go to definition, hover, rename, autocomplete, signature help, lint diagnostics.</td>
  </tr>
  <tr>
    <td>⚙️</td>
    <td><b>RTL automation</b></td>
    <td>Auto-arg, auto-function, auto-wire, auto-FF, and auto-instantiation.</td>
  </tr>
  <tr>
    <td>🧰</td>
    <td><b>Customizable</b></td>
    <td>Project-local behavior through <code>lazyverilog.toml</code>.</td>
  </tr>
</table>

&nbsp;

## 📊 Features

What LazyVerilog currently supports:

| Feature | Status | Notes |
|---------|--------|-------|
| Formatting | ✅ | Configurable via `lazyverilog.toml` |
| Lint diagnostics | ✅ | Parse diagnostics, optional semantic diagnostics, and configurable lint/style rules |
| Go to definition | ✅ | Modules, instances, ports, named arguments, symbols, and macros |
| Find references | ✅ | Symbols and macros across open files and configured project files |
| Rename symbol | ✅ | Best-effort across project files |
| Hover | ✅ | Symbol details for modules, ports, signals, parameters, typedefs, subroutines, and macros |
| Completion | ✅ | Context-aware auto-completions |
| Signature help | ✅ | Functions and tasks |
| Inlay hints | ✅ | Port directions on instantiation |
| Workspace symbols | ✅ | Modules and classes from indexed design files |
| RTL tree | ✅ | Module instantiation hierarchy |
| Auto-instantiation / Auto-wire / Auto-arg / Auto-function / Auto-FF | ✅ | Various Code Actions for RTL generation |
| Project-local config | ✅ | Full customization available by `lazyverilog.toml` at project root |

&nbsp;

## 📦 Installation

<a id="neovim-installation"></a>
<details>
<summary><b>Neovim installation</b></summary>

💤 Using <a href="https://github.com/folke/lazy.nvim"><code>lazy.nvim</code></a>:

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
```

</details>

<a id="vscode-installation"></a>
<details>
<summary><b>VS Code installation</b></summary>

#### 1. Marketplace (recommended)

Install LazyVerilog from the Visual Studio Marketplace:

https://marketplace.visualstudio.com/items?itemName=lazyverilog.lazyverilog

Then open a `.sv`, `.svh`, `.v`, or `.vh` file. The extension starts LazyVerilog automatically
for Verilog/SystemVerilog buffers and installs the matching `lazyverilog-lsp` release binary when
needed.

If you already have a locally built server binary, set it explicitly in VS Code settings:

```json
{
  "lazyverilog.serverPath": "/path/to/lazyverilog-lsp"
}
```

#### 2. Manual install from GitHub Releases

If the Marketplace version is not available yet, download the VSIX from the latest [GitHub Release](https://github.com/lazyverilog/LazyVerilog/releases/latest).

Then install it from VS Code:

1. Press `Ctrl+Shift+P` to open the Command Palette.
2. Run `Extensions: Install from VSIX...`.
3. Select the downloaded `lazyverilog-<version>.vsix` file.

</details>

## 🚀 Usage

### 1. Add a project config to your RTL project root.

Create `lazyverilog.toml` in the project root.  At minimum, point `design.vcode` to a filelist so
LazyVerilog can index modules, packages, ports, and cross-file references. This project setup is
shared by both Neovim and VS Code.

For full configuration, refer to [`lazyverilog.toml`](lazyverilog.toml) — complete example configuration.

```toml
[design]
vcode = "path/to/vcode/file"
define = ["VERILATOR", "MY_DEFINE"]

[compilation]
background_compilation = true   # run semantic compilation in background workers (richer diagnostics)
                                # Caution: can be laggy on slow machines.

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
```

Example `vcode.f`:

```text
path/to/rtl1.sv
path/to/rtl2.sv
path/to/rtl3.sv
+incdir+path/to/your/include_dir1
+incdir+path/to/your/include_dir2
```

<details>
<summary><b>Neovim usage</b></summary>

#### Open a SystemVerilog project

Open a Verilog/SystemVerilog RTL file:

Server will traverse toward the root(`/`) and look for `lazyverilog.toml`.

```bash
nvim path/to/rtl.sv
```

The Neovim plugin downloads LazyVerilog release if it is your first launch. Then Neovim plugin starts the LazyVerilog LSP automatically for `verilog` and `systemverilog` buffers.
Use `:LspInfo` to confirm that the `lazyverilog` client is attached.

#### Use standard LSP actions

LazyVerilog provides normal Neovim LSP features.  Use your existing LSP keymaps, or add mappings
like this:

```lua
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "gr", vim.lsp.buf.references)
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename)
vim.keymap.set({ "n", "v" }, "<leader>ca", vim.lsp.buf.code_action)
```

Code actions include RTL helpers such as AutoInst, AutoWire, AutoArg, AutoFunc, and AutoFF when the
cursor is on a supported construct.

#### Use LazyVerilog commands

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

</details>

<details>
<summary><b>VS Code usage</b></summary>

Open a Verilog/SystemVerilog RTL file in VS Code after installing the extension. The extension starts
LazyVerilog automatically for `verilog` and `systemverilog` buffers.

Use the Command Palette for LazyVerilog commands such as:

- `LazyVerilog: Format Document`
- `LazyVerilog: Lint Current File`
- `LazyVerilog: Lint All Files`
- `LazyVerilog: Show RTL Hierarchy`
- `LazyVerilog: Show RTL Hierarchy (Reverse)`

See the [VS Code installation instructions](#vscode-installation) above for installation and `lazyverilog.serverPath` setup.

</details>

## 🏗️ Build

### ✅ Requirements

- CMake
- C++20-capable compiler

### 🧱 How to build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc) --target lazyverilog-lsp
```

## ⚙️ Configuration

LazyVerilog reads `lazyverilog.toml` from your project root. If neovim is opened in a subdirectory, LazyVerilog walks upward until it finds the nearest config file.

The config controls design inputs, semantic compilation, lint rules, formatter policy, RTL tree display, inlay hints, and automation helpers.

<details open>
<summary>📝 Minimal example</summary>

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

</details>

## 📚 Documents

- [`lazyverilog.toml`](lazyverilog.toml) — complete example configuration.
- [`docs/features.md`](docs/features.md) — all features at a glance.
- [`docs/releases/v1.1.0.md`](docs/releases/v1.1.0.md) — latest release notes.

**Project**
- [`docs/design/index.md`](docs/design/index.md) — design filelist and preprocessor defines.

**LSP & Editor**
- [`docs/lsp/index.md`](docs/lsp/index.md) — hover, definition, references, rename, completion, signature help, inlay hints, workspace symbols.

**Formatter**
- [`docs/formatter/cli.md`](docs/formatter/cli.md) — CLI usage and build instructions.
- [`docs/formatter/options.md`](docs/formatter/options.md) — formatter options.
- [`docs/formatter/macros.md`](docs/formatter/macros.md) — macro formatting policy.

**Linter**
- [`docs/linter/options.md`](docs/linter/options.md) — linter options with RTL examples.

**Semantic Diagnostics**
- [`docs/diagnostics/background-compilation.md`](docs/diagnostics/background-compilation.md) — background semantic diagnostics.

**Auto Features**
- [`docs/autoarg/index.md`](docs/autoarg/index.md) — generate non-ANSI module port list.
- [`docs/autoinst/index.md`](docs/autoinst/index.md) — generate module instantiation port connections.
- [`docs/autowire/index.md`](docs/autowire/index.md) — generate missing signal declarations.
- [`docs/autofunc/index.md`](docs/autofunc/index.md) — generate function/task call arguments.
- [`docs/autoff/index.md`](docs/autoff/index.md) — insert reset/capture assignments into existing always_ff blocks.
- [`docs/connect.md`](docs/connect.md) — interactively wire output-to-input module instance ports.
- [`docs/interface.md`](docs/interface.md) — inspect and edit signal interfaces between instances.
- [`docs/rtl-tree/index.md`](docs/rtl-tree/index.md) — module instantiation hierarchy viewer.

**For Developers**
- [`docs/dev/test.md`](docs/dev/test.md) — build, tests, and RTL format sweep.
- [`docs/dev/files.md`](docs/dev/files.md) — design filelist cache and extra-file mtime behavior.
- [`TODO.md`](TODO.md) — planned features and known issues.

&nbsp;

## 🤝 Contributing

Contributions are welcome.

Before sending a pull request:

1. Build the project.
2. Run the relevant tests.
3. Add or update tests for formatter, lint, LSP, or automation changes.
4. Update documentation for user-visible behavior or configuration changes.

Recommended checks:

```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

> [!IMPORTANT]
> Formatter changes should include focused cases in `tests/test_formatter.cpp` and preserve idempotency and safe-mode guarantees.

&nbsp;

## 📜 License

LazyVerilog is released under the MIT License. See [`LICENSE`](LICENSE).
