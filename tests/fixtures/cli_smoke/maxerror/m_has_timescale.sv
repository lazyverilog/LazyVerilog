`timescale 1ns/1ps

// The single element that *does* carry a timescale.  Its presence is what
// makes slang emit MissingTimeScale for every other element in the design.
module m_has_timescale;
endmodule
