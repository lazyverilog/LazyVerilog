# AutoWire

**Commands:** `lazyverilog.autowire`, `lazyverilog.autowirepreview`

Declares the `logic` signals the module uses but never declares. A signal counts when it is driven
in this module: connected to an instance **output** or **inout**, or assigned by an `assign` or an
`always_comb`.

```systemverilog
m_fifo u_fifo (
    .i_data  (s_data  ),  // input: not a driver, not declared
    .o_data  (s_data_o),  // output: declared
    .o_valid (s_valid )   // output: declared
);

// AutoWire inserts:
logic [7:0] s_data_o;
logic       s_valid;
```

A signal that only feeds an input is left alone, because nothing in this module drives it. Declare it
yourself. The preview command shows the declarations without applying them.

```toml
[autowire]
group_by_instance = false
sort_by_name = true
```

| Section | Option | Default | Description |
|---------|--------|---------|-------------|
| `autowire` | `group_by_instance` | `false` | Group declarations by the instance they come from |
| `autowire` | `sort_by_name` | `false` | Sort declarations alphabetically |
