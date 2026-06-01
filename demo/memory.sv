`include "params.svh"

module memory #(
    parameter int WIDTH = 4,
    parameter int DEPTH = 8
)(
    i_clk, address, i_data
);

input     logic                  i_clk                           ;
input     wire       [5:0]       address                    ;
input     wire       [5:0]       i_data                     ;

inv u_inv();
endmodule
