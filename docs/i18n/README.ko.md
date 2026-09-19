> 이 문서는 Claude가 생성한 자동 번역본이며, 정식 기준은 영문 원본 [README.md](../../README.md)입니다.

<p align="center">
  <img src="../../assets/lazyverilog_logo.png" alt="LazyVerilog logo" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>RTL 코딩을 위한 빠르고 실용적인 SystemVerilog LSP.</b>
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
  <b>한국어</b>
  ·
  <a href="../../docs/i18n/README.zh-CN.md">简体中文</a>
  ·
  <a href="../../docs/i18n/README.ja.md">日本語</a>
  ·
  <a href="../../docs/i18n/README.de.md">Deutsch</a>
  ·
  <a href="../../docs/i18n/README.es.md">Español</a>
  ·
  <a href="../../docs/i18n/README.fr.md">Français</a>
  ·
  <a href="../../docs/i18n/README.pt.md">Português</a>
</p>

<p align="center">
  <a href="#-데모">데모</a>
  ·
  <a href="#-왜-lazyverilog인가">도입 이유</a>
  ·
  <a href="#-기능">기능</a>
  ·
  <a href="#️-비교">비교</a>
  ·
  <a href="#-설치">설치</a>
  ·
  <a href="#-사용법">사용법</a>
  ·
  <a href="#-cli-도구">CLI 도구</a>
  ·
  <a href="#️-설정">설정</a>
  ·
  <a href="#️-빌드">빌드</a>
</p>


<p align="center">
  LazyVerilog는 C++로 작성된 SystemVerilog LSP이며 neovim과 vscode를 지원합니다.
  실제 SystemVerilog 프로젝트를 대상으로 포매팅, 린팅, 코드 탐색, hover, 자동 완성, inlay hint, RTL code action을 제공합니다.
</p>

&nbsp;

## 🎬 데모

<details>
<summary><b>🎨 포매팅</b></summary>

