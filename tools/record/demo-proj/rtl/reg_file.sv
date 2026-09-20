module reg_file (
  input  logic        i_clk,
  input  logic [31:0] i_wdata,
  output logic [31:0] o_rs1,
  output logic [31:0] o_rs2
);

  logic [31:0] regs [32];

  always_ff @(posedge i_clk) regs[0] <= i_wdata;

  assign o_rs1 = regs[0];
  assign o_rs2 = regs[1];

endmodule
