# Install for Neovim

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

Open a `.sv` file. The first launch downloads the server.

## Options

Pass options to `setup()`:

```lua
require("lazyverilog").setup({
  cmd = "/path/to/lazyverilog-lsp",
  folding = true,
  inlay_hints = true,
})
```

| Option | Default | Effect |
|--------|---------|--------|
| `cmd` | auto | Use this server binary instead of `PATH` or the managed download |
| `cmd_args` | `{}` | Extra server arguments, e.g. `{ "--implicit-project=off" }` |
| `folding` | `true` | Fold from the server's folding ranges |
| `inlay_hints` | `true` | Show inlay hints |

Set `folding` or `inlay_hints` to `false` for very large files or a slow machine.

Without a `lazyverilog.toml`, the server indexes the git checkout around the file. See
[Without a lazyverilog.toml](../configuration.md#without-a-lazyverilog-toml).

Next: [Usage in Neovim](../usage/neovim.md).
