# Recording the demos

Scripts that record the GIFs and screenshots used on the documentation site and in the README.
They drive a **real Neovim** with the plugin from this checkout, render its screen to HTML, and
capture frames with headless Chrome. Every recording comes out at the same size, in the same theme,
from the same demo project, so a new one matches the old ones.

## What you need

- Python 3 with `pip install pillow msgpack`
- Neovim **0.11 or newer** (the plugin calls `client:request`, which 0.10 lacks)
- Google Chrome or Chromium (`CHROME` overrides the path)
- `git` and network access the first time: the Catppuccin theme is cloned to `.deps/`
- A `lazyverilog-lsp` binary. Set `LV_LSP=/path/to/lazyverilog-lsp` to record a build you want to
  show. Unset, the plugin finds or downloads the release binary, as it does for a user.

`NVIM` overrides the Neovim binary.

## Run

```bash
python tools/record/scenes.py --list                # scene names
python tools/record/scenes.py Format hover          # record some; output goes to tools/record/out/
python tools/record/scenes.py --all                 # record everything
python tools/record/scenes.py --install             # copy out/ into assets/videos and docs/public/screenshots
```

`out/` is not committed. Look at each `<name>-last.png` (the final frame) before installing.

Installed names are fixed, so the Demos page, the README and the docs pick the new files up
without edits:

| Recorded as | Installed to |
|-------------|--------------|
| GIFs (`Format`, `AutoInst`, `hover`, ...) | `assets/videos/<name>.gif` |
| `lint_diagnostics`, `inlay_hint` | `assets/videos/<name>.png` |
| `rtl_tree_neovim`, `interface_neovim`, `interface_one_neovim`, `connect_port_neovim`, `connect_preview_neovim` | `docs/public/screenshots/<name-with-dashes>.webp` |

## The look, in one place

`settings.py` holds every number that decides how a recording looks; `init.lua` is the Neovim
profile.

| Setting | Value |
|---------|-------|
| Screen | 100 x 26 cells for GIFs (872 x 505 px). Stills: `interface_*` and `connect_*` 100 x 30 (1880 x 1250 px), `rtl_tree_neovim` 110 x 22 (2060 x 938), `lint_diagnostics` 110 x 22, `inlay_hint` 100 x 22 |
| Font | Cascadia Mono / Consolas, line height 1.3. 14 px for GIFs, 15 px for stills |
| Scale | GIF 1x, stills 2x |
| GIF palette | 96 colours, shared by all frames, no dithering |
| Theme | Catppuccin Mocha (`catppuccin/nvim`) |
| Editor | `expandtab`, `shiftwidth=4`, status line ` name [+] ... line:col `, diagnostics off unless a scene turns them on |
| Typing | 60 ms per character of a typed `:command` |

Changing a value changes every recording made afterwards, so re-record the whole set together.

## The demo project

`demo-proj/` is the RTL the scenes act on: `soc_top` with `cpu_core` (`alu`, `reg_file`),
`bus_ctrl` and `mem_ctrl`, plus one small file per feature (`fmt_demo`, `inst_demo`, `wire_demo`,
`arg_demo`, `comp_demo`, `sig_demo`, `fold_demo`, `m_lint_demo`). Its `lazyverilog.toml` keeps the
formatter's port columns narrow and turns on the lint rules the lint screenshot shows.

Scenes never save a file, so the project is unchanged by a recording. Line and column numbers in
`scenes.py` refer to these files: change one, and check the scenes that point into it.

## Adding a scene

Write a function in `scenes.py` and decorate it with `@gif` or `@still`. A scene is a list of steps
on a `Rec`: `hold`, `keys`, `type`, `cmd` (a visible `:command`), `cursor`, `code_action` (opens
`gra`, picks the entry by title), and `finish()` or `shot()`.

- Prefer the LSP's own entry points (`gra`, `grn`, `gd`, `K`) over `:commands` for editor features.
  A command the plugin defines, such as `:Format` or `:Interface`, is fine.
- After a step that starts a request, give `settle` enough time for the server to answer.
- Read the last frame of every GIF. A recording should end on the result, not on the action.

## Known limits

- On Windows, Enter on an RtlTree entry opens an empty buffer, so the RtlTree recording stops at
  the tree.
- After `:Interface` connects two ports, the window's own refresh reports "instance not found"
  while the edit is already applied. The Connect recording reopens the window instead.
