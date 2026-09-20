module m_lint_demo (
    input  logic       i_clk,
    input  logic       i_rst_n,
    input  logic [1:0] i_sel,
    output logic [7:0] o_q
);

    always @(posedge i_clk) begin
        if (!i_rst_n) o_q = 0;
    end

    always_comb begin
        case (i_sel)
            2'd0: o_q = 8'd1;
            2'd1: o_q = 8'd2;
        endcase
    end

endmodule
