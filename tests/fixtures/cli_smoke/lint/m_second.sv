module m_second(
    input  logic i_clk,
    input  logic address,
    output logic o_out
);
assign o_out = i_clk & address;
endmodule
