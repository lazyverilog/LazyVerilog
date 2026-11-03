> 本文档为 Claude 自动翻译生成，英文原文 [README.md](../../README.md) 为权威版本。

<p align="center">
  <img src="../../docs/public/logo.webp" alt="LazyVerilog logo" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>面向 RTL 开发的快速、实用的 SystemVerilog LSP。</b>
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
  <a href="../../README.md">English</a>
  ·
  <a href="../../docs/i18n/README.ko.md">한국어</a>
  ·
  <b>简体中文</b>
</p>

<p align="center">
  <a href="#-演示">演示</a>
  ·
  <a href="#-对比">对比</a>
  ·
  <a href="#-功能特性">功能特性</a>
  ·
  <a href="#-快速开始">快速开始</a>
</p>


<p align="center">
  LazyVerilog 是一个用 C++ 编写的 SystemVerilog LSP，支持 neovim 和 vscode。
  它为真实的 SystemVerilog 项目提供格式化、lint、代码导航、悬浮提示、自动补全、内联提示以及 RTL 代码操作。
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="Auto-instantiation: generate a module instance with its ports connected" width="720">
</p>

<p align="center">
  <sub>如果 LazyVerilog 为你节省了时间，点一个 ⭐ 能帮助更多 RTL 工程师发现它。</sub>
</p>

&nbsp;

## 🎬 演示

<details>
<summary><b>🎨 格式化</b></summary>

![Formatting](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Lint 诊断</b></summary>

![Lint diagnostics](../../assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ 自动补全</b></summary>

![Auto-complete](../../assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 RTL 层次树</b></summary>

![RTL tree](../../assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 代码折叠</b></summary>

![Folding](../../assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 跳转到定义</b></summary>

![Go to definition](../../assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 Interface 连接</b></summary>

![Interface connect](../../assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 自动实例化</b></summary>

![Auto-instantiation](../../assets/videos/AutoInst.gif)

</details>

<details>
<summary><b>🔌 自动连线</b></summary>

![Auto-wire](../../assets/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ 自动端口列表</b></summary>

![Auto-arg](../../assets/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 悬浮提示</b></summary>

![Hover](../../assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 内联提示</b></summary>

![Inlay hints](../../assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 查找引用</b></summary>

![Find references](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ 重命名</b></summary>

![Rename](../../assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 工作区符号</b></summary>

![Workspace symbols](../../assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 签名帮助</b></summary>

![Signature help](../../assets/videos/sig_help.gif)

</details>

&nbsp;

## ⚖️ 对比

LazyVerilog 与两款最常用的开源 SystemVerilog LSP 的对比：
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
和 [`svlangserver`](https://github.com/imc-trading/svlangserver)。

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM、class、package | ✅ | ⚠️ | ❌ |
| 运行时 | C++ | C++ | Node.js |
| 解析器 | slang | Verible | 自研索引器 |
| 诊断 | 语法解析、lint、语义 | 语法解析、lint | Verilator |
| 格式化 | ✅ | ✅ | 依赖 Verible |
| 跳转到定义 | ✅ | ✅ | ✅ |
| 查找引用 | ✅ | ✅ | ❌ |
| 重命名 | ✅ | ❌ | ❌ |
| 悬浮提示 | ✅ | ⚠️ | ✅ |
| 自动补全 | ✅ | ❌ | ✅ |
| 签名帮助 | ✅ | ❌ | ✅ |
| 内联提示 | ✅ | ❌ | ❌ |
| 符号 | ✅ | ✅ | ✅ |
| Lint 自动修复 | ✅ (`--fix`) | ✅ | ❌ |
| RTL 代码生成 | ✅ | ❌ | ❌ |
| RTL 层次视图 | ✅ | ❌ | ✅ |
| Interface 与连接 | ✅ | ❌ | ❌ |

⚠️ 表示部分支持或处于实验阶段。

&nbsp;

## 📊 功能特性

LazyVerilog 目前支持的功能：

| 功能 | 状态 | 说明 |
|---------|--------|-------|
| 格式化 | ✅ | 可通过 `lazyverilog.toml` 配置 |
| Lint 诊断 | ✅ | 语法解析诊断、可选的语义诊断，以及可配置的 lint/风格规则 |
| Lint 快速修复 | ⚠️ | 补全缺失的 `default`、添加 `automatic` 生命周期 |
| 跳转到定义 | ✅ | module、实例、端口、命名参数、符号和宏 |
| 查找引用 | ✅ | 在已打开文件和已配置的项目文件中查找符号与宏 |
| 重命名符号 | ✅ | 在项目文件范围内尽力重命名 |
| 悬浮提示 | ✅ | 显示 module、端口、信号、parameter、typedef、子程序和宏的符号详情 |
| 自动补全 | ✅ | 上下文感知的自动补全 |
| 签名帮助 | ✅ | function 和 task |
| 内联提示 | ✅ | 实例化时显示端口方向 |
| 文档符号 | ✅ | 当前文件的大纲 |
| 工作区符号 | ✅ | module、interface、package、class 及其方法；function、task 和 typedef；支持 `pkg::name` 查询 |
| RTL 层次树 | ✅ | module 实例化层次结构 |
| 自动实例化 / 自动连线 / 自动端口列表 / 自动 function / 自动 FF | ✅ | 用于 RTL 生成的各类 Code Action |
| 项目级配置 | ✅ | 通过项目根目录下的 `lazyverilog.toml` 完全自定义 |

&nbsp;

## 🚀 快速开始

Neovim 和 VS Code 的安装、使用与配置说明：**https://lazyverilog.github.io**
