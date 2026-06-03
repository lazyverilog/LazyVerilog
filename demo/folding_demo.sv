// ============================================================
// folding_demo.sv — manual folding range demo for lazyverilog
//
// Open this file in Neovim with lazyverilog attached, then use:
//   zM  close all folds
//   zR  open all folds
//   za  toggle fold under cursor
//
// Notes:
// - FoldingRange does not rewrite RTL. The "expected output" is the set of
//   foldable regions that the editor receives from textDocument/foldingRange.
// - Line numbers shown below are intentionally omitted because this file is a
//   manual demo and comments may move. Each section labels the constructs that
//   should fold.
// ============================================================

// ============================================================
// 01. Import run folding
// Expected:
//   - These three consecutive top-level package imports fold together as
//     one "imports" region.
// ============================================================

import folding_pkg_a::*;
import folding_pkg_b::item_b;
import folding_pkg_c::*;

// ============================================================
// 02. `celldefine / `endcelldefine folding
// Expected:
//   - The whole `celldefine region folds.
//   - Each module inside it also folds independently.
// ============================================================

`celldefine
module folding_cell_a;
endmodule

module folding_cell_b;
endmodule
`endcelldefine

// ============================================================
// 03. Class, constraint, and covergroup folding
// Expected:
//   - class ... endclass folds.
//   - constraint block folds.
//   - covergroup ... endgroup folds.
// ============================================================

class folding_packet;
    rand bit [7:0] addr;
    rand bit [7:0] data;

    constraint addr_c {
        addr inside {[8'h10:8'h1f]
        };
        data != 8'h00;
    }

    covergroup addr_cg;
        coverpoint addr {
            bins low   = {[8'h00:8'h0f]};
            bins high  = {[8'hf0:8'hff]};
        }
    endgroup
endclass

// ============================================================
// 04. Multiline module parameter list and port list folding
// Expected:
//   - module declaration folds from module to endmodule.
//   - parameter port list folds.
//   - ANSI port list folds.
// ============================================================

module folding_demo #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 16,
    parameter int STAGES = 3
)(
    input     logic                   clk,
    input     logic                   rst_n,
    input     logic [WIDTH-1:0]       data_in,
    input     logic                   valid_in,
    output    logic [WIDTH-1:0]       data_out,
    output    logic                   valid_out
);

logic               [WIDTH-1:0]         pipe_data[STAGES]                   ;
logic                                   pipe_vld[STAGES]                    ;
logic               [WIDTH-1:0]         selected_data                       ;

// ============================================================
// 05. Consecutive own-line comment folding
// Expected:
//   - The three own-line comments below fold together as one "comment" region.
// ============================================================

// Own-line comment fold line 1.
// Own-line comment fold line 2.
// Own-line comment fold line 3.

// ============================================================
// 06. Trailing comments must not fold code lines
// Expected:
//   - The trailing comment on the assign line below must not join the own-line
//     comment after it. Folding comments must never hide the assign statement.
// ============================================================

assign selected_data = pipe_data[STAGES-1]; // trailing comment belongs to RTL
// This own-line comment should not fold together with the assign line above.

// ============================================================
// 07. Multiline block comment folding
// Expected:
//   - This block comment folds as one "comment" region because it starts on
//     its own line.
// ============================================================

/*
 * Block comment fold line 1.
 * Block comment fold line 2.
 * Block comment fold line 3.
 */

// ============================================================
// 08. begin/end, if/else, generate, and nested folding
// Expected:
//   - always_ff begin/end folds.
//   - nested if/else begin/end blocks fold independently.
//   - generate/endgenerate folds.
//   - for-generate begin/end folds.
// ============================================================

always_ff @ ( posedge clk or negedge rst_n ) begin
    if (!rst_n) begin
        pipe_data[0] <= '0;
        pipe_vld[0] <= 1'b0;
    end
    else begin
        pipe_data           [0] <=              data_in                             ;
        pipe_vld            [0] <=              valid_in                            ;
    end
end

