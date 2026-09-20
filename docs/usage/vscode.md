# Usage in VS Code

Open a Verilog or SystemVerilog file. LazyVerilog starts on its own. Not installed yet? See
[Install for VS Code](../installation/vscode.md).

Hover, go to definition, find references, rename, completion, and signature help use VS Code's
standard shortcuts. Code actions (`Ctrl+.`) offer [AutoInst](../autoinst/index.md),
[AutoWire](../autowire/index.md), [AutoArg](../autoarg/index.md), [AutoFunc](../autofunc/index.md),
and [AutoFF](../autoff/index.md) when the cursor is on a supported construct.

## Commands

Open the Command Palette with `Ctrl+Shift+P`:

| Command | Description |
|---------|-------------|
| `LazyVerilog: Format Document` | Format the file |
| `LazyVerilog: Lint Current File` | Show diagnostics for the file |
| `LazyVerilog: Lint All Files` | Show diagnostics for all indexed project files |
| `LazyVerilog: Show RTL Hierarchy` | Open the [module hierarchy](../rtl-tree/index.md) |
| `LazyVerilog: Show RTL Hierarchy (Reverse)` | Open the reverse hierarchy from the current module |
| `LazyVerilog: Interface` | Inspect and edit instance connections ([Interface](../interface.md)) |
| `LazyVerilog: Connect` | Connect two modules through the hierarchy ([Connect](../connect.md)) |
