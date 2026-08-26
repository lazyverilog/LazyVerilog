// Demo for lazyverilog-clk. Run:
//   lazyverilog-clk -f demo2/vcode.f clk_demo
//
// Port shapes:
//   clk_a, clk_b : edge sources
//   rst_n        : async reset, reported alongside every clock it gates
//   a_i          : captured by the clk_a flop
//   b_i          : captured by the clk_b flop
//   thru_i       : never reaches a flop
//   sum_o        : driven by the clk_a flop
//   xor_o        : driven through comb by the clk_b flop
//   thru_o       : combinational feedthrough
module clk_demo (
    input  logic clk_a,
    input  logic clk_b,
    input  logic rst_n,

    input  logic [7:0] a_i,
    input  logic [7:0] b_i,
    input  logic       thru_i,

    output logic [7:0] sum_o,
    output logic       xor_o,
    output logic       thru_o
);

    logic [7:0] a_q;
    logic       b_q;

    always_ff @(posedge clk_a or negedge rst_n) begin
        if (!rst_n) a_q <= '0;
        else        a_q <= a_i;
    end

    always_ff @(posedge clk_b) begin
        b_q <= b_i[0];
    end

    assign sum_o  = a_q;
    assign xor_o  = b_q ^ 1'b1;
    assign thru_o = thru_i;

endmodule
