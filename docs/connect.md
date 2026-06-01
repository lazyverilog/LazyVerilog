# Connect

`Connect` is an interactive Neovim command for wiring one module instance output
port to another module instance input port through their nearest common parent.

## Commands

- `:Connect <source_module> <dest_module>`
  - shows instances of `source_module`
  - shows output ports on the selected source instance
  - shows instances of `dest_module`
  - shows input ports on the selected destination instance
  - asks for a wire name
  - shows a confirmation preview before applying edits

The server-side commands are:

- `lazyverilog.connectInfo`
- `lazyverilog.connectApplyPreview`
- `lazyverilog.connectApply`

## Behavior

Connect uses the current open buffers plus configured design filelist files to
find module declarations and instantiations. Module, port, direction, datatype,
instance, and named-connection information comes from the slang-backed syntax
index.

When the user confirms the preview, Connect generates local text edits to:

- connect or replace `.port(signal)` on the selected source and destination
  instances
- declare the requested signal in the common parent module when it is not already
  declared
- for cross-hierarchy paths, add pass-through ports on intermediate modules
- warn in the preview when an existing connection will be overwritten
- warn but continue on source/destination type mismatch, using the source output
  port as the declaration source

## Declaration type rules

Generated bridge signals are declared from the **output port** datatype.

For net-style output ports, the generated bridge signal is a variable declaration
using `logic`, not a net declaration:

```systemverilog
module memory(output wire [5:0] o_data);
endmodule

// generated bridge declaration
logic [5:0] data32;
```

Symbolic and user-defined datatype text is preserved syntactically:

```systemverilog
output payload_t [`BUS_W-1:0] payload
// -> payload_t [`BUS_W-1:0] payload_w;

output logic [DEPTH-1:0] data
// -> logic [DEPTH-1:0] data_w;
```

If an older UI/client supplies only a packed dimension such as `[WIDTH-1:0]`, the
server falls back to a valid variable declaration:

```systemverilog
logic [WIDTH-1:0] data_w;
```

## Implementation notes and limitations

The structural model is syntax-index based, not a full semantic elaboration. It
supports normal named module instance connections and hierarchy built from parsed
modules/instances.

Text edits are still constructed as targeted source edits, so complex generated
or macro-expanded text can require manual cleanup. Known limitations include:

- complex generate hierarchy
- macro-generated instances or connections
- positional port connections
- real SystemVerilog `interface` / `modport` elaboration
