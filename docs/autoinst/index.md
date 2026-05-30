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

Requires the instantiated module to be present in the design index (`design.vcode`).

Port connection style follows `[lint.module] module_instantiation_style`.

No dedicated configuration.
