// Wrapper so `dut` is reached through a real hierarchical path (top.u_dut)
// rather than being elaborated as a top-level module itself.
module top (
    input  logic clk_a,
    input  logic clk_b,
    input  logic rst_n,
    input  logic data_i,
    input  logic direct_i,
    input  logic dual_i,
    input  logic thru_i,
    input  logic ff_i,
    output logic data_o,
    output logic comb_o,
    output logic thru_o,
    output logic sub_o,
    output logic ff_o
);

    dut u_dut (
        .clk_a   (clk_a),
        .clk_b   (clk_b),
        .rst_n   (rst_n),
        .data_i  (data_i),
        .direct_i(direct_i),
        .dual_i  (dual_i),
        .thru_i  (thru_i),
        .ff_i    (ff_i),
        .data_o  (data_o),
        .comb_o  (comb_o),
        .thru_o  (thru_o),
        .sub_o   (sub_o),
        .ff_o    (ff_o)
    );

endmodule
