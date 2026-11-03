> Esta é uma tradução automática gerada pelo Claude. A versão em inglês ([README.md](../../README.md)) é a referência oficial.

<p align="center">
  <img src="../../docs/public/logo.webp" alt="Logo do LazyVerilog" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>Um LSP de SystemVerilog rápido e prático para codificação de RTL.</b>
</p>

<p align="center">
  <a href="https://github.com/lazyverilog/LazyVerilog/actions/workflows/ci.yml"><img src="https://github.com/lazyverilog/LazyVerilog/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/lazyverilog/LazyVerilog/actions/workflows/release.yml"><img src="https://github.com/lazyverilog/LazyVerilog/actions/workflows/release.yml/badge.svg" alt="Release"></a>
</p>

<p align="center">
  <a href="https://github.com/lazyverilog/LazyVerilog/stargazers">
    <img alt="Estrelas no GitHub" src="https://img.shields.io/github/stars/lazyverilog/LazyVerilog?style=for-the-badge&logo=starship&label=%E2%AD%90%20Stars&color=ffd166&labelColor=2b2d42">
  </a>
  <a href="https://github.com/lazyverilog/LazyVerilog/releases/latest">
    <img alt="Última release" src="https://img.shields.io/github/v/release/lazyverilog/LazyVerilog?style=for-the-badge&logo=github">
  </a>
  <a href="https://github.com/lazyverilog/LazyVerilog/issues">
    <img alt="Issues no GitHub" src="https://img.shields.io/github/issues/lazyverilog/LazyVerilog?style=for-the-badge">
  </a>
  <a href="https://github.com/sponsors/kjoonha">
    <img alt="Patrocine kjoonha" src="https://img.shields.io/badge/Sponsor-kjoonha-ea4aaa?style=for-the-badge&logo=githubsponsors&logoColor=white&labelColor=2b2d42">
  </a>
</p>

<p align="center">
  <b>English</b>
  ·
  <a href="../../docs/i18n/README.ko.md">한국어</a>
</p>

<p align="center">
  <a href="#-demonstração">Demonstração</a>
  ·
  <a href="#-comparação">Comparação</a>
  ·
  <a href="#-funcionalidades">Funcionalidades</a>
  ·
  <a href="#-primeiros-passos">Primeiros passos</a>
</p>


<p align="center">
  O LazyVerilog é um LSP de SystemVerilog escrito em C++ com suporte a neovim e vscode.
  Ele oferece formatação, linting, navegação, hover, autocompletar, inlay hints e code actions de RTL para projetos SystemVerilog reais.
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="Auto-instanciação: gera uma instância de módulo com suas portas conectadas" width="720">
</p>

<p align="center">
  <sub>Se o LazyVerilog economiza seu tempo, uma ⭐ ajuda outros engenheiros de RTL a encontrá-lo.</sub>
</p>

&nbsp;

## 🎬 Demonstração

<details>
<summary><b>🎨 Formatação</b></summary>

![Formatação](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Diagnósticos de lint</b></summary>

![Diagnósticos de lint](../../assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ Autocompletar</b></summary>

![Autocompletar](../../assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 Árvore de RTL</b></summary>

![Árvore de RTL](../../assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 Folding</b></summary>

![Folding](../../assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 Ir para a definição</b></summary>

![Ir para a definição](../../assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 Conexão de interface</b></summary>

![Conexão de interface](../../assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 Auto-instanciação</b></summary>

![Auto-instanciação](../../assets/videos/AutoInst.gif)

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
<summary><b>💬 Inlay hints</b></summary>

![Inlay hints](../../assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 Localizar referências</b></summary>

![Localizar referências](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ Renomear</b></summary>

![Renomear](../../assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 Símbolos do workspace</b></summary>

![Símbolos do workspace](../../assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 Ajuda de assinatura</b></summary>

![Ajuda de assinatura](../../assets/videos/sig_help.gif)

</details>

&nbsp;

## ⚖️ Comparação

Comparação entre o LazyVerilog e os dois LSP de SystemVerilog open-source mais utilizados:
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
 e [`svlangserver`](https://github.com/imc-trading/svlangserver).

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM, classes, packages | ✅ | ⚠️ | ❌ |
| Runtime | C++ | C++ | Node.js |
| Parser | slang | Verible | indexador próprio |
| Diagnósticos | parse, lint, semântico | parse, lint | Verilator |
| Formatação | ✅ | ✅ | via Verible |
| Ir para a definição | ✅ | ✅ | ✅ |
| Localizar referências | ✅ | ✅ | ❌ |
| Renomear | ✅ | ❌ | ❌ |
| Hover | ✅ | ⚠️ | ✅ |
| Autocompletar | ✅ | ❌ | ✅ |
| Ajuda de assinatura | ✅ | ❌ | ✅ |
| Inlay hints | ✅ | ❌ | ❌ |
| Símbolos | ✅ | ✅ | ✅ |
| Autofix de lint | ✅ (`--fix`) | ✅ | ❌ |
| Geração de código RTL | ✅ | ❌ | ❌ |
| Visualização da hierarquia de RTL | ✅ | ❌ | ✅ |
| Interface e conexão | ✅ | ❌ | ❌ |

⚠️ indica suporte parcial ou experimental.

&nbsp;

## 📊 Funcionalidades

O que o LazyVerilog suporta atualmente:

| Funcionalidade | Status | Observações |
|---------|--------|-------|
| Formatação | ✅ | Configurável via `lazyverilog.toml` |
| Diagnósticos de lint | ✅ | Diagnósticos de parse, diagnósticos semânticos opcionais e regras de lint/estilo configuráveis |
| Quick fixes de lint | ⚠️ | Adicionar um `default` ausente, adicionar um lifetime `automatic` |
| Ir para a definição | ✅ | Módulos, instâncias, portas, argumentos nomeados, símbolos e macros |
| Localizar referências | ✅ | Símbolos e macros nos arquivos abertos e nos arquivos de projeto configurados |
| Renomear símbolo | ✅ | Melhor esforço em todos os arquivos do projeto |
| Hover | ✅ | Detalhes de símbolos para módulos, portas, sinais, parâmetros, typedefs, subrotinas e macros |
| Autocompletar | ✅ | Sugestões sensíveis ao contexto |
| Ajuda de assinatura | ✅ | Funções e tasks |
| Inlay hints | ✅ | Direção das portas na instanciação |
| Símbolos do documento | ✅ | Outline do arquivo atual |
| Símbolos do workspace | ✅ | Módulos, interfaces, packages, classes e métodos; funções, tasks e typedefs; consultas `pkg::name` |
| Árvore de RTL | ✅ | Hierarquia de instanciação de módulos |
| Auto-instanciação / Auto-wire / Auto-arg / Auto-function / Auto-FF | ✅ | Diversas Code Actions para geração de RTL |
| Configuração local do projeto | ✅ | Personalização completa por meio do `lazyverilog.toml` na raiz do projeto |

&nbsp;

## 🚀 Primeiros Passos

Instalação, uso e configuração para Neovim e VS Code: **https://lazyverilog.github.io**
