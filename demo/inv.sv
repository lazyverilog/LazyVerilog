// Simple combinational inverter.
//
// Example:
//   logic a;
//   logic y;
//
//   inv u_inv (
//       .i(a),
//       .o(y)
//   );
module inv(
    input     logic      i,
    output    logic      o
);

state_t             state                               ;
// Invert the input continuously; no clock or reset is required for this
// purely combinational RTL block.
assign o          = ~i;

endmodule
