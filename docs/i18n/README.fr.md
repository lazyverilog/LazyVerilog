> ⚠️ Traduction automatique générée par Claude : la version originale en anglais ([README.md](../../README.md)) fait référence.

<p align="center">
  <img src="../../docs/public/logo.webp" alt="Logo LazyVerilog" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>Un LSP SystemVerilog rapide et pragmatique pour le codage RTL.</b>
</p>

<p align="center">
  <a href="https://github.com/lazyverilog/LazyVerilog/actions/workflows/ci.yml"><img src="https://github.com/lazyverilog/LazyVerilog/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/lazyverilog/LazyVerilog/actions/workflows/release.yml"><img src="https://github.com/lazyverilog/LazyVerilog/actions/workflows/release.yml/badge.svg" alt="Release"></a>
</p>

<p align="center">
  <a href="https://github.com/lazyverilog/LazyVerilog/stargazers">
    <img alt="Étoiles GitHub" src="https://img.shields.io/github/stars/lazyverilog/LazyVerilog?style=for-the-badge&logo=starship&label=%E2%AD%90%20Stars&color=ffd166&labelColor=2b2d42">
  </a>
  <a href="https://github.com/lazyverilog/LazyVerilog/releases/latest">
    <img alt="Dernière version" src="https://img.shields.io/github/v/release/lazyverilog/LazyVerilog?style=for-the-badge&logo=github">
  </a>
  <a href="https://github.com/lazyverilog/LazyVerilog/issues">
    <img alt="Tickets GitHub" src="https://img.shields.io/github/issues/lazyverilog/LazyVerilog?style=for-the-badge">
  </a>
  <a href="https://github.com/sponsors/kjoonha">
    <img alt="Sponsoriser kjoonha" src="https://img.shields.io/badge/Sponsor-kjoonha-ea4aaa?style=for-the-badge&logo=githubsponsors&logoColor=white&labelColor=2b2d42">
  </a>
</p>

<p align="center">
  <a href="../../README.md">English</a>
  ·
  <a href="../../docs/i18n/README.ko.md">한국어</a>
  ·
  <b>Français</b>
</p>

<p align="center">
  <a href="#-démo">Démo</a>
  ·
  <a href="#-comparaison">Comparaison</a>
  ·
  <a href="#-fonctionnalités">Fonctionnalités</a>
  ·
  <a href="#-premiers-pas">Premiers pas</a>
</p>


<p align="center">
  LazyVerilog est un LSP SystemVerilog écrit en C++, avec prise en charge de neovim et vscode.
  Il offre le formatage, le linting, la navigation, les info-bulles, l'autocomplétion, les inlay hints et des code actions RTL pour de vrais projets SystemVerilog.
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="Auto-instanciation : génération d'une instance de module avec ses ports connectés" width="720">
</p>

<p align="center">
  <sub>Si LazyVerilog vous fait gagner du temps, une ⭐ aide les autres ingénieurs RTL à le découvrir.</sub>
</p>

&nbsp;

## 🎬 Démo

<details>
<summary><b>🎨 Formatage</b></summary>

![Formatage](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Diagnostics du linter</b></summary>

![Diagnostics du linter](../../assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ Autocomplétion</b></summary>

![Autocomplétion](../../assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 Arborescence RTL</b></summary>

![Arborescence RTL](../../assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 Pliage de code</b></summary>

![Pliage de code](../../assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 Aller à la définition</b></summary>

![Aller à la définition](../../assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 Connexion d'interface</b></summary>

![Connexion d'interface](../../assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 Auto-instanciation</b></summary>

![Auto-instanciation](../../assets/videos/AutoInst.gif)

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
<summary><b>💡 Info-bulles</b></summary>

![Info-bulles](../../assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 Inlay hints</b></summary>

![Inlay hints](../../assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 Rechercher les références</b></summary>

![Rechercher les références](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ Renommage</b></summary>

![Renommage](../../assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 Symboles de l'espace de travail</b></summary>

![Symboles de l'espace de travail](../../assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 Aide à la signature</b></summary>

![Aide à la signature](../../assets/videos/sig_help.gif)

</details>

&nbsp;

## ⚖️ Comparaison

Comparaison entre LazyVerilog et les deux LSP SystemVerilog open-source les plus utilisés :
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
 et [`svlangserver`](https://github.com/imc-trading/svlangserver).

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM, classes, packages | ✅ | ⚠️ | ❌ |
| Environnement d'exécution | C++ | C++ | Node.js |
| Parser | slang | Verible | indexeur propriétaire |
| Diagnostics | parsing, lint, sémantique | parsing, lint | Verilator |
| Formatage | ✅ | ✅ | via Verible |
| Aller à la définition | ✅ | ✅ | ✅ |
| Rechercher les références | ✅ | ✅ | ❌ |
| Renommage | ✅ | ❌ | ❌ |
| Info-bulles | ✅ | ⚠️ | ✅ |
| Autocomplétion | ✅ | ❌ | ✅ |
| Aide à la signature | ✅ | ❌ | ✅ |
| Inlay hints | ✅ | ❌ | ❌ |
| Symboles | ✅ | ✅ | ✅ |
| Correction automatique du lint | ✅ (`--fix`) | ✅ | ❌ |
| Génération de code RTL | ✅ | ❌ | ❌ |
| Vue de la hiérarchie RTL | ✅ | ❌ | ✅ |
| Interface et connexion | ✅ | ❌ | ❌ |

⚠️ signifie partiel ou expérimental.

&nbsp;

## 📊 Fonctionnalités

Ce que LazyVerilog prend en charge actuellement :

| Fonctionnalité | Statut | Remarques |
|---------|--------|-------|
| Formatage | ✅ | Configurable via `lazyverilog.toml` |
| Diagnostics du linter | ✅ | Diagnostics de parsing, diagnostics sémantiques optionnels et règles de lint/style configurables |
| Corrections rapides du linter | ⚠️ | Ajout d'un `default` manquant, ajout d'une durée de vie `automatic` |
| Aller à la définition | ✅ | Modules, instances, ports, arguments nommés, symboles et macros |
| Rechercher les références | ✅ | Symboles et macros dans les fichiers ouverts et les fichiers du projet configurés |
| Renommer un symbole | ✅ | Au mieux possible, dans l'ensemble des fichiers du projet |
| Info-bulles | ✅ | Détails des symboles pour les modules, ports, signaux, paramètres, typedefs, sous-programmes et macros |
| Autocomplétion | ✅ | Autocomplétions contextuelles |
| Aide à la signature | ✅ | Fonctions et tâches |
| Inlay hints | ✅ | Direction des ports à l'instanciation |
| Symboles du document | ✅ | Plan du fichier courant |
| Symboles de l'espace de travail | ✅ | Modules, interfaces, packages, classes et méthodes ; fonctions, tâches et typedefs ; requêtes `pkg::name` |
| Arborescence RTL | ✅ | Hiérarchie d'instanciation des modules |
| Auto-instanciation / Auto-wire / Auto-arg / Auto-function / Auto-FF | ✅ | Diverses Code Actions pour la génération de RTL |
| Configuration locale au projet | ✅ | Personnalisation complète via `lazyverilog.toml` à la racine du projet |

&nbsp;

## 🚀 Premiers pas

Installation, utilisation et configuration pour Neovim et VS Code : **https://lazyverilog.github.io**
