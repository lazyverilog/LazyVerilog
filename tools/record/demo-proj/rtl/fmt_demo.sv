module fmt_demo(input logic clk,input logic rst_n,input logic[7:0] a,input logic [7:0] b,output logic [7:0] sum);
logic [7:0] r_sum;
always_ff@(posedge clk or negedge rst_n) begin
if(!rst_n) r_sum<=0;
else begin
r_sum<=a+b;
end
end
assign sum=r_sum;
endmodule
