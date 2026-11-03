> Hinweis: Dies ist eine automatisierte Übersetzung, erstellt von Claude. Maßgeblich ist das englische Original: [README.md](../../README.md).

<p align="center">
  <img src="../../docs/public/logo.webp" alt="LazyVerilog logo" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>Ein schneller, praxisnaher SystemVerilog-LSP für die RTL-Entwicklung.</b>
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
  <a href="../../docs/i18n/README.ko.md">한국어</a>
</p>

<p align="center">
  <a href="#-demo">Demo</a>
  ·
  <a href="#-vergleich">Vergleich</a>
  ·
  <a href="#-funktionen">Funktionen</a>
  ·
  <a href="#-erste-schritte">Erste Schritte</a>
</p>


<p align="center">
  LazyVerilog ist ein in C++ geschriebener SystemVerilog-LSP mit Unterstützung für Neovim und VS Code.
  Er bietet Formatierung, Linting, Navigation, Hover, Vervollständigung, Inlay-Hints und RTL-Code-Actions für echte SystemVerilog-Projekte.
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="Auto-instantiation: generate a module instance with its ports connected" width="720">
</p>

<p align="center">
  <sub>Wenn LazyVerilog dir Zeit spart, hilft ein ⭐ anderen RTL-Entwicklern, es zu finden.</sub>
</p>

&nbsp;

## 🎬 Demo

<details>
<summary><b>🎨 Formatierung</b></summary>

![Formatting](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Lint-Diagnosen</b></summary>

![Lint diagnostics](../../assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ Auto-Vervollständigung</b></summary>

![Auto-complete](../../assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 RTL-Baum</b></summary>

![RTL tree](../../assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 Code-Faltung</b></summary>

![Folding](../../assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 Gehe zu Definition</b></summary>

![Go to definition](../../assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 Interface-Verbindung</b></summary>

![Interface connect](../../assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 Auto-Instanziierung</b></summary>

![Auto-instantiation](../../assets/videos/AutoInst.gif)

</details>

<details>
<summary><b>🔌 Auto-Wire</b></summary>

![Auto-wire](../../assets/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ Auto-Arg</b></summary>

![Auto-arg](../../assets/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 Hover</b></summary>

![Hover](../../assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 Inlay-Hints</b></summary>

![Inlay hints](../../assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 Referenzen suchen</b></summary>

![Find references](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ Umbenennen</b></summary>

![Rename](../../assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 Workspace-Symbole</b></summary>

![Workspace symbols](../../assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 Signaturhilfe</b></summary>

![Signature help](../../assets/videos/sig_help.gif)

</details>

&nbsp;

## ⚖️ Vergleich

Vergleich von LazyVerilog mit den beiden am häufigsten eingesetzten Open-Source-SystemVerilog-LSPs:
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
 und [`svlangserver`](https://github.com/imc-trading/svlangserver).

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM, Klassen, Packages | ✅ | ⚠️ | ❌ |
| Runtime | C++ | C++ | Node.js |
| Parser | slang | Verible | eigener Indexer |
| Diagnosen | Parsing, Lint, Semantik | Parsing, Lint | Verilator |
| Formatierung | ✅ | ✅ | über Verible |
| Gehe zu Definition | ✅ | ✅ | ✅ |
| Referenzen suchen | ✅ | ✅ | ❌ |
| Umbenennen | ✅ | ❌ | ❌ |
| Hover | ✅ | ⚠️ | ✅ |
| Vervollständigung | ✅ | ❌ | ✅ |
| Signaturhilfe | ✅ | ❌ | ✅ |
| Inlay-Hints | ✅ | ❌ | ❌ |
| Symbole | ✅ | ✅ | ✅ |
| Automatische Lint-Korrekturen | ✅ (`--fix`) | ✅ | ❌ |
| RTL-Code-Generierung | ✅ | ❌ | ❌ |
| RTL-Hierarchieansicht | ✅ | ❌ | ✅ |
| Interface und Verbindung | ✅ | ❌ | ❌ |

⚠️ bedeutet teilweise oder experimentell.

&nbsp;

## 📊 Funktionen

Was LazyVerilog derzeit unterstützt:

| Funktion | Status | Anmerkungen |
|---------|--------|-------|
| Formatierung | ✅ | Konfigurierbar über `lazyverilog.toml` |
| Lint-Diagnosen | ✅ | Parse-Diagnosen, optionale semantische Diagnosen sowie konfigurierbare Lint- und Style-Regeln |
| Lint-Quick-Fixes | ⚠️ | Fehlendes `default` ergänzen, `automatic`-Lifetime ergänzen |
| Gehe zu Definition | ✅ | Module, Instanzen, Ports, benannte Argumente, Symbole und Makros |
| Referenzen suchen | ✅ | Symbole und Makros über offene Dateien und konfigurierte Projektdateien hinweg |
| Symbol umbenennen | ✅ | Best-Effort über alle Projektdateien |
| Hover | ✅ | Symboldetails für Module, Ports, Signale, Parameter, Typedefs, Subroutinen und Makros |
| Vervollständigung | ✅ | Kontextbezogene Auto-Vervollständigungen |
| Signaturhilfe | ✅ | Funktionen und Tasks |
| Inlay-Hints | ✅ | Portrichtungen bei der Instanziierung |
| Dokumentsymbole | ✅ | Gliederung der aktuellen Datei |
| Workspace-Symbole | ✅ | Module, Interfaces, Packages, Klassen und Methoden; Funktionen, Tasks und Typedefs; `pkg::name`-Abfragen |
| RTL-Baum | ✅ | Hierarchie der Modulinstanziierungen |
| Auto-Instanziierung / Auto-Wire / Auto-Arg / Auto-Function / Auto-FF | ✅ | Verschiedene Code Actions zur RTL-Generierung |
| Projektlokale Konfiguration | ✅ | Vollständige Anpassung über `lazyverilog.toml` im Projektwurzelverzeichnis |

&nbsp;

## 🚀 Erste Schritte

Installation, Verwendung und Konfiguration für Neovim und VS Code: **https://lazyverilog.github.io**
