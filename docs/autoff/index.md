# AutoFF

**Commands:** `lazyverilog.autoffPreview`, `lazyverilog.autoffApply`, `lazyverilog.autoffAllPreview`,
`lazyverilog.autoffAllApply`

Adds the missing assignments for a register to an existing `always_ff` block. Put the cursor on a
declaration of exactly two signals: one whose name matches `register_pattern` (the register) and one
that does not (its source).

```systemverilog
logic [7:0] r_count, w_count;   // r_count is the register, w_count its source

always_ff @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
        // AutoFF inserts: r_count <= '0;
    end else begin
        // AutoFF inserts: r_count <= w_count;
    end
end
```

The first branch is treated as reset and the `else` branch as capture, whatever the clock and reset
are called. A signal already assigned is skipped. The preview is shown in a confirmation window in
Neovim. The `All` commands do every qualifying declaration in the file at once.

Requirements:

- The `always_ff` must already exist and have an `if (...) begin ... end else begin ... end` body.
- The declaration must have exactly one register-named signal. Ambiguous lines are skipped.

```toml
[autoff]
register_pattern = "^r_"
```

| Option | Default | Description |
|--------|---------|-------------|
| `register_pattern` | `"^r_"` | Regex. The signal matching it is the register, the other is the source |
