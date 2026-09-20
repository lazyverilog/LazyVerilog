# Connect

Wires an output port of one module instance to an input port of another, through their nearest common
parent.

- Neovim: `:Connect <source_module> <dest_module>`
- VS Code: `LazyVerilog: Connect`

You are asked to pick:

1. an instance of the source module, and its output port
2. an instance of the destination module, and its input port
3. a name for the wire

A preview shows the edits before anything is applied.

![Neovim asking for the source output port of mem_ctrl, with the candidate ports listed](/screenshots/connect-port-neovim.webp)

![Neovim showing the Connect Preview: two port connections and a new wire declaration, with [y] Apply and [n] Cancel](/screenshots/connect-preview-neovim.webp)

## What it edits

- Sets `.port(signal)` on both instances, replacing an existing connection (the preview warns).
- Declares the wire in the common parent if it is not declared yet.
- For instances in different branches of the hierarchy, adds pass-through ports on the modules between.
- On a type mismatch it warns and continues, using the source port's type.

## Wire type

The wire copies the source port's type. Net-style outputs become `logic`, and user-defined types and
symbolic dimensions are kept as written:

```systemverilog
output wire [5:0] o_data              // becomes: logic [5:0] data32;
output payload_t [`BUS_W-1:0] payload // becomes: payload_t [`BUS_W-1:0] payload_w;
```

If neither port exists yet, the wire is `logic`.

## Limits

Connect works on syntax, not elaboration, so it does not handle complex `generate` hierarchy,
macro-generated instances, positional connections, or SystemVerilog `interface`/`modport` ports.
Check the preview when the code is unusual.
