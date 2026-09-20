module cpu_core (
  input  logic        i_clk,
  input  logic        i_rst_n,
  output logic [31:0] o_addr,
  output logic [31:0] o_wdata,
  output logic        o_valid,
  input  logic [31:0] i_rdata,
  input  logic        i_ready,
  input  logic        i_irq
);

  logic [31:0] alu_result;
  logic [31:0] rs1;
  logic [31:0] rs2;

  reg_file u_regs (
    .i_clk  (i_clk),
    .i_wdata(alu_result),
    .o_rs1  (rs1),
    .o_rs2  (rs2)
  );

  alu u_alu (
    .i_a     (rs1),
    .i_b     (rs2),
    .o_result(alu_result)
  );

  assign o_addr  = alu_result;
  assign o_wdata = rs2;
  assign o_valid = i_ready;

endmodule
