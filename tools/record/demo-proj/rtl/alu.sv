module alu (
  input  logic [31:0] i_a,
  input  logic [31:0] i_b,
  output logic [31:0] o_result
);

  assign o_result = i_a + i_b;

endmodule
