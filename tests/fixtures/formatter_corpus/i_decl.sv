module i_decl;
  logic signed [7:0] a = 8'sd5, b;
  wire [3:0] c = 4'h1, d;
  int unsigned arr [$] = '{1, 2};
  bit [3:0][7:0] pk;
  string s = "a // not comment /* nor this */";
  localparam logic [1:0] L0 = 2'b01, L1 = 2'b10;
  typedef enum logic [2:0] { S0, S1 = 3'd4, S2[2], S3[5:6] } st_t;
  typedef struct packed { logic [3:0] hi; struct packed { logic a, b; } lo; } s_t;
  const int CI = 3;
  var logic vl;
  interconnect ic;
  nettype logic [3:0] my_net;
  real r = 1.5e-3;
  time t0 = 10ns;
endmodule
