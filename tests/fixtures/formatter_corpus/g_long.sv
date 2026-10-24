module g_long;
  initial begin
    some_func_name(argument_one, argument_two); // exactly long
    result_value = compute(alpha_value, beta_value) + compute(gamma_value, delta_value) - offset_x;
    obj.method_a().method_b(arg1, arg2).method_c(arg3);
    $display("%0d %0d %0d", a, b, c);
    if (a) $display("x"); else $display("y", z);
    q.push_back(new_item(.id(1), .data(2)));
    data = cond1 ? func_a(p1, p2, p3) : cond2 ? func_b(p4, p5) : func_c(p6, p7, p8, p9);
  end
endmodule
