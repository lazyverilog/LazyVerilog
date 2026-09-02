`include "params.svh"

module hpc_block_32 #(
    parameter int unsigned WIDTH_A = ADDR_WIDTH_416,
    parameter int unsigned WIDTH_B = DATA_WIDTH_928
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,
    input  logic [WIDTH_A-1:0]    data_i,
    output logic [WIDTH_B-1:0]    data_o,
    output logic                  valid_o
);

    logic [WIDTH_A-1:0] data_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) data_q <= '0;
        else         data_q <= data_i;
    end

    assign data_o  = data_q[WIDTH_B-1:0];
    assign valid_o = |data_q;

endmodule
