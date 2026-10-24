module o_v95 (clk, d, q, en);
input clk;
input [7:0] d;
output [7:0] q;
input en;
reg [7:0] q;
always @(posedge clk)
  if (en)
    begin
      q <= d;
    end
  else
    q <= q;
function [7:0] inc;
  input [7:0] v;
  begin
    inc = v + 1;
  end
endfunction
task automatic t;
  input integer n;
  output reg o;
  begin
    o = n;
  end
endtask
endmodule
