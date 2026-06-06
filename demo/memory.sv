`include "params.svh"

`define AA 3
`define BB 4
module memory #(
    parameter WIDTH = 8
)(
    i_clk, address, i_data,
    o_data
);

input                            i_clk                           ;
input                [5:0]       address                         ;
input     wire       [WIDTH-1:0] i_data            [1:0]         ;
output    logic      [WIDTH-1:0] o_data            [2:0]         ;
endmodule
