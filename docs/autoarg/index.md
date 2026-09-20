# AutoArg

**Code action and on-save.** Regenerates a non-ANSI module's port list from the port declarations in
its body.

```systemverilog
module m_alu(
    i_clk, i_rst_n, i_a, i_b, o_result   // regenerated from the declarations below
);
    input  logic        i_clk;
    input  logic        i_rst_n;
    input  logic [7:0]  i_a;
    input  logic [7:0]  i_b;
    output logic [7:0]  o_result;
```

As a code action it applies to the module under the cursor. With `autoarg_on_save` it runs on every
module in the file on save. The result is formatted with your `[format]` and `[format.module]`
settings, including the non-ANSI ports-per-line options.

```toml
[autoarg]
autoarg_on_save = true
```

| Section | Option | Default | Description |
|---------|--------|---------|-------------|
| `autoarg` | `autoarg_on_save` | `false` | Regenerate port lists for every module in the file on save |
