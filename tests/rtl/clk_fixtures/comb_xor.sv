// Purely combinational leaf, used to check that a comb path traced through a
// child instance boundary keeps going into the parent's flops.
module comb_xor (
    input  logic a,
    input  logic b,
    output logic y
);
    assign y = a ^ b;
endmodule
