module m_top(
    input  logic i_clk,
    input  logic address,
    output logic o_out
);
logic [`UNKNOWN_MACRO-1:0] data;
assign o_out = i_clk;
assign data = 0;
endmodule
