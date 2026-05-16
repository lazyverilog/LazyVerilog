`include "params.svh"
`define WIDTH 32
`define print_bytes(ARR, STARTBYTE, NUMBYTES) \
    for (int ii=STARTBYTE; ii<STARTBYTE+NUMBYTES; ii++) begin \
        if ((ii != 0) && (ii % 16 == 0)) \
            display("\n"); \
        $display("0x%x ", ARR[ii]); \
    end

typedef struct {
    logic [7:0] addr;
    logic valid;
} packet_wo_data_t;

typedef struct {
    logic signed [7:0] addr;
    logic [31:0] data;
    logic valid;
} packet_t;

function packet_t sum(input packet_t i_a, input i_b);
    return packet_t'({40'b0, i_a} + i_b);
endfunction

task add_number(input int a, input int b, output int result);
    result = a + b;
endtask

parameter DEPTH = 8;

module memory_top(
    i_clk, i_rst_n, i_data,
    i_data2, i_data3, i_dd,
    i_dd22222, dd22222, i_d33333,
    i_d44333, i_dd44321, i_d44334,
    VDD, VSS
);
input i_clk;
input i_rst_n;
input logic signed [1:0] i_data[7:0];
input var byte i_data2;
input i_data3;
input i_dd;
input i_dd22222;
input dd22222;
input i_d33333;
input i_d44333, i_dd44321;
input i_d44334;
output logic unsigned [0:0] VDD, VSS;

logic [7:0] dout                    = 8'hFF;
logic [8:0] douteeeeeeeeeeeeeeeeeee = 8'hFF;
logic [`WIDTH-1:0] data;

packet_tttttttttttttt[1:0] dp;
logic [2:0] a, b;
//dd
/* ehlo */  // a, b, c
//
//
//
//
logic [7:0] data_out;
logic tt;
reg signed [7:0] kj;
// logic                                               c                           ;

// b
/* ehlo */  // logic           [2:0]                   d                           ;

automatic int [3:0] b;

wire [1:0] addr;
logic address;
logic test, r_test;
logic test3, r_test2;
logic r_test4;
logic [7:0] ddtt;
logic [7:0] dd;
logic [7:0] holyshit;
logic [7:0] zzzry;
logic [7:0] testxrp;
logic [7:0] threeshit;
logic [7:0] intercontest;
assign d = a + 1;

memory #(.MEM_SIZE(3)) u_memory (
    .address    (addr         ),
    .data_in    (intercontest ),
    .read_write (read_wsssrite),
    .chip_en    (tt           ),
    .www3test   (a            ),
    .data_out   (threeshit    )
);

memory u_mem (
    .i_clk      (testxrp     ),
    .address    (addressss   ),
    .data_in    (threeshit   ),
    .data_out   (intercontest),
    .read_write (read_write  ),
    .chip_en    (chip_en     ),
    .www333     (www333      ),
    .zzfuk      (zzfuk       )
);
memory u_mem1 (
    .address   (          ),
    .data_in   (zzzry     ),
    .dataut    (          ),
    .read_te   (          ),
    .chip_en   (          ),
    .wwtest    (          )
);

memory u_mem2 (
    .address    (          ),
    .data_in    (          ),
    .data_out   (          ),
    .read_write (          )
);

memory u_mem5 (
    .i_clk      (i_clk     ),
    .address    (addr      ),
    .data_in    (data_in   ),
    .data_out   (kj        ),
    .read_write (read_write),
    .chip_en    (chip_en   ),
    .www333     (www333    ),
    .www333     (www333    ),
    .zzfuk      (zzfuk     ),
    .zzfuk      (zzfuk     )
);

`ifdef RTL_SIM
memory u_mem3 (
    .i_clk      (i_clk     ),
    .address    (address   ),
    .data_in    (data_in   ),
    .data_out   (addr      ),
    .read_write (read_write),
    .chip_en    (tt        ),
    .www333     (www333    ),
    .www333     (www333    ),
    .zzfuk      (zzfuk     ),
    .zzfuk      (zzfuk     )
);
`else
memory u_mem4();
`endif

inv u_intq (
    .i_a       (i_a       ),
    .o_d       (o_d       )
);

always_comb begin
    // tte
    a      = 3;
    // tte

    if (a == 3) begin
        a      += 1;
    end

    for (int i = 0; i < 32; i++) begin
    end
    while (i < 5) begin
        $display("i = %0d", i);
        i++;
    end
    for (int i = 0; i < 32; i++) begin
        while (i < 5) begin
            $display("i = %0d", i);
            i++;
        end
    end
    do begin
        $display("i = %0d", i);
        i++;
    end while (i < 5);

    foreach (arr[i]) begin
        $display("arr[%0d]  = %0d",
                 i,
                 arr[i]);
    end

    repeat (3) begin
        $display("Hello");
    end

    forever begin
        #10;
        $display("Tick at time %0t", $time);
    end

    sum(.i_a(i_a2), .i_b(i_b));
    sum(1, 2);
    add_number(.a(a2),
               .b(b),
               .result(result));
end

initial begin
    forever #5 clk = ~clk;
    // 10 time-unit period
end

// Standard D-FF with synchronous active-low reset
always_ff @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
        data   <= 32'b0;
        r_test <= '0;
    end
    else begin
        data   <= d;
        r_test <= test;
    end
end

endmodule

module inv(
    i_a, o_d
);
`include "params.svh"
input fifo_entry_t[3:0] i_a;
output fifo_entry_t[3:0] o_d;

assign i_d = ~i_a;

endmodule
