interface q_if #(parameter int W = 8) (input logic clk);
  logic [W-1:0] data;
  logic valid, ready;
  clocking cb @(posedge clk);
    default input #1step output #2;
    input data, valid;
    output ready;
  endclocking
  modport mst (output data, valid, input ready, clocking cb);
  modport slv (input data, valid, output ready, import task chk());
  task chk(); endtask
endinterface
module q_top (q_if.mst m, q_if s);
  assign m.data = s.data;
endmodule