generate for (genvar i = 1 ; i < STAGES ; i++) begin
        : g_pipe always_ff @ ( posedge clk or negedge rst_n ) begin
            if (!rst_n) begin
                pipe_data           [                   i] <= '0                            ;
                pipe_vld            [                   i] <= 1'b0                          ;
            end
            else begin
                pipe_data           [i] <=              pipe_data[i-1]                      ;
                pipe_vld            [i] <=              pipe_vld[i-1]                       ;
            end
        end
    end
endgenerate

// ============================================================
// 09. case/endcase and duplicate-fold regression
// Expected:
//   - case/endcase folds.
//   - each multiline case item / begin-end body folds.
//   - There should be no exact duplicate fold ranges for the same region.
// ============================================================

always_comb begin
    case (selected_data[1:0])
        2'b00: begin
            data_out   = selected_data;
        end
        2'b01: begin
            data_out   = selected_data ^ {WIDTH {1'b1
                }
            };
        end
        2'b10: begin
            data_out   = selected_data << 1;
        end
        default : begin
            data_out   = '0;
        end
    endcase
end

// ============================================================
// 10. Preprocessor `ifdef / `else / `endif folding
// Expected:
//   - Pressing "za" on the `ifdef line folds from `ifdef through the line
//     before `else. It must not fold through `endif.
//   - Pressing "za" on the `else line folds from `else through `endif.
//   - The always_ff and always_comb RTL blocks fold locally.
//   - Inner if/begin blocks fold locally instead of forcing users to fold the
//     whole preprocessor branch.
//   - Directive folding is based on slang directive syntax/trivia, not raw
//     source string matching.
// ============================================================

`ifdef FOLDING_DEMO_USE_REGISTERED_VALID
always_ff @ ( posedge clk or negedge rst_n ) begin
    if (!rst_n) begin
        valid_out  <= 1'b0;
    end
    else begin
        valid_out  <= pipe_vld[STAGES-1];
    end
end
`else
always_comb begin
    valid_out  = pipe_vld[STAGES-1];
end
`endif

// ============================================================
// 11. Nested preprocessor conditional folding
// Expected:
//   - Outer conditional folds from `ifdef FOLDING_DEMO_OUTER through its `endif.
//   - Inner conditional folds independently through its own `endif.
// ============================================================

`ifdef FOLDING_DEMO_OUTER
assign demo_outer_enabled = 1'b1;
`ifdef FOLDING_DEMO_INNER
assign demo_inner_enabled = 1'b1;
`endif
`endif

`ifndef FOLDING_DEMO_OUTER
assign demo_outer_enabled = 1'b1;
`ifndef FOLDING_DEMO_INNER
assign demo_inner_enabled = 1'b1;
`endif
`endif

// ============================================================
// 12. Function and task declaration folding
// Expected:
//   - function ... endfunction folds.
//   - task ... endtask folds.
// ============================================================

function automatic logic [WIDTH-1:0] invert_data(
    input logic   [WIDTH-1:0] value
);
    invert_data = ~value;
endfunction

task automatic clear_pipeline;
    for (int i = 0 ; i < STAGES ; i++) begin
        pipe_data           [                   i]                  = '0            ;
        pipe_vld            [                   i]                  = 1'b0          ;
    end
endtask

// ============================================================
// 13. Multiline instance parameter override and port connection folding
// Expected:
//   - #(...) parameter override folds.
//   - (...) instance connection list folds.
// ============================================================

folding_leaf #(.WIDTH(WIDTH), .DEPTH(DEPTH)) u_leaf(
                                                  .clk(clk),
                                                  .rst_n(rst_n),
                                                  .data_in(data_in),
                                                  .data_out()
                                              );

// ============================================================
// 14. Single-line constructs excluded
// Expected:
//   - The single-line function and single-line begin/end below must not create
//     one-line folding ranges. Only multi-line constructs should fold.
// ============================================================

function logic pass_through(
    input logic value
);
    pass_through = value;
endfunction

always_comb begin
    selected_single_line = pass_through(valid_in);
end

endmodule

// Leaf module for the instance-folding demo above.
module folding_leaf #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 16
)(
    input     logic                   clk,
    input     logic                   rst_n,
    input     logic [WIDTH-1:0]       data_in,
    output    logic [WIDTH-1:0]       data_out
);
assign data_out   = rst_n ? data_in : '0;
endmodule
