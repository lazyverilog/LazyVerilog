> 이 문서는 영문 [README.md](../../README.md)의 번역본입니다. 내용이 다르면 영문이 기준입니다.

<p align="center">
  <img src="../../docs/public/logo.webp" alt="LazyVerilog logo" width="260">
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
</p>

<p align="center">
  <a href="#-데모">데모</a>
  ·
  <a href="#️-비교">비교</a>
  ·
  <a href="#-기능">기능</a>
  ·
  <a href="#-시작하기">시작하기</a>
</p>


<p align="center">
  LazyVerilog는 C++로 작성된 SystemVerilog LSP이며 neovim과 vscode를 지원합니다.
  실제 SystemVerilog 프로젝트를 대상으로 포매팅, 린팅, 코드 탐색, hover, 자동 완성, inlay hint, RTL code action을 제공합니다.
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="자동 인스턴스화: 포트가 연결된 모듈 인스턴스 생성" width="720">
</p>

<p align="center">
  <sub>LazyVerilog가 시간을 아껴 주었다면 ⭐ 하나가 다른 RTL 엔지니어들이 찾는 데 도움이 됩니다.</sub>
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

## ⚖️ 비교

LazyVerilog와 가장 널리 쓰이는 두 오픈소스 SystemVerilog LSP인
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
및 [`svlangserver`](https://github.com/imc-trading/svlangserver)의 비교입니다.

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM, class, package | ✅ | ⚠️ | ❌ |
| 런타임 | C++ | C++ | Node.js |
| 파서 | slang | Verible | 자체 인덱서 |
| 진단 | 파싱, lint, 시맨틱 | 파싱, lint | Verilator |
| 포매팅 | ✅ | ✅ | Verible 사용 |
| 정의로 이동 | ✅ | ✅ | ✅ |
| 참조 찾기 | ✅ | ✅ | ❌ |
| 이름 변경 | ✅ | ❌ | ❌ |
| Hover | ✅ | ⚠️ | ✅ |
| 자동 완성 | ✅ | ❌ | ✅ |
| 시그니처 도움말 | ✅ | ❌ | ✅ |
| Inlay hint | ✅ | ❌ | ❌ |
| 심볼 | ✅ | ✅ | ✅ |
| Lint 자동 수정 | ⚠️ | ✅ | ❌ |
| RTL 코드 생성 | ✅ | ❌ | ❌ |
| RTL 계층 뷰 | ✅ | ❌ | ✅ |
| 인터페이스 / 연결 | ✅ | ❌ | ❌ |

⚠️는 일부 지원 또는 실험적 기능입니다.

&nbsp;

## 📊 기능

LazyVerilog가 현재 지원하는 기능:

| 기능 | 상태 | 비고 |
|---------|--------|-------|
| 포매팅 | ✅ | `lazyverilog.toml`로 설정 |
| Lint 진단 | ✅ | 파싱 진단, 선택적 시맨틱 진단, 설정 가능한 lint/스타일 규칙 |
| Lint quick fix | ⚠️ | 빠진 `default` 추가, `automatic` lifetime 추가 |
| 정의로 이동 | ✅ | 모듈, 인스턴스, 포트, 이름 지정 인자, 심볼, 매크로 |
| 참조 찾기 | ✅ | 열린 파일과 프로젝트 파일 전체의 심볼과 매크로 |
| 심볼 이름 변경 | ✅ | 프로젝트 파일 전체에 걸쳐 최선 노력 방식 |
| Hover | ✅ | 모듈, 포트, 신호, 파라미터, typedef, 서브루틴, 매크로의 정보 |
| 자동 완성 | ✅ | 문맥을 인식하는 자동 완성 |
| 시그니처 도움말 | ✅ | 함수와 태스크 |
| Inlay hint | ✅ | 인스턴스의 포트 방향 |
| 문서 심볼 | ✅ | 현재 파일의 아웃라인 |
| 워크스페이스 심볼 | ✅ | 모듈, 인터페이스, 패키지, class와 method, 함수, 태스크, typedef, `pkg::name` 검색 |
| RTL 트리 | ✅ | 모듈 인스턴스 계층 |
| 자동 인스턴스화 / Auto-wire / Auto-arg / Auto-function / Auto-FF | ✅ | RTL 생성을 위한 여러 Code Action |
| 프로젝트별 설정 | ✅ | 프로젝트의 `lazyverilog.toml`로 설정 |

&nbsp;

## 🚀 시작하기

Neovim과 VS Code의 설치, 사용법, 설정: **https://lazyverilog.github.io** (영문)
