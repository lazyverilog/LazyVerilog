`include "params.svh"

module memory_top #(
    parameter int WIDTH = 4,
    parameter int DEPTH = 8
)(
    i_clk, i_rst_n, i_data,
    i_data2, i_data3, i_dd,
    i_dd22222, dd22222, i_d33333,
    i_d44333, i_dd44321, i_d44334,
    VDD, VSS
);
logic               bridge_wire                         ;

memory #(.DEPTH(4), .WIDTH(3)) u_mem2 (
    .i_clk              (                              ),
    .address            (                              ),
    .i_data             (eqoo                          ),
    .o_data             (                              ),
    .i_test             (bridge_wire                   )
);

memory u_mem3 (
    .i_clk              (                              ),
    .address            (                              ),
    .i_data             (                              ),
    .o_data             (eqoo                          ),
    .o_test             (bridge_wire                   )
);

endmodule
