module wire_demo (
    input  logic        i_clk,
    input  logic [31:0] i_wdata,
    output logic [31:0] o_sum
);

    reg_file u_regs (
        .i_clk  (i_clk),
        .i_wdata(i_wdata),
        .o_rs1  (rs1),
        .o_rs2  (rs2)
    );

    alu u_alu (
        .i_a     (rs1),
        .i_b     (rs2),
        .o_result(o_sum)
    );

endmodule
