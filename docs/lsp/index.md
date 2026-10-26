# LSP Features

Standard Language Server Protocol features. None of these need configuration except where noted.

| Feature | What it does |
|---------|--------------|
| Hover | Kind, type, and documentation of the symbol under the cursor |
| Go to definition | Jumps to the declaration |
| Find references | All uses of a symbol, including macros (`` `WIDTH `` at its `` `define `` or any use) |
| Rename | Renames a symbol across all references. Keywords cannot be renamed |
| Document highlight | Marks every occurrence of the symbol under the cursor in the current file |
| Selection range | Expands the selection step by step to each enclosing piece of syntax (smart selection) |
| Completion | Context-aware suggestions; see [Completion](completion.md) |
| Signature help | Parameters while typing a call or a `#(...)` list; triggers on `(` and `,` |
| Document symbols | Outline of the current file; see below |
| Workspace symbols | Search across the project; see below |
| Inlay hints | See below |
| Folding ranges | See [Folding](../folding/index.md) |

Cross-file features need the project indexed through a filelist. See
[Design & filelist](../design/index.md).

## Document symbols

The outline of the current file: modules, interfaces and packages with their ports, parameters,
signals, functions, tasks, instances, modports and generate blocks; classes with their properties and
methods; typedefs with their enum members and struct fields.

## Workspace symbols

A case-insensitive substring search over the open files and the indexed project files (the filelist,
or the [implicit project](../configuration.md#without-a-lazyverilog-toml)):

- modules, interfaces, packages and classes, and each class's methods, always;
- functions and tasks of packages and modules, and typedefs (struct, enum, other), once the query is
  not empty.

`scope::name` matches `name` inside a package, class or module whose name contains `scope`, and
`::scope::name` needs the scope to match exactly. An exact name ranks first, then a prefix, then a
match at the start of a `_`-separated word, then anything else; at equal rank, modules, interfaces and
packages come before classes, and classes before their members. At most 100 results are returned.

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
