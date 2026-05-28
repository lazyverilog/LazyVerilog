typedef struct packed {
    logic                                   valid                               ;
    logic               [3:0]               id                                  ;
    logic               [31:0]              data                                ;
} fifo_entry_t;

function packet_t sum(
    input packet_t i_a,
    input i_b
);
    return packet_t'({40'b0, i_a}+i_b);
endfunction

task add_number(
    input int a,
    input int b,
    output int result
);
    result = a+b;
endtask

package cpu_pkg;

// State machine states
typedef enum logic [1:0] {
    IDLE      = 2'd0        ,
    FETCH     ,
    EXECUTE   = 2'd2        ,
    ERROR
} state_t;

// Instruction opcode
typedef enum logic [2:0] {
    OP_NOP    = 3'b000      ,
    OP_ADD    = 3'b001      ,
    OP_SUB    = 3'b010      ,
    OP_AND    = 3'b011      ,
    OP_OR     = 3'b100
} opcode_t;

// Shared constant
parameter int DATA_WIDTH = 32;

endpackage

typedef enum logic [1:0] {
    IDLE      ,
    FETCH     ,
    EXECUTE   ,
    ERROR
} state_t;
