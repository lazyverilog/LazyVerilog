module j_sva (input clk, a, b, c);
  sequence s1; a ##[1:3] b ##1 c[*2] ##0 (b throughout c); endsequence
  property p1(x); @(posedge clk) disable iff (!a) x |-> s_eventually b; endproperty
  ap: assert property (p1(c)) else $error("fail %0d", 1);
  cp: cover property (@(posedge clk) first_match(a ##[0:$] b));
  assume property (@(posedge clk) a |=> nexttime b);
  always @(posedge clk) begin
    assert #0 (a) else $fatal;
    if (a) begin end else begin end
  end
endmodule
