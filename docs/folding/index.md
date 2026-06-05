# Folding Ranges

LazyVerilog provides editor folding ranges for common SystemVerilog structure.
When your editor supports LSP folding, these ranges let you collapse large or
repetitive sections without changing the source text.

## Module headers

Parameterized module headers expose separate folds for the parameter list and
the port list.

```systemverilog
module folding_demo #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 16,
    parameter int STAGES = 3
)(
    input  logic             clk,
    input  logic             rst_n,
    output logic [WIDTH-1:0] data_out
);
```

Available folds:

- parameter list: the `#(...)` section
- port list: the following `(...)` section
- whole module: from `module` through `endmodule`

## Module bodies

A module can be folded as one whole region, including its header.

```systemverilog
module top;
    logic a;
    logic b;
endmodule
```

Folding inside the module body can collapse the whole module, unless the cursor
is inside a smaller nested fold such as a declaration run, comment block, or
procedural block.

## Declarations

Consecutive declarations fold as a single declaration section.

```systemverilog
logic [7:0] data_q;
logic       valid_q;
logic       ready_q;
```

The same behavior applies to different declaration forms, including parameters,
nets, variables, and user-defined types.

```systemverilog
localparam int WIDTH = 8;
wire [WIDTH-1:0] data_w;
payload_t        payload_q;
logic            valid_q;
```

A non-declaration statement breaks the declaration fold.

```systemverilog
logic a;
logic b;
assign b = a;
logic c;
logic d;
```

This creates two declaration folds: `a/b` and `c/d`.

## Non-ANSI port declarations

For non-ANSI modules, consecutive semicolon-terminated port declarations fold as
a declaration section. They can also fold together with following ordinary
signal declarations.

```systemverilog
module top (
    clk,
    rst_n,
    data_in,
    data_out
);
    input  logic     clk;
    input  logic     rst_n;
    input  payload_t data_in;
    output payload_t data_out;
    logic            valid_q;
    logic [3:0]      count_q;
endmodule
```

## Imports

Consecutive package imports fold as one import section.

```systemverilog
import pkg_a::*;
import pkg_b::item_t;
import pkg_c::*;
```

## Comments

Consecutive own-line comments fold as one comment block.

```systemverilog
// First line
// Second line
// Third line
```

Trailing comments do not start a comment fold.

## Preprocessor regions

Preprocessor conditional branches can be folded independently.

```systemverilog
`ifdef USE_A
assign y = a;
`else
assign y = b;
`endif
```

## Procedural and structural blocks

Common multi-line blocks are foldable, including:

- `begin` / `end`
- `case` / `endcase`
- `generate` / `endgenerate`
- `fork` / `join` variants
- function and task bodies
- class, constraint, and covergroup bodies
- typedef enum / struct / union bodies
