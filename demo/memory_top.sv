`include "params.svh"

module memory_top(
    input     looooooooong_t [LOOOOOOOOOONG:0] i_clk        [1:0]         ,
    input     packet_t   [1:0]       i_diveeeeeeeee [1:0][3:0][1:0][3:1],
    output    logic      [1:0]       o_mul        [1:0][3:0][1:0][3:1]
);

parameter int DATA_WIDTH = 32;

logic               [`BB-1:0]           mem2_to_mem3                        ;
looooooooooooog_packet_t [`BB-1:0]      mem3_to_mem4        [1:0] = 33333333333333;
logic               [`BB-1:0]           mem4_to_mem5                        ;

memory u_mem2 (
    .i_clk              (                              ),
    .address            (                              ),
    .i_data             (                              ),
    .o_data             (mem2_to_mem3                  )
);

memory u_mem3 (
    .i_clk              (                              ),
`ifdef FOO
    .address            (                              ),
    .i_data             (mem2_to_mem3                  ),
`elsif BAR
    .o_data             (eqoo                          ),
`endif
    .o_test             (bridge_wir                    )
);

always_comb begin
    Packet              p                                   ;
    if (1) begin
        add_number();
    end
    p.req_data();
end

endmodule
