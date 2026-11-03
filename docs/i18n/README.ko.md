> 이 문서는 Claude가 생성한 자동 번역본이며, 기준이 되는 원문은 영어 버전인 [README.md](../../README.md)입니다.

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
  <a href="#-비교">비교</a>
  ·
  <a href="#-기능">기능</a>
  ·
  <a href="#-시작하기">시작하기</a>
</p>


<p align="center">
  LazyVerilog는 C++로 작성된 SystemVerilog LSP로, Neovim과 VS Code를 지원합니다.
  실제 SystemVerilog 프로젝트를 위한 포매팅, 린팅, 코드 탐색, 호버, 자동 완성, 인레이 힌트, RTL 코드 액션을 제공합니다.
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="Auto-instantiation: generate a module instance with its ports connected" width="720">
</p>

<p align="center">
  <sub>LazyVerilog가 시간을 아껴주었다면, ⭐ 하나가 다른 RTL 엔지니어들이 이 프로젝트를 발견하는 데 도움이 됩니다.</sub>
</p>

&nbsp;

## 🎬 데모

<details>
<summary><b>🎨 포매팅</b></summary>

![Formatting](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 린트 진단</b></summary>

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
<summary><b>📂 코드 폴딩</b></summary>

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
<summary><b>🔌 자동 와이어 생성</b></summary>

![Auto-wire](../../assets/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ 자동 포트 목록 생성</b></summary>

![Auto-arg](../../assets/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 호버</b></summary>

![Hover](../../assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 인레이 힌트</b></summary>

![Inlay hints](../../assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 참조 찾기</b></summary>

![Find references](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ 이름 바꾸기</b></summary>

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
| UVM, 클래스, 패키지 | ✅ | ⚠️ | ❌ |
| 런타임 | C++ | C++ | Node.js |
| 파서 | slang | Verible | 자체 인덱서 |
| 진단 | 파싱, 린트, 시맨틱 | 파싱, 린트 | Verilator |
| 포매팅 | ✅ | ✅ | Verible 사용 |
| 정의로 이동 | ✅ | ✅ | ✅ |
| 참조 찾기 | ✅ | ✅ | ❌ |
| 이름 바꾸기 | ✅ | ❌ | ❌ |
| 호버 | ✅ | ⚠️ | ✅ |
| 자동 완성 | ✅ | ❌ | ✅ |
| 시그니처 도움말 | ✅ | ❌ | ✅ |
| 인레이 힌트 | ✅ | ❌ | ❌ |
| 심볼 | ✅ | ✅ | ✅ |
| 린트 자동 수정 | ✅ (`--fix`) | ✅ | ❌ |
| RTL 코드 생성 | ✅ | ❌ | ❌ |
| RTL 계층 구조 보기 | ✅ | ❌ | ✅ |
| 인터페이스 및 연결 | ✅ | ❌ | ❌ |

⚠️ 는 부분 지원 또는 실험적 지원을 의미합니다.

&nbsp;

## 📊 기능

현재 LazyVerilog가 지원하는 기능:

| 기능 | 상태 | 비고 |
|---------|--------|-------|
| 포매팅 | ✅ | `lazyverilog.toml`로 설정 가능 |
| 린트 진단 | ✅ | 파싱 진단, 선택적 시맨틱 진단, 설정 가능한 린트/스타일 규칙 |
| 린트 퀵 픽스 | ⚠️ | 누락된 `default` 추가, `automatic` 라이프타임 추가 |
| 정의로 이동 | ✅ | 모듈, 인스턴스, 포트, 네임드 인자, 심볼, 매크로 |
| 참조 찾기 | ✅ | 열려 있는 파일과 설정된 프로젝트 파일 전체의 심볼 및 매크로 |
| 심볼 이름 바꾸기 | ✅ | 프로젝트 파일 전체에 대해 최선 노력(best-effort) 방식으로 수행 |
| 호버 | ✅ | 모듈, 포트, 신호, 파라미터, typedef, 서브루틴, 매크로의 심볼 상세 정보 |
| 자동 완성 | ✅ | 컨텍스트를 인식하는 자동 완성 |
| 시그니처 도움말 | ✅ | function 및 task |
| 인레이 힌트 | ✅ | 인스턴스화 시 포트 방향 표시 |
| 문서 심볼 | ✅ | 현재 파일의 아웃라인 |
| 워크스페이스 심볼 | ✅ | 모듈, 인터페이스, 패키지, 클래스 및 메서드, function, task, typedef, `pkg::name` 형식 쿼리 |
| RTL 트리 | ✅ | 모듈 인스턴스화 계층 구조 |
| 자동 인스턴스화 / 자동 와이어 / 자동 포트 목록 / 자동 function / 자동 FF | ✅ | RTL 생성을 위한 다양한 코드 액션 |
| 프로젝트 로컬 설정 | ✅ | 프로젝트 루트의 `lazyverilog.toml`을 통해 전체 커스터마이징 가능 |

&nbsp;

## 🚀 시작하기

Neovim 및 VS Code 설치 방법, 사용법, 설정: **https://lazyverilog.github.io**
