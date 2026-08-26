// Sequential leaf.  Its flop is clocked by its own `clk` port, so tracing a
// parent port through it only produces a useful answer if the clock name is
// mapped back out to whatever net the parent connected.
module sub_ff (
    input  logic clk,
    input  logic d,
    output logic q
);
    always_ff @(posedge clk) q <= d;
endmodule
