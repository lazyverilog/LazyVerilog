module register #(
    parameter type T=logic [7:0],
    parameter int DEPTH=8,
    parameter int SIZE=4,
    parameter logic [7:0] TABLE[SIZE]='{8'h00, 8'h11, 8'h22, 8'h33}
)(
    input     logic      clk,
    input     logic      rst_n,
    input     T          d,
    output    T          q
);

always_ff @ ( posedge clk or negedge rst_n ) begin
    if (!rst_n)
        q      <= '0;
    else
        q      <= d;
end

endmodule
