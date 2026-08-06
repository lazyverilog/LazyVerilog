module memory_top(
    input                            i_clk                      ,
    input     packet_t               i_data                     ,
    // test
    input     packet_t               i_div        [1:0][1:0]    ,
    output    logic      [1:0]       o_mul
);
`include "params.svh"

parameter int DEPTH = 8;

parameter int ADDR_W = 10;
parameter int DEPTH = 2 ** ADDR_W; // hover shows "int = 2 ** ADDR_W", no 1024
state_t                                 state                               ;

logic               [cpu_pkg::WIDTH2-1:0] data                              ;
`define BB 3
logic               [LOOOOOOOOOOOOOONG_PARAM-1:0] mem2_to_mem3              ;
looooooooooooog_packet_t [`BB-1:0]      mem3_to_mem4        [1:0] = 33333333333333;
logic               [`BB-1:0]           mem4_to_mem5                        ;

always_ff @ ( posedge clk ) begin
end
memory u_mem2 (
    .i_clk              (i_diveeeeeeeee                ),
    .address            (                              ),
    .i_data             (i_data                        ),
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
    .o_test             (bridge_wir                    ),
    .o_data             (asdfdsahfasdf                 )
);
memory u_mem4 (
    .i_clk              (                              ),
`ifdef FOO
    .address            (                              ),
    .i_data             (mem2_to_mem3                  ),
`elsif BAR
    .o_data             (eqoo                          ),
`endif
    .o_test             (bridge_wir                    )
);

logic               [3:0]               arr                                 ;
fifo_entry_t                            fifo_entry                          ;
logic                                   valid                               ;
always_comb begin
    valid      = fifo_entry.valid;
    Packet                                  p                                   ;
    if (1) begin
        a          = - 3;
    end
    else if (1) begin
        a          = 3;
    end
    else
        a          = 3;
    p.req_data();
    b          = 3;
    arr[3]     = 3;
    arr[b]     = 3;
    arr[a]     = 3;
    arr[a.va]  = 3;

    case (r_state)
        1: begin
        end
    endcase
end

initial begin
    fork
        begin
            a          = 1;
        end
    join
end
logic                                   a                                   , r_a                                 ;
// test
logic                                   a                                   , r_a                                 ;
logic                                   b                                   , r_b                                 ;

logic                                   b                                   , r_b                                 ;
logic                                   c                                   , r_c                                 ;
logic                                   d                                   , r_d                                 ;
logic               [WIDTH-1:0][2:0]    asdfdsahfasdf                       ;
// test

always_ff @ ( posedge clk or negedge rst_n ) begin: ff_generation
    if (!rst_n) begin
        r_a        <= '0;
        r_b        <= '0;
        r_c        <= '0;
    end
    else begin
        r_a        <= a;
        r_b        <= b;
        r_c        <= c;
    end
end

endmodule
