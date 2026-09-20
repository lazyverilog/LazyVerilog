module mem_ctrl (
  input  logic        i_clk,
  input  logic [15:0] i_addr,
  input  logic        i_we,
  output logic [31:0] o_rdata,
  output logic        o_irq
);

  logic [31:0] mem [1024];

  always_ff @(posedge i_clk) if (i_we) mem[i_addr[9:0]] <= 32'h0;

  assign o_rdata = mem[i_addr[9:0]];
  assign o_irq   = 1'b0;

endmodule
