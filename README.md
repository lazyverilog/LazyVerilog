<p align="center">
  <img src="docs/public/logo.webp" alt="LazyVerilog logo" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>A fast, practical SystemVerilog LSP for RTL coding.</b>
</p>

<p align="center">
  <a href="https://github.com/lazyverilog/LazyVerilog/actions/workflows/ci.yml"><img src="https://github.com/lazyverilog/LazyVerilog/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
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
  <a href="https://github.com/sponsors/kjoonha">
    <img alt="Sponsor kjoonha" src="https://img.shields.io/badge/Sponsor-kjoonha-ea4aaa?style=for-the-badge&logo=githubsponsors&logoColor=white&labelColor=2b2d42">
  </a>
</p>

<p align="center">
  <b>English</b>
  ·
  <a href="docs/i18n/README.ko.md">한국어</a>
  ·
  <a href="docs/i18n/README.zh-CN.md">简体中文</a>
  ·
  <a href="docs/i18n/README.ja.md">日本語</a>
  ·
  <a href="docs/i18n/README.de.md">Deutsch</a>
  ·
  <a href="docs/i18n/README.es.md">Español</a>
  ·
  <a href="docs/i18n/README.fr.md">Français</a>
  ·
  <a href="docs/i18n/README.pt.md">Português</a>
</p>

<p align="center">
  <a href="#-demo">Demo</a>
  ·
  <a href="#-comparison">Comparison</a>
  ·
  <a href="#-features">Features</a>
  ·
  <a href="#-getting-started">Getting Started</a>
</p>


<p align="center">
  LazyVerilog is a SystemVerilog LSP written in C++ with neovim and vscode support.
  It provides formatting, linting, navigation, hover, completion, inlay hints, and RTL code actions for real SystemVerilog projects.
</p>

<p align="center">
  <img src="assets/videos/AutoInst.gif" alt="Auto-instantiation: generate a module instance with its ports connected" width="720">
</p>

<p align="center">
  <sub>If LazyVerilog saves you time, a ⭐ helps other RTL engineers find it.</sub>
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

## ⚖️ Comparison

Comparison of LazyVerilog and the two most commonly used open-source SystemVerilog LSP:
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
 and [`svlangserver`](https://github.com/imc-trading/svlangserver).

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM, classes, packages | ✅ | ⚠️ | ❌ |
| Runtime | C++ | C++ | Node.js |
| Parser | slang | Verible | own indexer |
| Diagnostics | parse, lint, semantic | parse, lint | Verilator |
| Formatting | ✅ | ✅ | via Verible |
| Go to definition | ✅ | ✅ | ✅ |
| Find references | ✅ | ✅ | ❌ |
| Rename | ✅ | ❌ | ❌ |
| Hover | ✅ | ⚠️ | ✅ |
| Completion | ✅ | ❌ | ✅ |
| Signature help | ✅ | ❌ | ✅ |
| Inlay hints | ✅ | ❌ | ❌ |
| Symbols | ✅ | ✅ | ✅ |
| Lint autofix | ❌ | ✅ | ❌ |
| RTL code generation | ✅ | ❌ | ❌ |
| RTL hierarchy view | ✅ | ❌ | ✅ |
| Interface and connect | ✅ | ❌ | ❌ |

⚠️ means partial or experimental.

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

## 🚀 Getting Started

Installation, usage, and configuration for Neovim and VS Code: **https://lazyverilog.github.io**
