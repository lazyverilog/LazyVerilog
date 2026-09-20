module fold_demo (
    input  logic       i_clk,
    input  logic       i_rst_n,
    input  logic [7:0] i_a,
    output logic [7:0] o_q
);

    logic [7:0] r_q;

    always_ff @(posedge i_clk or negedge i_rst_n) begin
        if (!i_rst_n) begin
            r_q <= '0;
        end else begin
            r_q <= i_a;
        end
    end

    function automatic logic [7:0] inc(input logic [7:0] v);
        return v + 8'd1;
    endfunction

    always_comb begin
        o_q = inc(r_q);
    end

endmodule