![Formatting](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Lint 진단</b></summary>

![Lint diagnostics](../../assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ 자동 완성</b></summary>

![Auto-complete](../../assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 RTL 트리</b></summary>

![RTL tree](../../assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 코드 접기</b></summary>

![Folding](../../assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 정의로 이동</b></summary>

![Go to definition](../../assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 인터페이스 연결</b></summary>

![Interface connect](../../assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 자동 인스턴스화</b></summary>

![Auto-instantiation](../../assets/videos/AutoInst.gif)

</details>

<details>
<summary><b>🔌 Auto-wire</b></summary>

![Auto-wire](../../assets/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ Auto-arg</b></summary>

![Auto-arg](../../assets/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 Hover</b></summary>

![Hover](../../assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 Inlay hint</b></summary>

![Inlay hints](../../assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 참조 찾기</b></summary>

![Find references](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ 이름 변경</b></summary>

![Rename](../../assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 워크스페이스 심볼</b></summary>

![Workspace symbols](../../assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 시그니처 도움말</b></summary>

![Signature help](../../assets/videos/sig_help.gif)

</details>

&nbsp;

## ✨ 왜 LazyVerilog인가?

<table>
  <tr>
    <td>🎯</td>
    <td><b>정확한 파싱</b></td>
    <td>SystemVerilog 문법 파싱은 <a href="https://github.com/MikePopoloski/slang">slang</a>이 담당합니다.</td>
  </tr>
  <tr>
    <td>🧠</td>
    <td><b>풍부한 LSP 기능</b></td>
    <td>Inlay hint, 참조 찾기, 정의로 이동, hover, 이름 변경, 자동 완성, 시그니처 도움말, lint 진단.</td>
  </tr>
  <tr>
    <td>⚙️</td>
    <td><b>RTL 자동화</b></td>
    <td>Auto-arg, auto-function, auto-wire, auto-FF, 자동 인스턴스화.</td>
  </tr>
  <tr>
    <td>🧰</td>
    <td><b>커스터마이즈 가능</b></td>
    <td><code>lazyverilog.toml</code>로 프로젝트별 동작을 지정합니다.</td>
  </tr>
</table>

&nbsp;

## ⚖️ 비교

LazyVerilog와 가장 널리 쓰이는 두 오픈소스 SystemVerilog LSP인
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
및 [`svlangserver`](https://github.com/imc-trading/svlangserver)의 비교입니다.

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| **UVM / class / package 지원** | ✅ | ⚠️ | ❌ 공식 문서: "대부분의 검증 전용 개념(예: class)을 이해하지 못함" |
| **성능** | ✅ 네이티브 C++ | ✅ 네이티브 C++ | ❌ Node.js 런타임 |
| 파서 | [slang](https://github.com/MikePopoloski/slang) | Verible 자체 SystemVerilog 파서 | 자체 경량 인덱서 |
| 진단 | ✅ 파싱 + 커스터마이즈 가능한 lint 규칙 + 선택적 시맨틱 진단 | 구문 오류 + Verible의 lint 규칙 세트 | Verilator에 위임 |
| 포매팅 | 내장 | 내장 | `verible-verilog-format`에 위임 |
| 정의로 이동 | ✅ | ✅ | ✅ |
| 참조 찾기 | ✅ | ✅ | ❌ |
| 심볼 이름 변경 | ✅ | ❌ (계획됨으로 표기) | ❌ |
| Hover | ✅ | ⚠️ (실험적) | ✅ |
| 자동 완성 | ✅ | ❌ | ✅ |
| 시그니처 도움말 | ✅ | ❌ | ✅ |
| Inlay hint | ✅ 인스턴스의 포트 방향 | ❌ | ❌ |
| 문서 / 워크스페이스 심볼 | ✅ | ✅ 문서 아웃라인 | ✅ |
| Lint 자동 수정 code action | ❌ | ✅ | ❌ |
| RTL 코드 생성 | ✅ AutoInst / AutoWire / AutoArg / AutoFunc / AutoFF | ❌ | ❌ |
| RTL 계층 뷰 | ✅ | ❌ | ✅  |
| 인터페이스 / 인스턴스 연결 도구 | ✅ `:Interface`, `:Connect` | ❌ | ❌ |

&nbsp;


## 📊 기능

LazyVerilog가 현재 지원하는 기능:

| 기능 | 상태 | 비고 |
|---------|--------|-------|
| 포매팅 | ✅ | `lazyverilog.toml`로 설정 가능 |
| Lint 진단 | ✅ | 파싱 진단, 선택적 시맨틱 진단, 설정 가능한 lint/스타일 규칙 |
| 정의로 이동 | ✅ | 모듈, 인스턴스, 포트, 이름 지정 인자, 심볼, 매크로 |
| 참조 찾기 | ✅ | 열린 파일과 설정된 프로젝트 파일 전반의 심볼 및 매크로 |
| 심볼 이름 변경 | ✅ | 프로젝트 파일 전반에 걸쳐 최선 노력 방식으로 수행 |
| Hover | ✅ | 모듈, 포트, 신호, 파라미터, typedef, 서브루틴, 매크로의 심볼 정보 |
| 자동 완성 | ✅ | 문맥을 인식하는 자동 완성 |
| 시그니처 도움말 | ✅ | 함수와 태스크 |
| Inlay hint | ✅ | 인스턴스화 시 포트 방향 표시 |
| 워크스페이스 심볼 | ✅ | 인덱싱된 설계 파일의 모듈과 class |
| RTL 트리 | ✅ | 모듈 인스턴스화 계층 |
| 자동 인스턴스화 / Auto-wire / Auto-arg / Auto-function / Auto-FF | ✅ | RTL 생성을 위한 다양한 Code Action |
| 프로젝트별 설정 | ✅ | 프로젝트 루트의 `lazyverilog.toml`로 전체 커스터마이즈 가능 |

&nbsp;

## 📦 설치

<a id="neovim-installation"></a>
<details>
<summary><b>Neovim 설치</b></summary>

💤 <a href="https://github.com/folke/lazy.nvim"><code>lazy.nvim</code></a> 사용:

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

예시:

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

</details>

<a id="vscode-installation"></a>
<details>
<summary><b>VS Code 설치</b></summary>

#### 1. 마켓플레이스 (권장)

[Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=lazyverilog.lazyverilog-vscode)에서 LazyVerilog를 설치하세요.

그 다음 `.sv`, `.svh`, `.v`, `.vh` 파일을 엽니다. 확장은 Verilog/SystemVerilog 버퍼에 대해 LazyVerilog를
자동으로 시작하며, 필요할 경우 해당하는 `lazyverilog-lsp` 릴리스 바이너리를 설치합니다.

로컬에서 빌드한 서버 바이너리가 이미 있다면 VS Code 설정에서 명시적으로 지정하세요:

```json
{
  "lazyverilog.serverPath": "/path/to/lazyverilog-lsp"
}
```

#### 2. GitHub Releases에서 수동 설치

마켓플레이스 버전이 아직 제공되지 않는다면, 최신 [GitHub Release](https://github.com/lazyverilog/LazyVerilog/releases/latest)에서 VSIX를 내려받으세요.

그런 다음 VS Code에서 설치합니다:

1. `Ctrl+Shift+P`를 눌러 명령 팔레트를 엽니다.
2. `Extensions: Install from VSIX...`를 실행합니다.
3. 내려받은 `lazyverilog-<version>.vsix` 파일을 선택합니다.

</details>

## 🚀 사용법

### 1. RTL 프로젝트 루트에 프로젝트 설정 파일을 추가합니다.

프로젝트 루트에 `lazyverilog.toml`을 만듭니다. 최소한 `design.vcode`가 filelist를 가리키도록 설정해야
LazyVerilog가 모듈, 패키지, 포트, 파일 간 참조를 인덱싱할 수 있습니다.

**RTL 파일보다 상위에 있는 어느 디렉터리에 두어도 됩니다.**

전체 설정은 [`lazyverilog.toml`](../../lazyverilog.toml)을 참고하세요 — 완전한 설정 예시입니다.

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

`vcode.f` 예시:

```text
path/to/rtl1.sv
path/to/rtl2.sv
path/to/rtl3.sv
+incdir+path/to/your/include_dir1
+incdir+path/to/your/include_dir2
```

<details>
<summary><b>Neovim 사용자 가이드</b></summary>

#### SystemVerilog 프로젝트 열기

Verilog/SystemVerilog RTL 파일을 엽니다:

서버는 열린 파일의 위치에서 상위 디렉터리로 거슬러 올라가며 `lazyverilog.toml`을 찾습니다 — 1단계를 참고하세요.

```bash
nvim path/to/rtl.sv
```

첫 실행이라면 Neovim 플러그인이 LazyVerilog 릴리스를 내려받습니다. 이후 Neovim 플러그인은 `verilog` 및 `systemverilog` 버퍼에 대해 LazyVerilog LSP를 자동으로 시작합니다.
`:LspInfo`로 `lazyverilog` 클라이언트가 연결되었는지 확인하세요.

#### 표준 LSP 액션 사용하기

LazyVerilog는 일반적인 Neovim LSP 기능을 제공합니다. 기존 LSP 키맵을 그대로 쓰거나, 아래처럼
매핑을 추가하세요:

```lua
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "gr", vim.lsp.buf.references)
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename)
vim.keymap.set({ "n", "v" }, "<leader>ca", vim.lsp.buf.code_action)
```

커서가 지원되는 구문 위에 있을 때 code action에는 AutoInst, AutoWire, AutoArg, AutoFunc, AutoFF 같은
RTL 헬퍼가 포함됩니다.

#### LazyVerilog 명령어 사용하기

| 명령어 | 설명 |
|---------|-------------|
| `:Format` | 현재 버퍼 또는 비주얼 선택 범위를 포매팅 |
| `:Lint` | 현재 버퍼의 진단 표시 |
| `:LintAll` | 인덱싱된 프로젝트 파일의 진단 표시 |
| `:RtlTree` | 모듈 인스턴스화 계층 열기 |
| `:RtlTreeReverse` | 현재 모듈 기준 역방향 계층 열기 |
| `:Interface <inst>` | 인스턴스 하나의 인터페이스 확인 |
| `:Interface <inst1> <inst2>` | 두 인스턴스 사이의 연결 확인 및 편집 |
| `:Connect <module1> <module2>` | 계층을 따라 모듈 인스턴스를 대화식으로 연결 |

</details>

<details>
<summary><b>VS Code 사용자 가이드</b></summary>

확장을 설치한 뒤 VS Code에서 Verilog/SystemVerilog RTL 파일을 엽니다. 확장은 `verilog` 및
`systemverilog` 버퍼에 대해 LazyVerilog를 자동으로 시작합니다.

명령 팔레트에서 다음과 같은 LazyVerilog 명령을 사용할 수 있습니다:

- `LazyVerilog: Format Document`
- `LazyVerilog: Lint Current File`
- `LazyVerilog: Lint All Files`
- `LazyVerilog: Show RTL Hierarchy`
- `LazyVerilog: Show RTL Hierarchy (Reverse)`

설치 방법과 `lazyverilog.serverPath` 설정은 위의 [VS Code 설치 안내](#vscode-installation)를 참고하세요.

</details>

## 🧰 CLI 도구

LazyVerilog는 에디터용 서버인 `lazyverilog-lsp` 외에도 스크립트/CI에서 쓸 수 있는 독립 실행형
커맨드라인 바이너리를 함께 제공합니다. 각 도구는 대상 파일의 디렉터리에서 상위로 거슬러 올라가며
LSP 서버와 동일한 방식으로 `lazyverilog.toml`을 읽습니다.

<details>
<summary><b><code>lazyverilog-fmt</code> — 독립 실행형 formatter</b></summary>

파일 하나를 포매팅해 stdout으로 출력하거나, `-i`로 파일을 직접 수정합니다.

```bash
cmake --build build -j$(nproc) --target lazyverilog-fmt
./build/lazyverilog-fmt -i rtl/memory_top.sv
```

| 플래그 | 설명 |
|------|-------------|
| `-i`, `--in-place` | 포매팅 결과를 stdout 대신 원본 파일에 기록 |
| `--log <log-dir>` | 디버깅용으로 formatter 내부 pass 로그를 `<log-dir>`에 기록 |

전체 레퍼런스: [`docs/formatter/cli.md`](../../docs/formatter/cli.md).

</details>

<details>
<summary><b><code>lazyverilog-lint</code> — 독립 실행형 linter</b></summary>

파일 하나 또는 `-f` filelist에 있는 모든 파일을 lint하고, lint 진단과 컴파일 진단을
보기 좋게 출력합니다 (`<file>:<line>:<col>: <severity>: <message>`).

```bash
cmake --build build -j$(nproc) --target lazyverilog-lint
./build/lazyverilog-lint rtl/memory_top.sv
./build/lazyverilog-lint -f rtl/vcode.f
```

| 플래그 | 설명 |
|------|-------------|
| `-f <filelist>` | `<file>` 대신(또는 추가로) 프로젝트 filelist의 모든 파일을 lint |
| `--lint-only` | lint 규칙 진단만 출력하고 파싱/시맨틱 진단은 제외 |

전체 레퍼런스: [`docs/linter/cli.md`](../../docs/linter/cli.md).

</details>

<details>
<summary><b><code>lazyverilog-rtltree</code> — 독립 실행형 RTL 계층 뷰어</b></summary>

`<file>`의 모듈을 루트로 하는 모듈 인스턴스화 계층을 들여쓰기된 트리로 출력합니다 —
기본은 정방향(하위 모듈), `--reverse`를 쓰면 역방향(상위 모듈)입니다.

```bash
cmake --build build -j$(nproc) --target lazyverilog-rtltree
./build/lazyverilog-rtltree rtl/memory_top.sv
./build/lazyverilog-rtltree --reverse rtl/memory.sv
```

| 플래그 | 설명 |
|------|-------------|
| `-f <filelist>` | 파일 간 계층 해석을 위한 프로젝트 filelist |
| `--reverse` | 정방향 대신 역방향 계층을 구성 |

전체 레퍼런스: [`docs/rtl-tree/cli.md`](../../docs/rtl-tree/cli.md).

</details>

## 🏗️ 빌드

### ✅ 요구 사항

- CMake
- C++20을 지원하는 컴파일러

### 🧱 빌드 방법

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc) --target lazyverilog-lsp
```

### 서버 바이너리 탐색 순서

에디터는 다음 순서로 `lazyverilog-lsp` 서버를 찾습니다:

1. 명시적 설정 경로
   - VS Code: `lazyverilog.serverPath`를 설정합니다.
   - Neovim: `require("lazyverilog").setup({ ... })`에 `cmd`를 전달합니다.
   - LazyVerilog가 절대 교체해서는 안 되는 로컬 빌드나 커스텀 바이너리에 이 방법을 사용하세요.
2. PATH의 바이너리
   - `lazyverilog-lsp`가 `PATH`에 있으면(Windows에서는 `lazyverilog-lsp.exe`), 에디터는 이를 사용자 소유 바이너리로 사용합니다.
   - PATH 바이너리는 LazyVerilog가 체크섬을 검사하거나 자동 업데이트하지 않습니다.
3. 관리형 바이너리
   - 명시적 설정 경로나 PATH 바이너리가 없으면, 에디터는 자체 관리 저장소 디렉터리를 사용하고 그곳에 해당하는 릴리스 바이너리를 내려받을 수 있습니다.
   - 관리형 바이너리는 릴리스가 소유합니다. 플러그인이나 VS Code 확장이 업데이트되면 오래된 관리형 바이너리는 검증된 최신 릴리스 바이너리로 교체될 수 있습니다.
   - 관리형 바이너리 디렉터리에 커스텀 빌드를 두지 마세요. 대신 명시적 설정 경로나 PATH를 사용하세요.

## ⚙️ 설정

LazyVerilog는 파일을 열 때마다 상위 디렉터리로 거슬러 올라가며 가장 가까운 `lazyverilog.toml`을 찾습니다 — [사용법 1단계](#1-rtl-프로젝트-루트에-프로젝트-설정-파일을-추가합니다)를 참고하세요. 하위 디렉터리를 열어도 그 상위의 설정이 가려지지 않습니다.

이 설정은 설계 입력, 시맨틱 컴파일, lint 규칙, formatter 정책, RTL 트리 표시, inlay hint, 자동화 헬퍼를 제어합니다.

<details open>
<summary>📝 최소 예시</summary>

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

## 📚 문서

- [`lazyverilog.toml`](../../lazyverilog.toml) — 완전한 설정 예시.
- [`docs/features.md`](../../docs/features.md) — 전체 기능 한눈에 보기.
- [`docs/releases/v1.1.0.md`](../../docs/releases/v1.1.0.md) — 최신 릴리스 노트.

**프로젝트**
- [`docs/design/index.md`](../../docs/design/index.md) — 설계 filelist와 전처리기 define.

**LSP & 에디터**
- [`docs/lsp/index.md`](../../docs/lsp/index.md) — hover, 정의로 이동, 참조 찾기, 이름 변경, 자동 완성, 시그니처 도움말, inlay hint, 워크스페이스 심볼.

**Formatter**
- [`docs/formatter/cli.md`](../../docs/formatter/cli.md) — CLI 사용법과 빌드 방법.
- [`docs/formatter/options.md`](../../docs/formatter/options.md) — formatter 옵션.
- [`docs/formatter/macros.md`](../../docs/formatter/macros.md) — 매크로 포매팅 정책.

**Linter**
- [`docs/linter/cli.md`](../../docs/linter/cli.md) — `lazyverilog-lint` CLI 사용법과 빌드 방법.
- [`docs/linter/options.md`](../../docs/linter/options.md) — RTL 예시와 함께 보는 linter 옵션.

**시맨틱 진단**
- [`docs/diagnostics/background-compilation.md`](../../docs/diagnostics/background-compilation.md) — 백그라운드 시맨틱 진단.

**자동화 기능**
- [`docs/autoarg/index.md`](../../docs/autoarg/index.md) — non-ANSI 모듈 포트 목록 생성.
- [`docs/autoinst/index.md`](../../docs/autoinst/index.md) — 모듈 인스턴스화 포트 연결 생성.
- [`docs/autowire/index.md`](../../docs/autowire/index.md) — 누락된 신호 선언 생성.
- [`docs/autofunc/index.md`](../../docs/autofunc/index.md) — function/task 호출 인자 생성.
- [`docs/autoff/index.md`](../../docs/autoff/index.md) — 기존 always_ff 블록에 리셋/캡처 할당문 삽입.
- [`docs/connect.md`](../../docs/connect.md) — 모듈 인스턴스의 출력-입력 포트를 대화식으로 연결.
- [`docs/interface.md`](../../docs/interface.md) — 인스턴스 간 신호 인터페이스 확인 및 편집.
- [`docs/rtl-tree/index.md`](../../docs/rtl-tree/index.md) — 모듈 인스턴스화 계층 뷰어.
- [`docs/rtl-tree/cli.md`](../../docs/rtl-tree/cli.md) — `lazyverilog-rtltree` CLI 사용법과 빌드 방법.

**개발자용**
- [`docs/dev/test.md`](../../docs/dev/test.md) — 빌드, 테스트, RTL 포맷 스윕.
- [`docs/dev/files.md`](../../docs/dev/files.md) — 설계 filelist 캐시와 추가 파일 mtime 동작.
- [`TODO.md`](../../TODO.md) — 계획된 기능과 알려진 이슈.

&nbsp;

## 🤝 기여하기

기여를 환영합니다.

풀 리퀘스트를 보내기 전에:

1. 프로젝트를 빌드합니다.
2. 관련 테스트를 실행합니다.
3. formatter, lint, LSP, 자동화 관련 변경에는 테스트를 추가하거나 갱신합니다.
4. 사용자에게 보이는 동작이나 설정이 바뀌면 문서를 갱신합니다.

권장 검증 절차:

```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

> [!IMPORTANT]
> formatter 변경에는 `tests/test_formatter.cpp`에 해당 동작을 겨냥한 테스트 케이스를 포함해야 하며, 멱등성과 safe-mode 보장을 유지해야 합니다.

&nbsp;

## 📜 라이선스

LazyVerilog는 MIT 라이선스로 배포됩니다. [`LICENSE`](../../LICENSE)를 참고하세요.
