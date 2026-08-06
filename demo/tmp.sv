module tmp();

logic                                   a                                   , r_a                                 ;
always_comb begin
    case (r_state)
        ST_RX_D_TO_C_TEST_RX_INIT_D_TO_C_WAIT_REMOTE_ACTION : begin
            if (receive_message(.i_sb_rx_data_valid(i_sb_rx_data_valid),
                                .i_sb_rx_data(i_sb_rx_data),
                                .o_sb_rx_data_ready(sb_rx_data_ready)) == 1) begin
                for (int lane = 0 ; lane < N_MB_LANE ; lane = lane + 1) begin
                    if (rx_inferred_ledge_per_lane_found[lane] & rx_inferred_redge_per_lane_found[lane]) begin
                        rx_inferred_center_per_lane[lane] = (rx_inferred_ledge_per_lane[lane] + rx_inferred_redge_per_lane[lane]) >> 1;
                    end // only left edge was found
                    else if (rx_inferred_ledge_per_lane_found[lane]) begin
                        rx_inferred_center_per_lane [lane] = (rx_inferred_ledge_per_lane[lane] + pi_code_t'('1)) >> 1;
                    end // left edge was not found
                    else begin
                        rx_inferred_center_per_lane[lane] = pi_code_t'('1);
                    end
                end // Parse {Rx Init D to C sweep done with results}
                left_edge  = i_sb_rx_data.data0[7:0];
                right_edge = i_sb_rx_data.data0[15:8];
                state      = ST_RX_D_TO_C_TEST_END_RX_INIT_D_TO_C_EYE_SWEEP_REQ;
            end
        end
    endcase
end
always_ff @ ( posedge clk or negedge rst_n ) begin
    if (!rst_n) begin
        r_a        <= '0;
    end
    else begin
        r_a        <= a;
    end
end

endmoudle
