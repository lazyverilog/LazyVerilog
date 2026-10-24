module k_wrapedge;
  initial begin
    if (a) begin
      if (b) begin
        if (c) begin
          long_function_name_x(alpha_alpha_alpha, beta_beta_beta, gamma_gamma_gamma_g);
          long_function_name_x(alpha_alpha_alpha, beta_beta_beta, gamma_gamma_gamma_gg);
          x = long_function_name_x(alpha_alpha, beta_beta_beta, gamma_gamma_gamma_gg);
          x <= outer_call(inner_call(alpha_alpha_alpha, beta_beta), gamma_gamma_gamma_ggg);
        end
      end
    end
  end
  assign w = sel ? fn(aaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbbb, ccccccccccccccc, ddddddddddddddddd) : 0;
  assign {carry, sum} = add_with_carry(operand_a_long_name, operand_b_long_name, carry_in_x);
endmodule
