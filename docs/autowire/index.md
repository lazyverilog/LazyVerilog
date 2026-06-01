# AutoWire

**Commands:** `lazyverilog.autowire`, `lazyverilog.autowirepreview`

Generates missing `logic` declarations for undeclared signals in the enclosing module. It uses output/inout module-instantiation port connections, continuous-assignment left-hand sides, and `always_comb` assignment left-hand sides as declaration sources.

```systemverilog
// instantiation references s_data and s_valid, but they are not declared
m_fifo u_fifo (
    .i_data  (s_data ),
    .o_valid (s_valid)
);

// AutoWire inserts:
logic [7:0] s_data;
logic       s_valid;
```

The preview command shows the generated declarations without applying them.

```toml
[autowire]
group_by_instance = false
sort_by_name = true
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `group_by_instance` | bool | `false` | Group generated declarations by the instance they come from |
| `sort_by_name` | bool | `false` | Sort generated declarations alphabetically |
