`include "params.svh"

`define AA 3
`define BB 4
module memory #(
    parameter int WIDTH = 4,
    parameter int DEPTH = 8
)(
    o_new_mem_port, i_new_mem_port, i_clk,
    address, i_data, i_test,
    o_data, o_test
);
output    logic                  o_new_mem_port                  ;
input     logic                  i_new_mem_port                  ;

input     logic                  i_clk                           ;
input     wire       [5:0]       address                    ;
input     wire       [`AA-1:0]   i_data                     ;
input     wire       [`AA-1:0]   i_test                     ;
output    wire       [`BB-1:0]   o_data                     ;
output    wire       [`BB-1:0]   o_test                     ;

state_t             state                               ;
inv u_inv (
    .o             (o_test                        ),
    .i             (i_test                        ),
    .o_inv_test    (o_data                        ),
    .i_inv_test    (i_data                        ),
    .o_new_inv_port(o_new_mem_port                ),
    .i_new_inv_port(i_new_mem_port                )
);
always_comb begin
    state      = state_t::IDLE;
end
endmodule
