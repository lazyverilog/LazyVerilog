// The diagnostic under test: `foo` declares a `type_a` return but returns a
// `type_b`.  slang reports it as an implicit-conversion warning, but only if
// elaboration is allowed to run far enough to reach the function body.
package pkg_maxerror;
  typedef struct packed { logic [7:0]  a; } type_a;
  typedef struct packed { logic [15:0] b; } type_b;
endpackage

module m_dut;
  import pkg_maxerror::*;

  function automatic type_a foo();
    type_b ret;
    return ret;
  endfunction

  type_a r;
  initial r = foo();
endmodule
