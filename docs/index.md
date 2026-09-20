---
layout: home
title: LazyVerilog
titleTemplate: A fast, practical SystemVerilog LSP

hero:
  name: LazyVerilog
  text: A fast SystemVerilog LSP.
  tagline: Written in C++, parsed by slang. For Neovim and VS Code.
  image:
    src: /logo.webp
    alt: LazyVerilog logo, a sleeping otter resting on the letters LV
  actions:
    - theme: brand
      text: Installation
      link: /installation/
    - theme: alt
      text: Features
      link: /features
    - theme: alt
      text: Usage
      link: /usage/
    - theme: alt
      text: Configuration
      link: /configuration
    - theme: alt
      text: Sponsor
      link: https://github.com/sponsors/kjoonha
---

## Install

Both editors download the matching `lazyverilog-lsp` server on first use.

### Neovim

With [`lazy.nvim`](https://github.com/folke/lazy.nvim):

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

More in [Install for Neovim](/installation/neovim).

### VS Code

Install from the
[Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=lazyverilog.lazyverilog-vscode),
then open a `.sv` file. More in [Install for VS Code](/installation/vscode).

### Then

Add a [`lazyverilog.toml`](/usage/) to your project. It points LazyVerilog at your filelist.
