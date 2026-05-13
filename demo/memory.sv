module memory #(parameter int WIDTH = 4, parameter int DEPTH = 8) (
    i_clk, address, data_in,
    data_out, read_write, chip_en,
    www333, zzfuk
);
`include "params.svh"

localparam MEM_SIZE = 8;
input                                          i_clk                                   ;
input  wire                [5:0]               address                                 ;
input[MEM_SIZE-1:0] data_in;
output logic               [7:0]               data_out                                ;
input  wire                                    read_write                              , chip_en                                 ;
output                                         fifo_entry_t        [3:0] www333        ;
input                                          fifo_entry_t        [3:0] www333        ;
output                                         fifo_entry_t        [3:0] zzfuk         ;
input                                          fifo_entry_t        [3:0] zzfuk         ;

reg                 [7:0]               mem                 [0:255]         ;

always @(address or data_in or read_write or chip_en) if (read_write == 1 && chip_en == 1) begin
    mem[address]               = data_in;
    mem[address]  /* test tset <= */ = data_in;
    a_b                        = 3;
    /*test comment block       = */
end

always @(read_write or chip_en or address) if (read_write == 0 && chip_en) data_out = mem[address];
else data_out                                                                       = 0;

always @(posedge i_clk  /* FIXME*/) begin
end

inv u_inv (
    .i_a       (zzfuk     ),
    .i_d       (          ),
    .o_d       (zzfuk     )
);
endmodule
