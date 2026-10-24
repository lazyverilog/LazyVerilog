module a_nest #(parameter int W = 8, parameter type T = logic [W-1:0]) (input logic clk, input T d, output T q);
  T r [2];
  always_ff @(posedge clk) begin
    r[0] <= cond ? '{default: '0} : r[1];
    q <= (d inside {[8'h0:8'h7], 8'hff}) ? {<<8{d}} : {>>{r[0]}};
    r[1] <= f(g(d, 1), '{a: 1, b: h(2)}, cond ? x : y);
  end
  generate
    for (genvar i = 0; i < 2; i++) begin : g_l
      case (i)
        0: begin : g_c0
          fork
            begin end
          join_none
        end
        default: ;
      endcase
    end
  endgenerate
endmodule
