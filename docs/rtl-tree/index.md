# RTL Tree

**Commands:** `lazyverilog.rtlTree`, `lazyverilog.rtlTreeReverse`

Builds a JSON instantiation hierarchy tree for the current module.

- `rtlTree` — forward hierarchy: the module and all its instantiated children (recursively).
- `rtlTreeReverse` — reverse hierarchy: where the current module is instantiated.

![Neovim's RtlTree window listing soc_top, its instances and their children beside the source file](/screenshots/rtl-tree-neovim.webp)

```toml
[rtltree]
show_instance_name = true
show_file = true
```

| Section | Option | Type | Default | Description |
|---------|--------|------|---------|-------------|
| `rtltree` | `show_instance_name` | bool | `true` | Show `module (u_instance)` instead of just `module` |
| `rtltree` | `show_file` | bool | `true` | Show the file each module is defined in |

Neovim and `lazyverilog-rtltree` append `(u_instance)` and `[path/to/file.sv]` to each line. VS Code
shows the instance name in the label and the file as the item's description; the full path is always
in the tooltip.

In Neovim, `setup({ rtltree = { show_instance_name = false, show_file = false } })` overrides these two
values for every project.

### Neovim: alignment

`:RtlTree align` (or `:RtlTreeReverse align`) pads the module and instance columns so `(u_instance)`
and `[path]` line up on every row. `:RtlTree noalign` turns it off. With no argument, the default
comes from `setup()`:

```lua
require("lazyverilog").setup({ rtltree = { align = true } })  -- default: false
```

`r` (refresh) keeps the layout the tree was opened with.

### Neovim: file paths

The server reports each file as an absolute path. `rtltree.path` sets how Neovim shows it:

| Value | Shows |
|-------|-------|
| `"cwd"` (default) | Relative to Vim's `:pwd`; `~/…` for files outside it under your home; otherwise absolute |
| `"project"` | Relative to the directory of the nearest `lazyverilog.toml` above the file; absolute if there is none |
| `"tail"` | File name only |
| `"absolute"` | Full path |

```lua
require("lazyverilog").setup({ rtltree = { path = "project" } })
```

Only the label changes: `<CR>` and the splits always open the real file. `lazyverilog-rtltree` (the CLI)
still prints absolute paths.

### Neovim: highlight groups

Each part of a row has its own highlight group. They are all linked with `default = true`, so your own
`:highlight` settings and colorscheme win.

| Group | Default link | Part |
|-------|--------------|------|
| `LazyVerilogRtlTreeModule` | `Type` | module name |
| `LazyVerilogRtlTreeInstance` | `Identifier` | `(u_instance)` |
| `LazyVerilogRtlTreeFile` | `Comment` | `[path/to/file.sv]` |
| `LazyVerilogRtlTreeGuide` | `NonText` | `├─` `└─` `│` tree guides |
| `LazyVerilogRtlTreeMarker` | `WarningMsg` | `<recursive>` |
| `LazyVerilogRtlTreeTitle` | `Title` | header line |
| `LazyVerilogKeyGuide` | `Comment` | key help line |

To show a part as plain text, link it to `Normal`:

```lua
vim.api.nvim_set_hl(0, "LazyVerilogRtlTreeFile", { link = "Normal" })
```

### Keys in the Neovim window

| Key | Action |
|-----|--------|
| `<CR>` | Jump to the module's definition |
| `o` / `v` / `t` | Open it in a split / vertical split / tab |
| `r` | Rebuild the tree |
| `q` | Close the window |

In VS Code, click an item to jump to it. `LazyVerilog: Refresh RTL Tree` (also the refresh button in
the view's title bar) rebuilds the tree.
