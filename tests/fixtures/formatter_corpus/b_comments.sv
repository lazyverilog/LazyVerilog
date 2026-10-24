module b_comments (
  input logic a, // first
  /* mid */ input logic b,
  output logic y /* last */
);
  assign y = a /* and */ & b;
  always_comb begin
    if /* why */ (a) y = 1;
    else // other
      y = 0;
  end
  logic [7:0 /* hi */] v;
  initial begin
    #10 /* wait */ v = 1;
    v = 2 /* two */ ;
  end
  always @(posedge a) begin : blk
  end /* end blk */ : blk
endmodule // tail
