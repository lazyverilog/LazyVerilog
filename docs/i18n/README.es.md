> Esta es una traducción automática generada por Claude; el original en inglés, [README.md](../../README.md), es la versión autoritativa.

<p align="center">
  <img src="../../docs/public/logo.webp" alt="LazyVerilog logo" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>Un LSP de SystemVerilog rápido y práctico para escribir RTL.</b>
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
  <b>Español</b>
  ·
  <a href="../../README.md">English</a>
  ·
  <a href="../../docs/i18n/README.ko.md">한국어</a>
</p>

<p align="center">
  <a href="#-demo">Demo</a>
  ·
  <a href="#-comparación">Comparación</a>
  ·
  <a href="#-características">Características</a>
  ·
  <a href="#-primeros-pasos">Primeros pasos</a>
</p>


<p align="center">
  LazyVerilog es un LSP de SystemVerilog escrito en C++ con soporte para neovim y vscode.
  Ofrece formateo, linting, navegación, hover, autocompletado, inlay hints y code actions de RTL para proyectos reales de SystemVerilog.
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="Auto-instanciación: genera una instancia de módulo con sus puertos conectados" width="720">
</p>

<p align="center">
  <sub>Si LazyVerilog te ahorra tiempo, una ⭐ ayuda a que otros ingenieros de RTL lo descubran.</sub>
</p>

&nbsp;

## 🎬 Demo

<details>
<summary><b>🎨 Formateo</b></summary>

![Formateo](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Diagnósticos del linter</b></summary>

![Diagnósticos del linter](../../assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ Autocompletado</b></summary>

![Autocompletado](../../assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 Árbol RTL</b></summary>

![Árbol RTL](../../assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 Plegado de código</b></summary>

![Plegado de código](../../assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 Ir a la definición</b></summary>

![Ir a la definición](../../assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 Conexión de interfaces</b></summary>

![Conexión de interfaces](../../assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 Auto-instanciación</b></summary>

![Auto-instanciación](../../assets/videos/AutoInst.gif)

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
<summary><b>🔍 Buscar referencias</b></summary>

![Buscar referencias](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ Renombrar</b></summary>

![Renombrar](../../assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 Símbolos del workspace</b></summary>

![Símbolos del workspace](../../assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 Ayuda de firma</b></summary>

![Ayuda de firma](../../assets/videos/sig_help.gif)

</details>

&nbsp;

## ⚖️ Comparación

Comparación entre LazyVerilog y los dos LSP de SystemVerilog de código abierto más utilizados:
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
 y [`svlangserver`](https://github.com/imc-trading/svlangserver).

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM, clases, paquetes | ✅ | ⚠️ | ❌ |
| Runtime | C++ | C++ | Node.js |
| Parser | slang | Verible | indexador propio |
| Diagnósticos | parseo, lint, semánticos | parseo, lint | Verilator |
| Formateo | ✅ | ✅ | mediante Verible |
| Ir a la definición | ✅ | ✅ | ✅ |
| Buscar referencias | ✅ | ✅ | ❌ |
| Renombrar | ✅ | ❌ | ❌ |
| Hover | ✅ | ⚠️ | ✅ |
| Autocompletado | ✅ | ❌ | ✅ |
| Ayuda de firma | ✅ | ❌ | ✅ |
| Inlay hints | ✅ | ❌ | ❌ |
| Símbolos | ✅ | ✅ | ✅ |
| Corrección automática del linter | ✅ (`--fix`) | ✅ | ❌ |
| Generación de código RTL | ✅ | ❌ | ❌ |
| Vista de jerarquía RTL | ✅ | ❌ | ✅ |
| Interfaces y conexión | ✅ | ❌ | ❌ |

⚠️ significa parcial o experimental.

&nbsp;

## 📊 Características

Lo que LazyVerilog soporta actualmente:

| Característica | Estado | Notas |
|---------|--------|-------|
| Formateo | ✅ | Configurable mediante `lazyverilog.toml` |
| Diagnósticos del linter | ✅ | Diagnósticos de parseo, diagnósticos semánticos opcionales y reglas de lint/estilo configurables |
| Correcciones rápidas del linter | ⚠️ | Añadir un `default` ausente, añadir un lifetime `automatic` |
| Ir a la definición | ✅ | Módulos, instancias, puertos, argumentos nombrados, símbolos y macros |
| Buscar referencias | ✅ | Símbolos y macros en los archivos abiertos y en los archivos del proyecto configurados |
| Renombrar símbolo | ✅ | En la medida de lo posible, en todos los archivos del proyecto |
| Hover | ✅ | Detalles de símbolos para módulos, puertos, señales, parámetros, typedefs, subrutinas y macros |
| Autocompletado | ✅ | Sugerencias sensibles al contexto |
| Ayuda de firma | ✅ | Funciones y tareas |
| Inlay hints | ✅ | Direcciones de los puertos en la instanciación |
| Símbolos del documento | ✅ | Esquema del archivo actual |
| Símbolos del workspace | ✅ | Módulos, interfaces, paquetes, clases y métodos; funciones, tareas y typedefs; consultas `pkg::name` |
| Árbol RTL | ✅ | Jerarquía de instanciación de módulos |
| Auto-instanciación / Auto-wire / Auto-arg / Auto-function / Auto-FF | ✅ | Diversas Code Actions para generar RTL |
| Configuración local del proyecto | ✅ | Personalización completa mediante `lazyverilog.toml` en la raíz del proyecto |

&nbsp;

## 🚀 Primeros pasos

Instalación, uso y configuración para Neovim y VS Code: **https://lazyverilog.github.io**
