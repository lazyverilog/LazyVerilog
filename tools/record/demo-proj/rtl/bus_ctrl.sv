module bus_ctrl (
  input  logic        i_clk,
  input  logic        i_rst_n,
  input  logic [31:0] i_addr,
  input  logic [31:0] i_wdata,
  input  logic        i_valid,
  output logic [31:0] o_rdata,
  output logic        o_ready,
  output logic [15:0] o_mem_addr,
  output logic        o_mem_we,
  input  logic [31:0] i_mem_rdata
);

  assign o_mem_addr = i_addr[15:0];
  assign o_mem_we   = i_valid;
  assign o_rdata    = i_mem_rdata;
  assign o_ready    = i_valid;

endmodule
