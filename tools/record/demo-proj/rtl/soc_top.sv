module soc_top (
  input logic i_clk,
  input logic i_rst_n
);

  logic [31:0] cpu_addr;
  logic [31:0] cpu_wdata;
  logic [31:0] cpu_rdata;
  logic        cpu_valid;
  logic        cpu_ready;
  logic [15:0] mem_addr;
  logic        mem_we;
  logic [31:0] mem_rdata;

  cpu_core u_cpu (
    .i_clk  (i_clk),
    .i_rst_n(i_rst_n),
    .o_addr (cpu_addr),
    .o_wdata(cpu_wdata),
    .o_valid(cpu_valid),
    .i_rdata(cpu_rdata),
    .i_ready(cpu_ready),
    .i_irq  ()
  );

  bus_ctrl u_bus (
    .i_clk      (i_clk),
    .i_rst_n    (i_rst_n),
    .i_addr     (cpu_addr),
    .i_wdata    (cpu_wdata),
    .i_valid    (cpu_valid),
    .o_rdata    (cpu_rdata),
    .o_ready    (cpu_ready),
    .o_mem_addr (mem_addr),
    .o_mem_we   (mem_we),
    .i_mem_rdata(mem_rdata)
  );

  mem_ctrl u_mem (
    .i_clk  (i_clk),
    .i_addr (mem_addr),
    .i_we   (mem_we),
    .o_rdata(mem_rdata),
    .o_irq  ()
  );

endmodule
