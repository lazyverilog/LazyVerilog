module arg_demo (
);
    input  logic        i_clk;
    input  logic        i_rst_n;
    input  logic [7:0]  i_a;
    input  logic [7:0]  i_b;
    output logic [7:0]  o_sum;

    assign o_sum = i_a + i_b;

endmodule
