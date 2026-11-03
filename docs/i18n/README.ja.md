> この文書は Claude による自動翻訳です。内容の正確性については英語版の原文 [README.md](../../README.md) を参照してください。

<p align="center">
  <img src="../../docs/public/logo.webp" alt="LazyVerilog logo" width="260">
</p>

<h1 align="center">⚡ LazyVerilog</h1>

<p align="center">
  <b>RTL コーディングのための、高速で実用的な SystemVerilog LSP。</b>
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
  <b>日本語</b>
</p>

<p align="center">
  <a href="#-デモ">デモ</a>
  ·
  <a href="#-比較">比較</a>
  ·
  <a href="#-機能">機能</a>
  ·
  <a href="#-はじめに">はじめに</a>
</p>


<p align="center">
  LazyVerilog は C++ で書かれた SystemVerilog LSP で、neovim と vscode に対応しています。
  実際の SystemVerilog プロジェクトに対して、フォーマット、lint、ナビゲーション、ホバー、補完、インレイヒント、RTL コードアクションを提供します。
</p>

<p align="center">
  <img src="../../assets/videos/AutoInst.gif" alt="Auto-instantiation: generate a module instance with its ports connected" width="720">
</p>

<p align="center">
  <sub>LazyVerilog が役に立ったら、⭐ を付けていただけると他の RTL エンジニアにも見つけてもらいやすくなります。</sub>
</p>

&nbsp;

## 🎬 デモ

<details>
<summary><b>🎨 フォーマット</b></summary>

![Formatting](../../assets/videos/Format.gif)

</details>

<details>
<summary><b>🚨 Lint 診断</b></summary>

![Lint diagnostics](../../assets/videos/lint_diagnostics.png)

</details>

<details>
<summary><b>⚡ 自動補完</b></summary>

![Auto-complete](../../assets/videos/AutoComplete.gif)

</details>

<details>
<summary><b>🌳 RTL ツリー</b></summary>

![RTL tree](../../assets/videos/RtlTree.gif)

</details>

<details>
<summary><b>📂 折りたたみ</b></summary>

![Folding](../../assets/videos/Folding.gif)

</details>

<details>
<summary><b>🧭 定義へジャンプ</b></summary>

![Go to definition](../../assets/videos/GoToDef.gif)

</details>

<details>
<summary><b>🔗 インターフェース接続</b></summary>

![Interface connect](../../assets/videos/InterfaceConnect.gif)

</details>

<details>
<summary><b>🧩 自動インスタンス化</b></summary>

![Auto-instantiation](../../assets/videos/AutoInst.gif)

</details>

<details>
<summary><b>🔌 自動 wire 生成</b></summary>

![Auto-wire](../../assets/videos/AutoWire.gif)

</details>

<details>
<summary><b>🛠️ 自動 arg 生成</b></summary>

![Auto-arg](../../assets/videos/AutoArg.gif)

</details>

<details>
<summary><b>💡 ホバー</b></summary>

![Hover](../../assets/videos/hover.gif)

</details>

<details>
<summary><b>💬 インレイヒント</b></summary>

![Inlay hints](../../assets/videos/inlay_hint.png)

</details>

<details>
<summary><b>🔍 参照の検索</b></summary>

![Find references](../../assets/videos/get_reference.gif)

</details>

<details>
<summary><b>✏️ リネーム</b></summary>

![Rename](../../assets/videos/rename.gif)

</details>

<details>
<summary><b>🔭 ワークスペースシンボル</b></summary>

![Workspace symbols](../../assets/videos/workspace_symbols.gif)

</details>

<details>
<summary><b>📝 シグネチャヘルプ</b></summary>

![Signature help](../../assets/videos/sig_help.gif)

</details>

&nbsp;

## ⚖️ 比較

LazyVerilog と、最も広く使われているオープンソースの SystemVerilog LSP である
[`verible`](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
および [`svlangserver`](https://github.com/imc-trading/svlangserver) との比較です。

| | ⚡ LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM・クラス・パッケージ | ✅ | ⚠️ | ❌ |
| ランタイム | C++ | C++ | Node.js |
| パーサー | slang | Verible | 独自インデクサ |
| 診断 | パース・lint・セマンティック | パース・lint | Verilator |
| フォーマット | ✅ | ✅ | Verible 経由 |
| 定義へジャンプ | ✅ | ✅ | ✅ |
| 参照の検索 | ✅ | ✅ | ❌ |
| リネーム | ✅ | ❌ | ❌ |
| ホバー | ✅ | ⚠️ | ✅ |
| 補完 | ✅ | ❌ | ✅ |
| シグネチャヘルプ | ✅ | ❌ | ✅ |
| インレイヒント | ✅ | ❌ | ❌ |
| シンボル | ✅ | ✅ | ✅ |
| Lint の自動修正 | ✅ (`--fix`) | ✅ | ❌ |
| RTL コード生成 | ✅ | ❌ | ❌ |
| RTL 階層ビュー | ✅ | ❌ | ✅ |
| インターフェースと接続 | ✅ | ❌ | ❌ |

⚠️ は部分対応または実験的な対応を意味します。

&nbsp;

## 📊 機能

LazyVerilog が現在サポートしている機能:

| 機能 | 状態 | 備考 |
|---------|--------|-------|
| フォーマット | ✅ | `lazyverilog.toml` で設定可能 |
| Lint 診断 | ✅ | パース診断、オプションのセマンティック診断、設定可能な lint / スタイルルール |
| Lint クイックフィックス | ⚠️ | 欠けている `default` の追加、`automatic` ライフタイムの追加 |
| 定義へジャンプ | ✅ | モジュール、インスタンス、ポート、名前付き引数、シンボル、マクロ |
| 参照の検索 | ✅ | 開いているファイルおよび設定されたプロジェクトファイル全体にわたるシンボルとマクロ |
| シンボルのリネーム | ✅ | プロジェクトファイル全体に対してベストエフォートで実行 |
| ホバー | ✅ | モジュール、ポート、信号、パラメータ、typedef、サブルーチン、マクロのシンボル詳細 |
| 補完 | ✅ | コンテキストに応じた自動補完 |
| シグネチャヘルプ | ✅ | function と task |
| インレイヒント | ✅ | インスタンス化時のポート方向 |
| ドキュメントシンボル | ✅ | 現在のファイルのアウトライン |
| ワークスペースシンボル | ✅ | モジュール、インターフェース、パッケージ、クラスとメソッド、function、task、typedef、`pkg::name` クエリ |
| RTL ツリー | ✅ | モジュールのインスタンス化階層 |
| 自動インスタンス化 / 自動 wire / 自動 arg / 自動 function / 自動 FF | ✅ | RTL 生成のためのさまざまな Code Action |
| プロジェクトローカル設定 | ✅ | プロジェクトルートの `lazyverilog.toml` で全面的にカスタマイズ可能 |

&nbsp;

## 🚀 はじめに

Neovim および VS Code 向けのインストール方法、使い方、設定について: **https://lazyverilog.github.io**
