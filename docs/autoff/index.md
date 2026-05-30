# AutoFF

**Commands:** `lazyverilog.autoffPreview`, `lazyverilog.autoffApply`, `lazyverilog.autoffAllPreview`, `lazyverilog.autoffAllApply`

Generates `always_ff` blocks for signals whose names match `lint.naming.register_pattern`. For each matching signal, AutoFF creates a clocked register assignment with optional synchronous reset logic.

```systemverilog
// signal matching register_pattern = "^r_.*$"
logic [7:0] r_count;

// AutoFF generates:
always_ff @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
        r_count <= '0;
    end else begin
        r_count <= r_count;
    end
end
```

Preview commands show the generated blocks without applying them.

Controlled by the register naming pattern:

```toml
[lint.naming]
register_pattern = "^r_.*$"
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `register_pattern` | string | `"^r_.*$"` | Regex — signals matching this pattern get `always_ff` blocks generated |
