typedef struct packed {
    logic valid;
    logic [3:0] id;
    logic [31:0] data;
} fifo_entry_t;
