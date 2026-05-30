<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>A fast, practical SystemVerilog LSP for everyday RTL coding.</b>
</p>

<p align="center">
  <a href="https://github.com/hxxdev/LazyVerilog/stargazers">
    <img alt="GitHub stars" src="https://img.shields.io/github/stars/hxxdev/LazyVerilog?style=for-the-badge&logo=starship&label=%E2%AD%90%20Stars&color=ffd166&labelColor=2b2d42">
  </a>
  <a href="https://github.com/hxxdev/LazyVerilog/releases/latest">
    <img alt="Latest release" src="https://img.shields.io/github/v/release/hxxdev/LazyVerilog?style=for-the-badge&logo=github">
  </a>
  <a href="LICENSE">
    <img alt="License" src="https://img.shields.io/github/license/hxxdev/LazyVerilog?style=for-the-badge">
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
  <a href="#-installation">Installation</a>
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

![Formatting](demo/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Lint diagnostics</b></summary>

<!-- TODO -->

</details>

<details>
<summary><b>🌳 RTL tree</b></summary>

![RTL tree](demo/videos/RtlTree.gif)

</details>

<details>
<summary><b>🧭 Go to definition</b></summary>

![Go to definition](demo/videos/GoToDef.gif)

</details>

<details>
<summary><b>🧩 Auto-instantiation</b></summary>

![Auto-instantiation](demo/videos/AutoInst.gif)

</details>

<details>
<summary><b>🔌 Auto-wire</b></summary>

![Auto-wire](demo/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ Auto-arg</b></summary>

![Auto-arg](demo/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 Inlay hints / completion / hover</b></summary>

<!-- TODO -->

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
    <td>Auto-arg, auto-function, auto-wire, auto-FF, auto-instantiation, connect/interface helpers.</td>
  </tr>
  <tr>
    <td>🧰</td>
    <td><b>Customizable</b></td>
    <td>Project-local behavior through <code>lazyverilog.toml</code>.</td>
  </tr>
</table>

&nbsp;

## 📦 Installation

### 💤 Neovim plugin

Using <a href="https://github.com/folke/lazy.nvim"><code>lazy.nvim</code></a>:

```lua
{
  "hxxdev/lazyverilog",
  -- The repository contains large RTL test corpora as submodules; users do not
  -- need them for the editor plugin.
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

## 🏗️ Build

### ✅ Requirements

- CMake
- C++20-capable compiler

### 🧱 How to build

```bash
cmake -B build
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
safe_mode = true

[lint]
enable = true
```

</details>

## 📚 Documentation

- [`lazyverilog.toml`](lazyverilog.toml) — complete example configuration.
- [`docs/features.md`](docs/features.md) — all features at a glance.

**Project**
- [`docs/design/index.md`](docs/design/index.md) — design filelist and preprocessor defines.

**LSP & Editor**
- [`docs/lsp/index.md`](docs/lsp/index.md) — hover, definition, references, rename, completion, signature help, inlay hints, workspace symbols.

**Formatter**
- [`docs/formatter/usage.md`](docs/formatter/usage.md) — CLI usage and build instructions.
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
- [`docs/autoff/index.md`](docs/autoff/index.md) — generate always_ff blocks for registers.
- [`docs/rtl-tree/index.md`](docs/rtl-tree/index.md) — module instantiation hierarchy viewer.

**For Developers**
- [`docs/dev.md`](docs/dev.md) — build, tests, and RTL format sweep.

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
