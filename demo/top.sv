module top;

logic               [DEPTH-1:0]         addr                                ;
logic               [cpu_pkg::WIDTH2-1:0] data                              ;
cpu_pkg::opcode_t o;
cpu_pkg::packet_cfg cfg;
endmodule
