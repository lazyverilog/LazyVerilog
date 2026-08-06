`define PARAMSVH_PARAM 589
typedef struct packed {
    logic                                   valid                               ;
    logic               [3:0]               id                                  ;
    logic               [31:0]              data                                ;
} fifo_entry_t;

function packet_t sum(
    input packet_t i_a,
    input i_b
);
    return packet_t'({40'b0, i_a} + i_b);
endfunction
typedef enum logic [1:0] {
    IDLE       = 2'd0        ,
    FETCH                    ,
    EXECUTE    = 2'd2        ,
    ERROR
} state_t;

task add_number(
    input int a,
    input int b,
    output int result
);
    result     = a + b;
endtask

package cpu_pkg;
// State machine states

// Instruction opcode
typedef enum logic [2:0] {
    OP_NOP     = 3'b000      ,
    OP_ADD     = 3'b001      ,
    OP_SUB     = 3'b010      ,
    OP_AND     = 3'b011      ,
    OP_OR      = 3'b100
} opcode_t;
endpackage

// Shared constant
parameter int DATA_WIDTH = 32;

task req_data();
    $display("%0d", data);
endtask

class Packet;

    int                 data                                ;

    function void set_data(
        int d
    );
        data       = d;
    endfunction

    function int get_data();
        return data;
    endfunction

`ifdef FOO
    task print_data();
        $display("%0d", data);
    endtask
    task req_data();
`elsif BAR
        $display("%0d", data);
    endtask
`endif

endclass
