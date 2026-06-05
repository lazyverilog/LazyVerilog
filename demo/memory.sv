`include "params.svh"

`define AA 3
`define BB 4
module memory #(
    parameter int WIDTH = 4,
    parameter int DEPTH = 8
)(
    i_clk, address, i_data,
    o_data
);

input     logic                  i_clk                           ;
input     wire       [5:0]       address                    ;
input     wire       [`AA-1:0]   i_data                     ;
output    wire       [`BB-1:0]   o_data                     ;

state_t             state                               ;
inv u_inv (
    .o        (o_data    ),
    .i        (i_data    )
);
always_comb begin
    state      = state_t::IDLE;
end
endmodule
