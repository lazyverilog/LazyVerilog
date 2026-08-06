module adc_ctrl_reg_top(
    input                            clk_i                      ,
    input                            rst_ni                     ,
    input     tlul_pkg::tl_h2d_t     tl_i                       ,
    output    tlul_pkg::tl_d2h_t     tl_o, // To HW
    output    adc_ctrl_reg_pkg::adc_ctrl_reg2hw_t reg2hw, // Write
    input     adc_ctrl_reg_pkg::adc_ctrl_hw2reg_t hw2reg, // Read
    // Integrity check errors\n"
    output    logic                  intg_err_o
);
endmodule
