---
layout: home
title: LazyVerilog
titleTemplate: A fast, practical SystemVerilog LSP

hero:
  name: LazyVerilog
  text: A fast, practical SystemVerilog LSP for RTL coding.
  tagline: Written in C++ and parsed by slang. Formatting, linting, navigation, and RTL automation for Neovim and VS Code.
  image:
    src: /logo.webp
    alt: LazyVerilog logo, a sleeping otter resting on the letters LV
  actions:
    - theme: brand
      text: Installation
      link: /installation
    - theme: alt
      text: Usage
      link: /usage
    - theme: alt
      text: Configuration
      link: /configuration
    - theme: alt
      text: Sponsor
      link: https://github.com/sponsors/kjoonha

features:
  - icon: 🎯
    title: Accurate parsing
    details: SystemVerilog is parsed by slang, including UVM, classes, and packages.
    link: /design/
  - icon: 🎨
    title: Formatting
    details: An idempotent, token-based formatter you configure per project.
    link: /formatter/options
  - icon: 🚨
    title: Lint diagnostics
    details: Parse diagnostics, optional semantic diagnostics, and configurable naming and style rules.
    link: /linter/options
  - icon: 🧭
    title: Navigation
    details: Go to definition, find references, and rename across your project files.
    link: /lsp/
  - icon: 💡
    title: Hover, completion, signature help
    details: Symbol details, context-aware completion, and signatures for functions and tasks.
    link: /lsp/completion
  - icon: 💬
    title: Inlay hints
    details: Port directions shown on every instantiation.
    link: /lsp/
  - icon: ⚙️
    title: RTL automation
    details: AutoInst, AutoWire, AutoArg, AutoFunc, and AutoFF as code actions.
    link: /autoinst/
  - icon: 🌳
    title: RTL tree and connect
    details: Browse the module hierarchy and wire instances together interactively.
    link: /rtl-tree/
  - icon: 🧰
    title: Project-local config
    details: One lazyverilog.toml near your RTL controls the whole toolchain.
    link: /configuration
  - icon: ⌨️
    title: CLI tools
    details: lazyverilog-fmt, lazyverilog-lint, and lazyverilog-rtltree for scripts and CI.
    link: /cli
  - icon: 🧩
    title: Neovim and VS Code
    details: Native plugin and extension that install the matching server binary for you.
    link: /installation
---
