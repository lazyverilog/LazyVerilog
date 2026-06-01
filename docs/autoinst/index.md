# AutoInst

**Code action**

Generates a full named port connection list for a module instantiation. Triggered as a code action when the cursor is on an instantiation with missing or incomplete port connections.

```systemverilog
// before (incomplete)
m_fifo u_fifo ();

// after AutoInst
m_fifo u_fifo (
    .i_clk   (i_clk  ),
    .i_rst_n (i_rst_n),
    .i_data  (i_data ),
    .o_data  (o_data )
);
```

Requires the instantiated module definition to be available from the current file or the design filelist (`design.vcode`).

AutoInst currently emits named `.port(signal)` connections.

No dedicated configuration.
