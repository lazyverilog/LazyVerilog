# Usage in VS Code

Open a Verilog or SystemVerilog file. LazyVerilog starts on its own. Not installed yet? See
[Install for VS Code](../installation/vscode.md).

Hover, go to definition, find references, rename, completion, and signature help use VS Code's
standard shortcuts. Code actions (`Ctrl+.`) offer [AutoInst](../autoinst/index.md),
[AutoWire](../autowire/index.md), [AutoArg](../autoarg/index.md), [AutoFunc](../autofunc/index.md),
and [AutoFF](../autoff/index.md) when the cursor is on a supported construct, and
[quick fixes](../linter/options.md#quick-fixes) for some lint diagnostics. Outside a class, the menu
also offers `Insert always_ff block` and `Insert always_comb block`, which insert an empty template at
the cursor line.

## Commands

Open the Command Palette with `Ctrl+Shift+P`:

| Command | Description |
|---------|-------------|
| `LazyVerilog: Format Document` | Format the file |
| `LazyVerilog: Lint Current File` | Show diagnostics for the file |
| `LazyVerilog: Lint All Files` | Show diagnostics for all indexed project files |
| `LazyVerilog: Show RTL Hierarchy` | Open the [module hierarchy](../rtl-tree/index.md) |
| `LazyVerilog: Show RTL Hierarchy (Reverse)` | Open the reverse hierarchy from the current module |
| `LazyVerilog: Refresh RTL Tree` | Rebuild the tree shown in the RTL Hierarchy view |
| `LazyVerilog: Interface` | Inspect and edit instance connections ([Interface](../interface.md)) |
| `LazyVerilog: Connect` | Connect two modules through the hierarchy ([Connect](../connect.md)) |

## Formatting

`Format Document` (`Shift+Alt+F`) and `LazyVerilog: Format Document` always format. Saving formats
only when `[format].enable_format_on_save = true` is set in `lazyverilog.toml`: the server formats
in `textDocument/willSaveWaitUntil`, which VS Code sends on every save, so `editor.formatOnSave`
is not needed. If you also turn on `editor.formatOnSave` for these files, a save formats twice
(the second pass changes nothing).
