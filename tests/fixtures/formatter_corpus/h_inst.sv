module h_inst;
  sub #(.W(8), .D(4)) u0 (.clk, .d(din[7:0]), .q({a, b}), .*);
  sub #(8) u1 [3:0] (clk, , q1);
  sub u2 (
    .clk(clk), // clock
    // .rst(rst),
    .d(cond ? a : b),
    .q()
  );
  and #(1, 2) g1 (o1, i1, i2), g2 (o2, i3, i4);
  assign #(1:2:3) w = ~a;
  bind sub chk u_chk (.a(clk));
endmodule
