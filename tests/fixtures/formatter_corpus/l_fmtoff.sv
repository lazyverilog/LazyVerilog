module l_fmtoff;
  // verilog_format: off
  assign   a=b;
  // verilog_format: on
  assign c=d;
  always_comb begin
    // verilog_format: off
      x   =   1;
    // verilog_format: on
    y=2;
  end
  logic  p;   // verilog_format: off
  logic   q;
  // verilog_format: on
  logic r;


  /* block
       comment */
  logic s;
endmodule
