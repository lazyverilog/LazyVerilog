module m_top(
    input  logic i_clk,
    output logic o_out
);
logic mid;
m_leaf u_leaf_a (
    .i_clk  (i_clk),
    .o_out  (mid)
);
m_leaf u_leaf_b (
    .i_clk  (mid),
    .o_out  (o_out)
);
endmodule
