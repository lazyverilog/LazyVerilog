# LSP Features

Standard Language Server Protocol features. None of these need configuration except where noted.

| Feature | What it does |
|---------|--------------|
| Hover | Kind, type, and documentation of the symbol under the cursor |
| Go to definition | Jumps to the declaration |
| Find references | All uses of a symbol, including macros (`` `WIDTH `` at its `` `define `` or any use) |
| Rename | Renames a symbol across all references. Keywords cannot be renamed |
| Completion | Context-aware suggestions; see [Completion](completion.md) |
| Signature help | Parameters while typing a call or a `#(...)` list; triggers on `(` and `,` |
| Workspace symbols | Case-insensitive substring search over modules and classes. Needs `[design].vcode` |
| Inlay hints | See below |
| Folding ranges | See [Folding](../folding/index.md) |

Cross-file features need the project indexed through a filelist. See
[Design & filelist](../design/index.md).

## Inlay hints

Inside each module instantiation, LazyVerilog shows the direction and type of every connected port,
and an `N/M` count of connected ports at the opening parenthesis. Instances of modules from the
filelist show hints once the background index is ready, so a file opened at startup may briefly show
none.

```toml
[inlay_hint]
enable = true
```

## Folding ranges

```toml
[folding]
enable = true
```

Neovim asks for folds and inlay hints on every edit. With `enable = false` the server answers with
nothing. To stop the requests altogether, also set `folding = false` or `inlay_hints = false` in the
Neovim [`setup()`](../installation/neovim.md).
