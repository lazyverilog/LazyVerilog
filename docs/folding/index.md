# Folding Ranges

LazyVerilog gives your editor folds for common SystemVerilog structure. They come from a fast scan of
the tokens, so they work while you type. Single-line constructs never fold.

| Fold | Covers |
|------|--------|
| Module | `module` through `endmodule`, header included |
| Header lists | the `#(...)` parameter list and the `(...)` port list |
| Declarations | a run of consecutive declarations |
| Imports | a run of consecutive `import` lines |
| Comments | consecutive own-line comments, and block comments |
| Preprocessor | each branch of `` `ifdef ``, `` `else ``, `` `endif `` |
| Blocks | `begin`/`end`, `case`, `generate`, `fork`/`join`, `function`, `task`, `class`, `constraint`, `covergroup`, `clocking`, `typedef enum`/`struct`/`union` |

## Declaration runs

A run folds when its lines start with a keyword or type: `logic`, `wire`, `reg`, `var`, `integer`,
`localparam`, `input`, and so on. Any other statement ends the run.

```systemverilog
logic a;
logic b;
assign b = a;   // ends the run
logic c;
logic d;
```

This gives two folds, `a`/`b` and `c`/`d`.

## Limits

- A declaration that starts with a user-defined type (`state_e state_q;`) does not fold. It looks the
  same as an instance to a token scan, so LazyVerilog does not guess.
- An instance does not fold as a whole. Its `#(...)` override list folds on its own.
- The parameter list and the port list share the `)(` line, so Neovim merges them into one header fold.

## Turn it off

Set `[folding].enable = false` in `lazyverilog.toml`, and `folding = false` in the Neovim
[`setup()`](../installation/neovim.md) to stop the editor asking. See
[LSP features](../lsp/index.md#folding-ranges).
