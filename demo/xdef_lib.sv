// Cross-file go-to-def demo library.
// Listed in vcode.f so the LSP indexes it as a project file.
// Open xdef_top.sv and trigger go-to-def on: adder, clamp, delay_n, word_t.

`define LIB_WIDTH 8

typedef logic [7:0] byte_t;

package util_pkg;
    typedef logic [15:0] word_t;

    function automatic int clamp(input int val, input int lo, input int hi);
        return val < lo ? lo : val > hi ? hi : val;
    endfunction

    task delay_n(input int n);
        repeat (n) #1;
    endtask
endpackage

module adder #(
    parameter int WIDTH = 8
)(
    input  logic [WIDTH-1:0] i_a,
    input  logic [WIDTH-1:0] i_b,
    output logic [WIDTH-1:0] o_sum
);
    assign o_sum = i_a + i_b;
endmodule
