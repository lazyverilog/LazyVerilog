module tb;
`DV_EDN_IF_CONNECT `DV_ALERT_IF_CONNECT()

key_sideload_if sideload_if(.clk_i(clk), .rst_ni(rst_n), .sideload_key());
endmodule
