`define MAX(a, b) \
  ((a) > (b) ? (a) : (b))
module c_pp (
`ifdef HAS_CLK
  input logic clk,
`endif
  input logic [`MAX(3, 4):0] d,
  output logic q
);
  always_comb begin
    if (d[0])
      q = 1;
`ifdef ALT
    else if (d[1])
      q = 0;
`endif
    else
      q = d[2];
  end
  always_comb
    case (d)
`ifdef X
      1: q = 0;
`endif
      default: q = 1;
    endcase
  `MY_STMT(q)
  `MY_TYPE xx;
endmodule
