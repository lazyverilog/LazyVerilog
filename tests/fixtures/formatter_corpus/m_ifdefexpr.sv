module m_ifdefexpr (input a, b, output y);
  assign y = a &
`ifdef INV
    ~b
`else
    b
`endif
    ;
  always_comb begin
    case (a)
      1'b0: begin
`ifdef DBG
        $display("zero");
`endif
      end
      default:
        y = 0;
    endcase
  end
  `ifdef INDENTED
    logic z;
  `endif
endmodule
