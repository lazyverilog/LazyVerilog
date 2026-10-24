module p_blocks;
  initial begin
    fork
      #1 a = 1;
      #2 begin b = 2; end
    join
    for (int i = 0, j = 0; i < 4; i++, j++) begin end
    while (x) @(posedge clk);
    do begin x--; end while (x > 0);
    forever #5 clk = ~clk;
    repeat (2) begin : r end
    unique case (s) inside
      [0:3], 7: y = 1;
      default: begin y = 0; end
    endcase
    priority if (a) y = 1; else if (b) y = 2;
    randcase 1: y = 0; 2: y = 1; endcase
  end
  always_latch if (en) q = d;
  final $display("done");
endmodule
