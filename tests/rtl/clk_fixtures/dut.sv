// Fixture for lazyverilog-clk.  Every port exercises a different shape of
// clock-domain trace; the expected table is asserted in tests/clk_cli_smoke.cpp
// and shown in docs/clock-domain/cli.md.
module dut (
    input  logic clk_a,
    input  logic clk_b,
    input  logic rst_n,

    input  logic data_i,    // comb, then a clk_a flop
    input  logic direct_i,  // straight into a clk_b flop
    input  logic dual_i,    // fans out to both clk_a and clk_b flops
    input  logic thru_i,    // never reaches a flop
    input  logic ff_i,      // captured by a flop inside a child instance

    output logic data_o,    // driven by a clk_a flop
    output logic comb_o,    // driven by comb off a clk_b flop
    output logic thru_o,    // combinational feedthrough
    output logic sub_o,     // driven through a child instance off a clk_a flop
    output logic ff_o       // driven by a flop inside a child instance
);

    logic pre_a;
    logic ff_a;
    logic ff_b;
    logic sub_y;

    assign pre_a = data_i & dual_i;

    always_ff @(posedge clk_a or negedge rst_n) begin
        if (!rst_n)
            ff_a <= 1'b0;
        else
            ff_a <= pre_a;
    end

    always_ff @(posedge clk_b) begin
        ff_b <= direct_i | dual_i;
    end

    comb_xor u_xor (
        .a(ff_a),
        .b(1'b0),
        .y(sub_y)
    );

    // The flop lives inside this child and is clocked by the child's own `clk`
    // port; both ff_i and ff_o must still report clk_b, the parent's net.
    sub_ff u_ff (
        .clk(clk_b),
        .d  (ff_i),
        .q  (ff_o)
    );

    assign data_o = ff_a;
    assign comb_o = ff_b ^ 1'b1;
    assign thru_o = thru_i;
    assign sub_o  = sub_y;

endmodule
