`include "params.svh"
`define WIDTH 32
`define print_bytes(ARR, STARTBYTE, NUMBYTES)                \
    for(int ii=STARTBYTE; ii<STARTBYTE+NUMBYTES; ii++) begin \
        if((ii != 0) && (ii % 16 == 0))                      \
            display("\n");                                   \
        $display("0x%x ", ARR[ii]);                          \
    end
function packet_t foo(input packet_t i_a, input i_b);
    return packet_t'(i_a+i_b);
endfunction

typedef struct {
    logic               [7:0]               addr                                ;
    logic                                   valid                               ;
} packet_wo_data_t;

typedef struct {
    logic signed        [7:0]               addr                                ;
    logic               [31:0]              data                                ;
    logic                                   valid                               ;
} packet_ta;

typedef enum logic [1:0] {
    IDLE       = 2'd0        ,
    FETCH                    ,
    EXECUTE    = 2'd2        ,
    ERROR
} state_t;

interface bus_intf #(
    parameter W_IDTH = 8
)(
    input     logic      i_clk
);

logic                                   valid                               ;
logic                                   ready                               ;
logic               [`WIDTH123:0]       addr                                ;
logic               [`WIDTH-1:0]        wdata                               ;
logic               [`WIDTH-1:0]        rdata                               ;
logic                                   write                               ;

// DUT view
modport dut (
    input          clk       ,
    input          validdddddddd,
    input          addr      ,
    input          wdata     ,
    input          write     ,
    output         ready     ,
    output         rdata
);

// Testbench/driver view
modport tb (
    input          clk       ,
    input          ready     ,
    input          rdata     ,
    output         valid     ,
    output         addr      ,
    output         wdata     ,
    output         write
);

endinterface

parameter DEPTH = 8;

module memory_top #(
    parameter int WIDTH = 4,
    parameter int DEPTH = 8
)( /*autoarg*/
    i_clk, i_rst_n, i_data,
    i_d
);
input                            i_clk                           ;
input                            i_rst_n                         ;
input     logic      [1:0]       i_data            [7:0]         ; // input
input     var byte               i_data2                         ;
input                            i_data3                         ;
input                            i_dd                            ;
input                            i_dd22222                       ;
input                            dd22222                         ;
input                            i_d33333                        ;
input                            i_d44333                        , i_dd44321                  ;
input                            i_d44334                        ;
output    logic unsigned [0:0]   VDD                             , VSS                        ; // output
output    packet_tttttttttttttt [0:0] test                       , VSS                        ; /* output */ // test

logic               [7:0]               dout                = 8'hFF         ;
logic               [8:0]               douteeeeeee         = 8'hFF         ;
packet_tttttttttttttt [1:0]             test_init           = 8'hFF         ;
packet_t                                test_init2          = 8'hFF         ;
logic               [`WIDTH-1:0]        data                                ;

packet_ttttttttttteeettt [1:0]          dp                                  ;
// test
logic               [2:0]               a, b                                   ;
//dd
/* ehlo */ // a, b, c
//
//
//
//
logic               [7:0]               data_out                            ;
logic                                   tt                                  ;
reg signed          [7:0]               kj                                  ;
// logic                                               c                           ;

// b
/* ehlo */ // logic           [2:0]                   d                           ;

automatic int       [3:0]               b                                   ;

wire                [1:0]               addr                                ;
logic                                   address                             ;
logic                                   test, r_test                              ; // test
logic                                   test3, r_test2                             ;
/* */
logic                                   r_test4                             ;
logic               [7:0]               ddtt                                ;
logic               [7:0]               dd                                  ;
logic               [7:0]               holyshit                            ;
logic               [7:0]               zzzry                               ;
logic               [7:0]               testxrp                             ;
logic               [7:0]               threeshit                           ;
logic               [3:0]               www333                              ;
logic               [3:0]               zzfuk                               ;
logic               [3:0]               o_d                                 ;
logic                                   d                                   ;
logic               [7:0]               intercontest                        ;
logic               [7+:1]              intercontest                        ;

assign d          = a+2;
assign daaa       = a+2;
assign very_long_text = a+2;

memory #(.MEM_SIZE(3) /*testtest*/) u_memory ( /*autoinst*/
    .address  (addr      ),
    .data_in  (intercontest),
    .read_write (read_wsssrite),
    .chip_en  (tt        ),
    .www3test (a         ),
    .data_out (threeshit )
);

memory #( /*param*/.MEM_SIZE(3) /*testtest*/) u_mem ( /*autoinst*/
    .i_clk    (testxrp   ),
    .address  (addressss ),
    .data_in  (threeshit ), // input
    .chip_en  (chip_en   ), // output
    .www333   (www333    ), // input
    .zzfuk    (zzfuk     )
);

memory u_mem1 (
    .address  (          ),
    .data_in  (zzzry     ), // test
    .dataut   (          ),
    .read_te  (          ),
    .chip_en  (          ),
    .wwtest   (          )
);

memory u_mem2 (
    .address  (          ),
    .data_in  (          ),
    .data_out (          ),
    .read_write (          )
);

memory u_mem5 ( //test
    // input
    .i_clk    (i_clk     ), // input
    .address  (addr      ), // output() .data_in  (data_in   ),
`ifdef A
    .data_out (kj        ), // test
`elsif B
    .read_write (read_write),
`endif
    .chip_en  (chip_en   ),
    .www333   (www333    ),
    .www333   (www333    ),
    .zzfuk    (zzfuk     ),
    .zzfuk    (zzfuk     )
);

