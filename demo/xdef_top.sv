// Cross-file go-to-def demo top.
// Trigger go-to-def on: adder (line 8), clamp (line 13), delay_n (line 14), word_t (line 11).
// xdef_lib.sv must be indexed via vcode.f for cross-file navigation to work.

import util_pkg::*;

module xdef_top #(
    parameter int WIDTH = `LIB_WIDTH
)(
    input  logic [WIDTH-1:0] i_a,
    input  logic [WIDTH-1:0] i_b,
    output word_t            o_word,
    output logic [WIDTH-1:0] o_sum
);
    adder #(.WIDTH(WIDTH)) u_adder (
        .i_a(i_a),
        .i_b(i_b),
        .o_sum(o_sum)
    );

    always_comb begin
        o_word = word_t'(clamp(int'(o_sum), 0, 255));
        delay_n(1);
    end
endmodule
