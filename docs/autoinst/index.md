# AutoInst

**Code action.** Fills in the named port connections of a module instance. Put the cursor on an
instance with missing or incomplete connections.

```systemverilog
// before
m_fifo u_fifo ();

// after AutoInst
m_fifo u_fifo (
    .i_clk   (i_clk  ),
    .i_rst_n (i_rst_n),
    .i_data  (i_data ),
    .o_data  (o_data )
);
```

The module must be defined in the current file or the [filelist](../design/index.md). Connections are
always named (`.port(signal)`). No configuration.
