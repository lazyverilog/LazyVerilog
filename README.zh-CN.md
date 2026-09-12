<p align="center">
  <img src="assets/lazyverilog_logo.png" alt="LazyVerilog logo" width="260">
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
  <a href="README.md">English</a>
  ·
  <a href="README.ko.md">한국어</a>
  ·
  <b>简体中文</b>
</p>

<p align="center">
  <a href="#demo">演示</a>
  ·
  <a href="#why">为什么选择 LazyVerilog</a>
  ·
  <a href="#features">功能</a>
  ·
  <a href="#comparison">对比</a>
  ·
  <a href="#installation">安装</a>
  ·
  <a href="#usage">使用</a>
  ·
  <a href="#cli-tools">命令行工具</a>
  ·
  <a href="#configuration">配置</a>
  ·
  <a href="#build">构建</a>
</p>


<p align="center">
  LazyVerilog 是一个用 C++ 编写的 SystemVerilog LSP，支持 neovim 和 vscode。
  它为真实的 SystemVerilog 工程提供格式化、代码检查、跳转导航、悬停提示、自动补全、内联提示以及 RTL 代码操作。
</p>

&nbsp;

<a id="demo"></a>

## 🎬 演示

<details>
<summary><b>🎨 格式化</b></summary>

![Formatting](assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 代码检查诊断</b></summary>

![Lint diagnostics](assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ 自动补全</b></summary>

![Auto-complete](assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 RTL 层次树</b></summary>

![RTL tree](assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 代码折叠</b></summary>

![Folding](assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 跳转到定义</b></summary>

![Go to definition](assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 接口连接</b></summary>

![Interface connect](assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 自动例化</b></summary>

![Auto-instantiation](assets/videos/AutoInst.gif)

</details>

<details>
<summary><b>🔌 自动连线</b></summary>

![Auto-wire](assets/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ 自动端口列表</b></summary>

![Auto-arg](assets/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 悬停提示</b></summary>

![Hover](assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 内联提示</b></summary>

![Inlay hints](assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 查找引用</b></summary>

![Find references](assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ 重命名</b></summary>

![Rename](assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 工作区符号</b></summary>

![Workspace symbols](assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 签名帮助</b></summary>

![Signature help](assets/videos/sig_help.gif)

</details>

&nbsp;

<a id="why"></a>

## ✨ 为什么选择 LazyVerilog？

<table>
  <tr>
    <td>🎯</td>
    <td><b>精确的语法解析</b></td>
    <td>SystemVerilog 语法由 <a href="https://github.com/MikePopoloski/slang">slang</a> 解析。</td>
  </tr>
  <tr>
    <td>🧠</td>
    <td><b>丰富的 LSP 功能</b></td>
    <td>内联提示、查找引用、跳转到定义、悬停提示、重命名、自动补全、签名帮助、代码检查诊断。</td>
  </tr>
  <tr>
    <td>⚙️</td>
    <td><b>RTL 自动化</b></td>
    <td>自动端口列表（Auto-arg）、自动函数（Auto-function）、自动连线（Auto-wire）、自动寄存器（Auto-FF）和自动例化（Auto-instantiation）。</td>
  </tr>
  <tr>
    <td>🧰</td>
    <td><b>可定制</b></td>
    <td>通过 <code>lazyverilog.toml</code> 定制工程内的行为。</td>
  </tr>
</table>

&nbsp;

<a id="comparison"></a>

## ⚖️ 对比

LazyVerilog 与两个最常用的开源 SystemVerilog LSP：
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
和 [`svlangserver`](https://github.com/imc-trading/svlangserver) 的对比。

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| **UVM / 类 / 包支持** | ✅ | ⚠️ | ❌ 其自身文档称："doesn't understand most verification specific concepts (e.g. classes)" |
| **性能** | ✅ 原生 C++ | ✅ 原生 C++ | ❌ Node.js 运行时 |
| 解析器 | [slang](https://github.com/MikePopoloski/slang) | Verible 自研的 SystemVerilog 解析器 | 自研的轻量索引器 |
| 诊断 | ✅ 解析诊断 + 可配置的 lint 规则 + 可选的语义诊断 | 语法错误 + Verible 的 lint 规则集 | 委托给 Verilator |
| 格式化 | 内置 | 内置 | 委托给 `verible-verilog-format` |
| 跳转到定义 | ✅ | ✅ | ✅ |
| 查找引用 | ✅ | ✅ | ❌ |
| 重命名符号 | ✅ | ❌（仅列为计划中） | ❌ |
| 悬停提示 | ✅ | ⚠️（实验性） | ✅ |
| 自动补全 | ✅ | ❌ | ✅ |
| 签名帮助 | ✅ | ❌ | ✅ |
| 内联提示 | ✅ 例化时显示端口方向 | ❌ | ❌ |
| 文档 / 工作区符号 | ✅ | ✅ 文档大纲 | ✅ |
| lint 自动修复代码操作 | ❌ | ✅ | ❌ |
| RTL 代码生成 | ✅ 自动例化 / 自动连线 / 自动端口列表 / 自动函数 / 自动寄存器 | ❌ | ❌ |
| RTL 层次视图 | ✅ | ❌ | ✅ |
| 接口 / 例化连接工具 | ✅ `:Interface`、`:Connect` | ❌ | ❌ |

&nbsp;

<a id="features"></a>

## 📊 功能

LazyVerilog 目前支持的功能：

| 功能 | 状态 | 说明 |
|---------|--------|-------|
| 格式化 | ✅ | 通过 `lazyverilog.toml` 配置 |
| 代码检查诊断 | ✅ | 解析诊断、可选的语义诊断，以及可配置的 lint/风格规则 |
| 跳转到定义 | ✅ | 模块、例化、端口、具名参数、符号和宏 |
| 查找引用 | ✅ | 跨已打开文件与已配置工程文件的符号和宏 |
| 重命名符号 | ✅ | 在工程文件范围内尽力而为 |
| 悬停提示 | ✅ | 模块、端口、信号、参数、typedef、子程序和宏的详细信息 |
| 自动补全 | ✅ | 感知上下文的自动补全 |
| 签名帮助 | ✅ | 函数与任务 |
| 内联提示 | ✅ | 例化时显示端口方向 |
| 工作区符号 | ✅ | 已索引设计文件中的模块与类 |
| RTL 层次树 | ✅ | 模块例化层次结构 |
| 自动例化 / 自动连线 / 自动端口列表 / 自动函数 / 自动寄存器 | ✅ | 用于生成 RTL 的多种代码操作 |
| 工程级配置 | ✅ | 通过工程根目录的 `lazyverilog.toml` 完全定制 |

&nbsp;

<a id="installation"></a>

## 📦 安装

<a id="neovim-installation"></a>
<details>
<summary><b>Neovim 安装</b></summary>

💤 使用 <a href="https://github.com/folke/lazy.nvim"><code>lazy.nvim</code></a>：

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

示例：

```lua
-- 按照自动安装 / PATH / 托管二进制的顺序查找服务器。
require("lazyverilog").setup()

-- 显式使用本地构建的二进制。
require("lazyverilog").setup({
  cmd = "/path/to/lazyverilog-lsp",
})
```

</details>

<a id="vscode-installation"></a>
<details>
<summary><b>VS Code 安装</b></summary>

#### 1. 应用市场（推荐）

从 [Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=lazyverilog.lazyverilog-vscode) 安装 LazyVerilog。

然后打开 `.sv`、`.svh`、`.v` 或 `.vh` 文件。扩展会自动为 Verilog/SystemVerilog 缓冲区启动 LazyVerilog，
并在需要时安装对应的 `lazyverilog-lsp` 发布版二进制。

如果你已经有本地构建的服务器二进制，可在 VS Code 设置中显式指定：

```json
{
  "lazyverilog.serverPath": "/path/to/lazyverilog-lsp"
}
```

#### 2. 从 GitHub Releases 手动安装

如果应用市场版本尚不可用，请从最新的 [GitHub Release](https://github.com/lazyverilog/LazyVerilog/releases/latest) 下载 VSIX。

然后在 VS Code 中安装：

1. 按 `Ctrl+Shift+P` 打开命令面板。
2. 执行 `Extensions: Install from VSIX...`。
3. 选择下载好的 `lazyverilog-<version>.vsix` 文件。

</details>

<a id="usage"></a>

## 🚀 使用

### 1. 在 RTL 工程根目录添加工程配置。

在工程根目录创建 `lazyverilog.toml`。至少要让 `design.vcode` 指向一个文件列表，
LazyVerilog 才能索引模块、包、端口以及跨文件引用。

完整配置请参考 [`lazyverilog.toml`](lazyverilog.toml) —— 完整的示例配置。

```toml
[design]
vcode = "path/to/vcode/file"
define = ["VERILATOR", "MY_DEFINE"]

[compilation]
background_compilation = true   # 在后台工作线程中执行语义编译（诊断更丰富）。
                                # 注意：在性能较弱的机器上可能会卡顿。

[format]
enable_format_on_save = true # 保存文件时自动格式化。
indent_size = 4

[lint]
enable = true # 显示代码检查诊断

[lint.naming]
enable = true
severity = "warning"
input_port_pattern = "^i_.*$"  # 正则；输入端口应以 i_ 开头
output_port_pattern = "^o_.*$" # 正则；输出端口应以 o_ 开头

[inlay_hint]
enable = true
```

`vcode.f` 示例：

```text
path/to/rtl1.sv
path/to/rtl2.sv
path/to/rtl3.sv
+incdir+path/to/your/include_dir1
+incdir+path/to/your/include_dir2
```

<details>
<summary><b>neovim 用户指南</b></summary>

#### 打开一个 SystemVerilog 工程

打开一个 Verilog/SystemVerilog RTL 文件：

服务器会向根目录（`/`）方向逐级向上查找 `lazyverilog.toml`。

```bash
nvim path/to/rtl.sv
```

首次启动时，Neovim 插件会下载 LazyVerilog 发布版。之后插件会自动为 `verilog` 和 `systemverilog` 缓冲区启动 LazyVerilog LSP。
使用 `:LspInfo` 确认 `lazyverilog` 客户端已经附加。

#### 使用标准 LSP 操作

LazyVerilog 提供常规的 Neovim LSP 功能。可以沿用你已有的 LSP 键位映射，或添加如下映射：

```lua
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "gr", vim.lsp.buf.references)
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename)
vim.keymap.set({ "n", "v" }, "<leader>ca", vim.lsp.buf.code_action)
```

当光标位于受支持的语法结构上时，代码操作会包含 AutoInst、AutoWire、AutoArg、AutoFunc 和 AutoFF 等 RTL 辅助功能。

#### 使用 LazyVerilog 命令

| 命令 | 说明 |
|---------|-------------|
| `:Format` | 格式化当前缓冲区或可视选区 |
| `:Lint` | 显示当前缓冲区的诊断 |
| `:LintAll` | 显示已索引工程文件的诊断 |
| `:RtlTree` | 打开模块例化层次结构 |
| `:RtlTreeReverse` | 从当前模块打开反向层次结构 |
| `:Interface <inst>` | 查看单个例化的接口 |
| `:Interface <inst1> <inst2>` | 查看并编辑两个例化之间的连接 |
| `:Connect <module1> <module2>` | 沿层次结构交互式连接模块例化 |

</details>

<details>
<summary><b>vscode 用户指南</b></summary>

安装扩展后，在 VS Code 中打开 Verilog/SystemVerilog RTL 文件。扩展会自动为 `verilog` 和
`systemverilog` 缓冲区启动 LazyVerilog。

可在命令面板中使用如下 LazyVerilog 命令：

- `LazyVerilog: Format Document`
- `LazyVerilog: Lint Current File`
- `LazyVerilog: Lint All Files`
- `LazyVerilog: Show RTL Hierarchy`
- `LazyVerilog: Show RTL Hierarchy (Reverse)`

安装方式与 `lazyverilog.serverPath` 的设置请参考上面的 [VS Code 安装说明](#vscode-installation)。

</details>

<a id="cli-tools"></a>

## 🧰 命令行工具

除了面向编辑器的 `lazyverilog-lsp` 服务器，LazyVerilog 还提供可用于脚本/CI 的独立命令行
二进制。它们与 LSP 服务器一样，从目标文件所在目录逐级向上查找并读取 `lazyverilog.toml`。

<details>
<summary><b><code>lazyverilog-fmt</code> —— 独立格式化工具</b></summary>

将单个文件格式化后输出到 stdout，或使用 `-i` 原地修改。

```bash
cmake --build build -j$(nproc) --target lazyverilog-fmt
./build/lazyverilog-fmt -i rtl/memory_top.sv
```

| 选项 | 说明 |
|------|-------------|
| `-i`, `--in-place` | 将格式化结果写回源文件，而不是输出到 stdout |
| `--log <log-dir>` | 将格式化器内部各 pass 的日志写入 `<log-dir>` 以便调试 |

完整参考：[`docs/formatter/cli.md`](docs/formatter/cli.md)。

</details>

<details>
<summary><b><code>lazyverilog-lint</code> —— 独立代码检查工具</b></summary>

检查单个文件，或 `-f` 文件列表中的全部文件，并以
`<file>:<line>:<col>: <severity>: <message>` 的格式打印 lint 诊断与编译诊断。

```bash
cmake --build build -j$(nproc) --target lazyverilog-lint
./build/lazyverilog-lint rtl/memory_top.sv
./build/lazyverilog-lint -f rtl/vcode.f
```

| 选项 | 说明 |
|------|-------------|
| `-f <filelist>` | 检查工程文件列表中的全部文件，替代（或附加于）`<file>` |
| `--lint-only` | 只打印 lint 规则诊断，去掉解析/语义诊断 |

完整参考：[`docs/linter/cli.md`](docs/linter/cli.md)。

</details>

<details>
<summary><b><code>lazyverilog-rtltree</code> —— 独立 RTL 层次结构查看器</b></summary>

以缩进树的形式打印以 `<file>` 中模块为根的模块例化层次结构 ——
默认是正向（子模块），使用 `--reverse` 则为反向（父模块）。

```bash
cmake --build build -j$(nproc) --target lazyverilog-rtltree
./build/lazyverilog-rtltree rtl/memory_top.sv
./build/lazyverilog-rtltree --reverse rtl/memory.sv
```

| 选项 | 说明 |
|------|-------------|
| `-f <filelist>` | 用于跨文件层次结构解析的工程文件列表 |
| `--reverse` | 构建反向层次结构而不是正向层次结构 |

完整参考：[`docs/rtl-tree/cli.md`](docs/rtl-tree/cli.md)。

</details>

<a id="build"></a>

## 🏗️ 构建

### ✅ 环境要求

- CMake
- 支持 C++20 的编译器

### 🧱 构建方法

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc) --target lazyverilog-lsp
```

### 服务器二进制的查找顺序

编辑器按以下顺序查找 `lazyverilog-lsp` 服务器：

1. 显式配置路径
   - VS Code：设置 `lazyverilog.serverPath`。
   - Neovim：向 `require("lazyverilog").setup({ ... })` 传入 `cmd`。
   - 对于不希望被 LazyVerilog 替换的本地构建或自定义二进制，请使用这种方式。
2. PATH 中的二进制
   - 如果 `PATH` 中存在 `lazyverilog-lsp`（Windows 上为 `lazyverilog-lsp.exe`），编辑器会将其作为用户自有的二进制使用。
   - LazyVerilog 不会对 PATH 中的二进制做校验和检查，也不会自动更新它们。
3. 托管二进制
   - 如果既没有显式配置路径也没有 PATH 中的二进制，编辑器会使用自己的托管存储目录，并可在该目录下载匹配的发布版二进制。
   - 托管二进制归发布流程所有：当插件或 VS Code 扩展更新时，过期的托管二进制可能会被当前经过校验的发布版二进制替换。
   - 不要把自定义构建放进托管二进制目录，请改用显式配置路径或 PATH。

<a id="configuration"></a>

## ⚙️ 配置

LazyVerilog 会读取工程根目录下的 `lazyverilog.toml`。如果 neovim 在子目录中打开，LazyVerilog 会逐级向上查找，直到找到最近的配置文件。

该配置控制设计输入、语义编译、lint 规则、格式化策略、RTL 层次树显示、内联提示以及自动化辅助功能。

<details open>
<summary>📝 最小示例</summary>

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

## 📚 文档

- [`lazyverilog.toml`](lazyverilog.toml) —— 完整的示例配置。
- [`docs/features.md`](docs/features.md) —— 所有功能一览。
- [`docs/releases/v1.1.0.md`](docs/releases/v1.1.0.md) —— 最新发布说明。

**工程**
- [`docs/design/index.md`](docs/design/index.md) —— 设计文件列表与预处理器宏定义。

**LSP 与编辑器**
- [`docs/lsp/index.md`](docs/lsp/index.md) —— 悬停提示、跳转到定义、查找引用、重命名、自动补全、签名帮助、内联提示、工作区符号。

**格式化器**
- [`docs/formatter/cli.md`](docs/formatter/cli.md) —— CLI 用法与构建说明。
- [`docs/formatter/options.md`](docs/formatter/options.md) —— 格式化器选项。
- [`docs/formatter/macros.md`](docs/formatter/macros.md) —— 宏格式化策略。

**代码检查器**
- [`docs/linter/cli.md`](docs/linter/cli.md) —— `lazyverilog-lint` CLI 用法与构建说明。
- [`docs/linter/options.md`](docs/linter/options.md) —— 带 RTL 示例的检查器选项。

**语义诊断**
- [`docs/diagnostics/background-compilation.md`](docs/diagnostics/background-compilation.md) —— 后台语义诊断。

**自动化功能**
- [`docs/autoarg/index.md`](docs/autoarg/index.md) —— 生成非 ANSI 风格的模块端口列表。
- [`docs/autoinst/index.md`](docs/autoinst/index.md) —— 生成模块例化的端口连接。
- [`docs/autowire/index.md`](docs/autowire/index.md) —— 生成缺失的信号声明。
- [`docs/autofunc/index.md`](docs/autofunc/index.md) —— 生成函数/任务调用的参数。
- [`docs/autoff/index.md`](docs/autoff/index.md) —— 在已有的 always_ff 块中插入复位/采样赋值。
- [`docs/connect.md`](docs/connect.md) —— 交互式连接模块例化的输出端口到输入端口。
- [`docs/interface.md`](docs/interface.md) —— 查看并编辑例化之间的信号接口。
- [`docs/rtl-tree/index.md`](docs/rtl-tree/index.md) —— 模块例化层次结构查看器。
- [`docs/rtl-tree/cli.md`](docs/rtl-tree/cli.md) —— `lazyverilog-rtltree` CLI 用法与构建说明。

**面向开发者**
- [`docs/dev/test.md`](docs/dev/test.md) —— 构建、测试与 RTL 格式化全量验证。
- [`docs/dev/files.md`](docs/dev/files.md) —— 设计文件列表缓存与额外文件的 mtime 行为。
- [`TODO.md`](TODO.md) —— 计划中的功能与已知问题。

&nbsp;

## 🤝 参与贡献

欢迎贡献。

在提交 Pull Request 之前：

1. 构建项目。
2. 运行相关测试。
3. 为格式化器、lint、LSP 或自动化相关的改动添加或更新测试。
4. 当用户可见的行为或配置发生变化时更新文档。

建议执行的检查：

```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

> [!IMPORTANT]
> 格式化器的改动应在 `tests/test_formatter.cpp` 中包含有针对性的用例，并保持幂等性与安全模式的保证。

&nbsp;

## 📜 许可证

LazyVerilog 基于 MIT 许可证发布。详见 [`LICENSE`](LICENSE)。
