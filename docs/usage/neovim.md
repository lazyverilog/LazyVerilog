# Usage in Neovim

```bash
nvim path/to/rtl.sv
```

Run `:LspInfo` to confirm the `lazyverilog` client is attached. Not installed yet? See
[Install for Neovim](../installation/neovim.md).

## Keymaps

Use your existing LSP keymaps, or add:

```lua
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "gr", vim.lsp.buf.references)
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename)
vim.keymap.set({ "n", "v" }, "<leader>ca", vim.lsp.buf.code_action)
```

The code action menu offers [AutoInst](../autoinst/index.md), [AutoWire](../autowire/index.md),
[AutoArg](../autoarg/index.md), [AutoFunc](../autofunc/index.md), and [AutoFF](../autoff/index.md)
when the cursor is on a supported construct.

## Commands

| Command | Description |
|---------|-------------|
| `:Format` | Format the buffer or visual range |
| `:Lint` | Show diagnostics for the buffer |
| `:LintAll` | Show diagnostics for all indexed project files |
| `:RtlTree` | Open the [module hierarchy](../rtl-tree/index.md) |
| `:RtlTreeReverse` | Open the reverse hierarchy from the current module |
| `:Interface <inst>` | Inspect one instance ([Interface](../interface.md)) |
| `:Interface <inst1> <inst2>` | Inspect and edit connections between two instances |
| `:Connect <module1> <module2>` | Connect two modules through the hierarchy ([Connect](../connect.md)) |
