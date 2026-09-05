# AutoWire

**Commands:** `lazyverilog.autowire`, `lazyverilog.autowirepreview`

Generates missing `logic` declarations for undeclared signals in the enclosing module. It uses output/inout module-instantiation port connections, continuous-assignment left-hand sides, and `always_comb` assignment left-hand sides as declaration sources.

```systemverilog
// instantiation references s_data, s_data_o and s_valid, but they are not declared
m_fifo u_fifo (
    .i_data  (s_data  ),  // input  -> not a declaration source
    .o_data  (s_data_o),  // output -> declared
    .o_valid (s_valid )   // output -> declared
);

// AutoWire inserts:
logic [7:0] s_data_o;
logic       s_valid;
```

A signal connected only to an **input** port is left alone on purpose: AutoWire
declares nets that something in this module drives, and an input connection is a
read, not a driver. Declare those yourself, or drive them from an output first.

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