`ifdef RTL_SIM
memory u_mem3 (
    .i_clk    (i_clk     ),
    .address  (address   ),
    .data_in  (data_in   ),
    .data_out (addr      ),
    .read_write (read_write),
    .chip_en  (tt        ),
    .www333   (www333    ),
    .www333   (www333    ),
    .zzfuk    (zzfuk     ),
    .zzfuk    (zzfuk     )
);
`else
memory u_mem4();
`endif

inv u_intq (
    .i_a      (i_a       ),
    .o_d      (o_d       )
);

always @ ( * ) begin
    if (a) begin
        a2         <= 3; /* ttt*/
    end
end

always_comb begin
    // tte
    a          <= 3;
    very_long  <= 3;

    if (a==3) begin
        a          += 1;
        //test
    end

    case (a)
        1: begin
        end
    endcase

    for (int i = 0 ; i<32 ; i++) begin
    end

    while (i<5) begin
        $display("i = %0d", i);
        /* test*/
        i++;
    end

    for (int i = 0 ; i<32 ; i++) begin
        while (i<5) begin
            $display("i = %0d", i);
            i++;
        end
    end

    do begin
        $display("i = %0d", i);
        i++;
    end while (i<5);

    foreach (arr[i]) begin
        $display(
            "arr[%0d]  = %0d",
            i,
            arr[i]
        );
    end

    repeat (3) begin
        $display("Hello");
    end

    // Whitespace-sensitive macro invocation: formatter should preserve the
    // original spacing and nested call layout inside the macro arguments.
    `print_bytes(data_out, 0, add_number(1, 2, 3))
    `print_bytes(1, 2, 3)

    forever begin
        #10;
        $display("Tick at time %0t", $time);
    end

// verilog_format: off
        sum(.i_a(i_a2),
        .i_b(i_b));
    // verilog_format: on
    sum(.i_a(1), .i_b(2));
    sum(.i_a(1), .i_b(i_b));
    sum(.i_a(1), .i_b(i_b));
    // com
    sum(.i_a(1), .i_b(i_b));
    sum(1, 2);
    /**/
    add_number(
`ifdef HI
    .a(a3),
`endif
    .b(b), .result(result));

    add_number(
        a,
        add_number(a, b, c),
        c
    );

    // Multiline function call with a macro argument: exercises
    // format_multiline_macro_arg_calls_pass before the line-based function
    // call formatter sees each physical line separately.
    add_number(a,
    `WIDTH, c);

    if (add_number(
            .a(a),
            .b(b),
            .result(result)
        )) begin
        a          = 3;
        b          = 7+1;
    end

    if (add_number(
            .a(a),
            .b(b),
            .result(result)
        ))
        a          = 3;
    b          = 3;

    b          = 7;
    add_number(
        .a(a),
        .b(b),
        .result(result)
    );
end

initial begin
    forever #5 clk = ~clk;
    // 10 time-unit period
end

// Standard D-FF with synchronous active-low reset
always_ff @ ( posedge i_clk or negedge i_rst_n ) begin
    if (!i_rst_n) begin
        data       <= 32'b0;
        r_test     <= '0;
    end
    else begin
        data       <= d;
        r_test     <= test;
    end
end

endmodule

// Test format_class_extends_parameter_pass: class extending parameterized base
// with multiple params on one line — formatter should reflow the #(...) list.
class my_seq extends base_seq #(
    int,
    string,
    logic [7:0]
);
endclass

class my_long_seq extends uvm_sequence #(
    my_item_t,
    my_rsp_t,
    int unsigned
);
endclass

// Test format_covergroup_pass and format_constraint_dist_pass.
class memory_format_demo_item;
    bit                 sample_clk                          ;
    rand int unsigned burst_len;
    rand bit [1:0] op;

    constraint burst_dist_c {
        burst_len dist {
            1 := 10,
            [2:4] := 20,
            [5:8] := 5
        };
    }
    covergroup cg @(posedge sample_clk);
        option.per_instance = 1;
        op_cp: coverpoint op {
            bins read_write[] = {[0:1]};
            bins idle  = {2};
        }
        burst_cp: coverpoint burst_len {
            bins short = {[1:4]};
            bins long  = {[5:8]};
        }
    endgroup
endclass

module inv(
    i_a, o_d
);

`include "params.svh"

logic               i_d                                 ;
logic               i_e                                 ;

input     fifo_entry_t [3:0]     i_a                        ;
output    fifo_entry_t [3:0]     o_d                        ;

assign i_d        = ~i_a;
assign i_e        = i_a;

endmodule
