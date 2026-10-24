module n_struct;
  typedef struct { int a; byte b; } pair_t;
  pair_t p = '{a: 1, b: 2};
  pair_t arr [2] = '{'{1, 2}, '{3, 4}};
  int q [$] = {1, 2, 3};
  int aa [string] = '{"x": 1, default: 0};
  initial begin
    p = '{default: 0};
    {p.a, p.b} = {32'd1, 8'd2};
    arr = '{2{'{0, 0}}};
    q = {q, 4};
    q = {<<byte{q}};
    foreach (aa[k]) $display(k);
    p.a = arr[0].a + arr[1].b * q[$] - q[$-1];
  end
endmodule
