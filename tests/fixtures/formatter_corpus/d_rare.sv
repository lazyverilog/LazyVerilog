module d_rare;
  let max2(a, b) = (a > b) ? a : b;
  logic [7:0] x, y;
  alias x = y;
  typedef union tagged { void Invalid; int Valid; } vi_t;
  vi_t v;
  initial begin
    v = tagged Valid 5;
    if (v matches tagged Valid .n) $display(n);
    case (v) matches
      tagged Invalid: ;
      tagged Valid .n: $display(n);
    endcase
  end
  export "DPI-C" function f;
  import "DPI-C" context function int c_f(input int a);
  function int f(); return 0; endfunction
  covergroup cg @(posedge x[0]);
    cp: coverpoint x { bins lo = {[0:3]}; bins hi[] = {[4:$]}; }
    cr: cross cp, y { ignore_bins ig = binsof(cp.lo) && binsof(y) intersect {0}; }
  endgroup
endmodule
program automatic p_t (input logic clk);
  initial begin @(posedge clk); $finish; end
endprogram
checker chk (logic a, event clk);
  a1: assert property (@clk a);
endchecker
