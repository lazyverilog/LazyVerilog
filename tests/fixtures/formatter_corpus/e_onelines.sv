module e_onelines(input clk, rst, output reg [3:0] cnt); always @(posedge clk or posedge rst) if (rst) cnt <= 0; else if (cnt == 4'hf) cnt <= 0; else cnt <= cnt + 1; endmodule
module e2; initial begin : b fork : f begin #1; end join : f end : b endmodule
module e3; function automatic int sq(int a); return a*a; endfunction task t; endtask initial repeat (3) @(posedge clk) t(); endmodule
