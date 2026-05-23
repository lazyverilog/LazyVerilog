# Formatter Options

All options live under `[format]` in `lazyverilog.toml`.

---

## Top-level options

### `indent_size`

Number of spaces per indentation level.

```toml
[format]
indent_size = 4
```

```systemverilog
// indent_size = 2
module m_top;
  logic a;
endmodule

// indent_size = 4
module m_top;
    logic a;
endmodule
```

---

### `blank_lines_between_items`

Maximum number of blank lines preserved between top-level items (modules, always blocks, assigns, etc.). Extra blank lines beyond this value are collapsed.

```toml
[format]
blank_lines_between_items = 1
```

```systemverilog
// blank_lines_between_items = 1
module m_top;
  assign a = b;

  assign c = d;
endmodule

// blank_lines_between_items = 0   (all blank lines removed)
module m_top;
  assign a = b;
  assign c = d;
endmodule
```

---

### `default_indent_level_inside_outmost_block`

How many indent levels to add inside the outermost `module`, `interface`, or `package` block. Set to `0` to keep module body at column 0.

```toml
[format]
default_indent_level_inside_outmost_block = 1
```

```systemverilog
// default_indent_level_inside_outmost_block = 1
module m_top;
  logic a;
endmodule

// default_indent_level_inside_outmost_block = 0
module m_top;
logic a;
endmodule
```

---

### `tab_align`

When `true`, alignment column widths are snapped to the nearest multiple of `indent_size`. Produces cleaner tab-stop-aligned columns across port declarations, variable declarations, instances, enums, and modports.

```toml
[format]
tab_align = true
indent_size = 4
```

```systemverilog
// tab_align = false, indent_size = 4
input  logic [7:0] data,
output logic       valid

// tab_align = true, indent_size = 4  (columns snap to multiples of 4)
input   logic   [7:0]   data,
output  logic            valid
```

---


### `enable_format_on_save`

When `true`, the LSP server responds to `textDocument/formatting` requests (triggered by editor save). When `false`, formatting requests return no edits.

```toml
[format]
enable_format_on_save = true
```

---

### `safe_mode`

When `true`, the formatter verifies that no non-whitespace content changed after formatting. If a formatting bug would corrupt code, it throws `SafeModeError` and aborts instead of returning broken output.

```toml
[format]
safe_mode = true
```

---

## `[format.spacing]`

Controls whitespace around operators, keywords, and delimiters.

### `control_keyword_space`

Insert a space between control keywords (`if`, `for`, `while`, etc.) and the opening parenthesis.

```toml
[format.spacing]
control_keyword_space = true
```

```systemverilog
// control_keyword_space = true
if (a) begin

// control_keyword_space = false
if(a) begin
```

---

### `space_inside_parens`

Insert spaces inside ordinary parentheses.

```toml
[format.spacing]
space_inside_parens = false
```

```systemverilog
// space_inside_parens = false
assign a = foo(bar);

// space_inside_parens = true
assign a = foo( bar );
```

---

### `space_inside_dimension_brackets`

Insert spaces inside dimension brackets `[ ]`.

```toml
[format.spacing]
space_inside_dimension_brackets = false
```

```systemverilog
// space_inside_dimension_brackets = false
logic [7:0] data;

// space_inside_dimension_brackets = true
logic [ 7:0 ] data;
```

---

### `binary_operator_spacing`

Controls spaces around binary operators (`+`, `-`, `*`, `==`, etc.) outside dimension brackets.

Values: `"none"` | `"before"` | `"after"` | `"both"`

```toml
[format.spacing]
binary_operator_spacing = "both"
```

```systemverilog
// "both"
assign c = a + b;
// "none"
assign c = a+b;
// "before"
assign c = a +b;
// "after"
assign c = a+ b;
```

---

### `dimension_binary_operator_spacing`

Controls spaces around binary operators **inside** dimension brackets `[...]`.

Values: `"none"` | `"before"` | `"after"` | `"both"`

```toml
[format.spacing]
dimension_binary_operator_spacing = "none"
```

```systemverilog
// "none"
logic [WIDTH-1:0] data;
// "both"
logic [WIDTH - 1:0] data;
```

---

### `semicolon_spacing`

Controls spaces around semicolons in `for`-loop headers.

Values: `"none"` | `"before"` | `"after"` | `"both"`

```toml
[format.spacing]
semicolon_spacing = "after"
```

```systemverilog
// "after"
for (int i = 0; i < 8; i++)
// "both"
for (int i = 0 ; i < 8 ; i++)
// "none"
for (int i = 0;i < 8;i++)
```

---

### `range_colon_spacing`

Controls spaces around the colon in range expressions inside `[...]`.

Values: `"none"` | `"before"` | `"after"` | `"both"`

```toml
[format.spacing]
range_colon_spacing = "none"
```

```systemverilog
// "none"
logic [7:0] data;
// "both"
logic [7 : 0] data;
```

---

### `indexed_part_select_spacing`

Controls spaces around indexed part-select operators `+:` and `-:`.

Values: `"none"` | `"before"` | `"after"` | `"both"`

```toml
[format.spacing]
indexed_part_select_spacing = "both"
```

```systemverilog
// "both"
data[offset +: WIDTH]
// "none"
data[offset+:WIDTH]
```

---

### `procedural_event_control_at_spacing`

Controls spaces around `@` in procedural event control (`always @(...)`).

Values: `"none"` | `"before"` | `"after"` | `"both"`

```toml
[format.spacing]
procedural_event_control_at_spacing = "before"
```

```systemverilog
// "before"
always @(posedge clk)
// "after"
always@ (posedge clk)
// "both"
always @ (posedge clk)
// "none"
always@(posedge clk)
```

---

### `space_inside_event_control_parens`

Insert spaces inside event control parentheses `@(...)`.

```toml
[format.spacing]
space_inside_event_control_parens = false
```

```systemverilog
// false
always @(posedge clk)
// true
always @( posedge clk )
```

---

### `assignment_operator_spacing`

Controls spaces around assignment operators `=` and `<=`.

Values: `"none"` | `"before"` | `"after"` | `"both"`

```toml
[format.spacing]
assignment_operator_spacing = "both"
```

```systemverilog
// "both"
assign a = b;
q <= d;
// "none"
assign a=b;
q<=d;
```

---

## `[format.statement]`

Controls formatting of consecutive assignment statements.

### `align`

Align `=` and `<=` operators across consecutive assignment lines.

```toml
[format.statement]
align = true
```

```systemverilog
// align = true
a      = 1;
data   = 2;
result = 3;

// align = false
a = 1;
data = 2;
result = 3;
```

---

### `align_adaptive`

When `true`, each line computes its own alignment width (minimum of `lhs_min_width` or the line's LHS width). When `false`, all consecutive lines in a group share the same column.

```toml
[format.statement]
align_adaptive = false
```

```systemverilog
// align = true
// align_adaptive = false
a               = 1;
data            = 2;
result          = 3;
very_long_text  = 4;

// align = true
// align_adaptive = true
a           = 1;
data        = 2;
result      = 3;
very_long_text  = 4;

---

### `lhs_min_width`

Minimum character width of the left-hand side column before the assignment operator when `align` is `true`.

```toml
[format.statement]
lhs_min_width = 6
```

```systemverilog
// lhs_min_width = 6
a      = 1;
data   = 2;

// lhs_min_width = 1
a    = 1;
data = 2;
```

---

### `begin_newline`

When `true`, block openers after control expressions are placed on a new line. When `false`,
they stay on the control line. This applies to `begin` and to constraint block braces.

```toml
[format.statement]
begin_newline = false
```

```systemverilog
// begin_newline = false
if (a) begin
  ...
end

constraint c {
  if (a) {
    x == 1;
  }
}

// begin_newline = true
if (a)
begin
  ...
end

constraint c
{
  if (a)
  {
    x == 1;
  }
}
```

---

### `wrap_end_else_clauses`

When `true`, `else` after `end` or `}` is placed on a new line. When `false`, `end else` or `} else`
stays on the same line.

```toml
[format.statement]
wrap_end_else_clauses = true
```

```systemverilog
// wrap_end_else_clauses = true
end
else begin
  ...
end

}
else {
  ...
}

// wrap_end_else_clauses = false
end else begin
  ...
end

} else {
  ...
}
```

---

## `[format.port_declaration]`

Controls alignment of
- ANSI port declarations inside module header.
- non-ANSI port declarations inside module body.

Port declarations are split into 5 sections:
1. Direction (`input`, `output`, `inout`)
2. Type + qualifier (`logic`, `wire signed`, etc.)
3. Packed dimension (`[7:0]`)
4. Port name
5. Trailing (unpacked dimensions, `= default`, etc.)

### `align`

Enable column alignment of the 5 sections across consecutive port declarations.

```toml
[format.port_declaration]
align = true
```

```systemverilog
// align = true
input  logic [7:0]  data,
output logic        valid

// align = false
input logic [7:0] data,
output logic valid
```

---

### `align_adaptive`

When `true`, each port line computes column widths independently using its own content and the minimum widths, rather than aligning to the widest value across the entire port group.

```toml
[format.port_declaration]
align_adaptive = false
```

---

### `section1_min_width` .. `section5_min_width`

Minimum character width for each alignment section. When `tab_align` is `true`, these are snapped to indent grid.

```toml
[format.port_declaration]
section1_min_width = 10   # direction column
section2_min_width = 20   # type column
section3_min_width = 20   # dimension column
section4_min_width = 30   # port name column
section5_min_width = 30   # trailing column
```

```systemverilog
// align = true
// align_adaptive = true
// section1_min_width = 10
// section2_min_width = 11
// section3_min_width = 12
// section4_min_width = 13
// section5_min_width = 14
// tab_align = false
input                            i_clk                      ;
input                            i_rst_n                    ;
input     logic      [1:0]       i_data       [7:0]         ;
input     var byte               i_data2                    ;
|        |           |           |            |             |
 section1   section2    section3   section4       section5
```

---

## `[format.var_declaration]`

Controls alignment of variable/signal declarations (`logic`, `wire`, `reg`, etc.) in module body.

Declarations are split into 4 sections:
1. Type + qualifier (`logic`, `wire signed`, etc.)
2. Packed dimension (`[7:0]`)
3. Signal name
4. Trailing (unpacked dimensions, initializers, etc.)

### `align`

Enable column alignment across consecutive variable declarations.

```toml
[format.var_declaration]
align = true
```

```systemverilog
// align = true
logic [7:0]  data;
logic        valid;

// align = false
logic [7:0] data;
logic valid;
```

---

### `align_adaptive`

Per-line adaptive alignment (same concept as port declarations).

```toml
[format.var_declaration]
align_adaptive = false
```

---

### `section1_min_width` .. `section4_min_width`

Minimum character width for each section.

```toml
[format.var_declaration]
section1_min_width = 16    # type column
section2_min_width = 12   # dimension column
section3_min_width = 20   # signal name column
section4_min_width = 16    # trailing column (0 disables trailing alignment)
```

```systemverilog
logic           [7:0]       dout                = 8'hFF         ;
logic           [8:0]       din                 = 8'hFF         ;
packet_t        [1:0]       test_init           = 8'hFF         ;
packet_t                    test_init2          = 8'hFF         ;
|               |           |                   |               |
    section1      section2          section3        section4
```

---

## `[format.instance]`

Controls formatting of module instantiation port connections.

### `align`

Align port connections across lines in an instance.

```toml
[format.instance]
align = true
```

```systemverilog
// align = true
m_fifo u_fifo (
  .clk   (clk  ),
  .data  (data ),
  .valid (valid)
);

// align = false
m_fifo u_fifo (
  .clk(clk),
  .data(data),
  .valid(valid)
);
```

---

### `port_indent_level`

Number of indent levels for port lines relative to the instantiation line.

```toml
[format.instance]
port_indent_level = 1
```

```systemverilog
// port_indent_level = 1, indent_size = 4
m_fifo u_fifo (
    .clk(clk)
);

// port_indent_level = 2, indent_size = 4
m_fifo u_fifo (
        .clk(clk)
);
```

---

### `instance_port_name_width`

Total field width from `.` to `(` — controls spacing between the port name and the opening parenthesis.

```toml
[format.instance]
instance_port_name_width = 10
```

```systemverilog
// instance_port_name_width = 10
m_fifo u_fifo (
    .clk       (clk  ),
    .data      (data ),
);
```

---

### `instance_port_between_paren_width`

Total field width from `(` to `)` — controls spacing between the signal name and the closing parenthesis.

```toml
[format.instance]
instance_port_between_paren_width = 10
```

```systemverilog
// instance_port_between_paren_width = 10
m_fifo u_fifo (
    .clk  (clk       ),
    .data (data      ),
);
```

---

### `align_adaptive`

When `true`, each port line computes its own gap. When `false`, all ports in the instance share common alignment columns.

```toml
[format.instance]
```

```systemverilog
// align_adaptive = false
m_fifo u_fifo (
    .clk            (clk           ),
    .data           (data          ),
    .very_long_text (              ),
    .din            (very_long_text)
);
// align_adaptive = true
m_fifo u_fifo (
    .clk   (clk       ),
    .data  (data      ),
    .very_long_text (data      ),
    .din   (very_long_text)
);
```

---

## `[format.function_call]`

Controls formatting of function/task calls.

### `break_policy`

When to break function call arguments onto separate lines.

- `"never"` — always single-line
- `"always"` — always break (if args exist)
- `"auto"` — break when line exceeds `line_length` or argument count exceeds `arg_count`

```toml
[format.function_call]
break_policy = "auto"
```

---

### `line_length`

When `break_policy = "auto"`, break if the single-line rendering exceeds this character width.

```toml
[format.function_call]
line_length = 100
```

```systemverilog
// break_policy = auto
// line_length = 10
    sum(.i_a(i_a2),
        .i_b(i_b));
// break_policy = auto
// line_length = 20
    sum(.i_a(i_a2), .i_b(i_b));
```

---

### `arg_count`

When `break_policy = "auto"`, break if the number of arguments is >= this value. Set to `-1` to disable arg-count breaking.

```toml
[format.function_call]
arg_count = 3
```

```systemverilog
// arg_count = 3, 2 args → stays single-line
foo(a, b);

// arg_count = 3, 3 args → breaks
foo(
    a,
    b,
    c
);
```

---

### `layout`

How to indent broken arguments.

- `"block"` — arguments indented one level from the call
- `"hanging"` — arguments aligned to the opening parenthesis

```toml
[format.function_call]
layout = "block"
```

```systemverilog
// layout = "block"
foo(
  a,
  b,
  c
);

// layout = "hanging"
foo(a,
    b,
    c);
```

---

### `space_before_paren`

Insert a space between the function name and the opening `(`.

```toml
[format.function_call]
space_before_paren = false
```

```systemverilog
// false
foo(a, b);
// true
foo (a, b);
```

---

### `space_inside_paren`

Insert spaces inside function call parentheses.

```toml
[format.function_call]
space_inside_paren = false
```

```systemverilog
// false
foo(a, b);
// true
foo( a, b );
```

---

## `[format.function_declaration]`

Controls formatting of `function` and `task` declaration port lists.

### `layout`

How to indent broken port arguments.

- `"block"` — ports indented one level from the declaration
- `"hanging"` — ports aligned to the opening parenthesis

```toml
[format.function_declaration]
layout = "block"
```

```systemverilog
// layout = "block"
function void foo(
  input logic a,
  input logic b
);

// layout = "hanging"
function void foo(input logic a,
                  input logic b);
```

---

### `line_length`

Declarations shorter than this stay single-line. Declarations exceeding this are broken according to `layout`.

```toml
[format.function_declaration]
line_length = 100
```

```systemverilog
// line_length = 100, short declaration stays single-line
function void foo(input logic a, input logic b);

// line_length = 40, same declaration breaks
function void foo(
  input logic a,
  input logic b
);
```

---

## `[format.module]`

Controls module header formatting.

### `parameter_layout`

How to lay out `#(...)` parameter lists when broken.

- `"block"` — parameters indented one level
- `"hanging"` — parameters aligned to `#(`

```toml
[format.module]
parameter_layout = "block"
```

```systemverilog
// parameter_layout = "block"
module m_top #(
  parameter WIDTH = 8,
  parameter DEPTH = 16
)(
  ...
);

// parameter_layout = "hanging"
module m_top #(parameter WIDTH = 8,
               parameter DEPTH = 16)(
  ...
);
```

---

### `non_ansi_port_per_line_enabled` / `non_ansi_port_per_line`

When enabled, non-ANSI port lists are wrapped with a fixed number of ports per line.

```toml
[format.module]
non_ansi_port_per_line_enabled = true
non_ansi_port_per_line = 3
```

```systemverilog
// non_ansi_port_per_line = 3
module m_top(
  a, b, c,
  d, e, f,
  g
);
```

---

### `non_ansi_port_max_line_length_enabled` / `non_ansi_port_max_line_length`

When enabled, non-ANSI port lists are wrapped based on maximum line length instead of a fixed port count. Mutually exclusive with `non_ansi_port_per_line` — if both are enabled, `non_ansi_port_per_line` takes priority.

```toml
[format.module]
non_ansi_port_max_line_length_enabled = true
non_ansi_port_max_line_length = 80
```

---

## `[format.enum_declaration]`

Controls alignment of enum member declarations.

### `align`

Align enum names and values across members.

```toml
[format.enum_declaration]
align = true
```

```systemverilog
// align = true
typedef enum logic [1:0] {
  IDLE    = 2'b00,
  ACTIVE  = 2'b01,
  DONE    = 2'b10
} state_t;

// align = false
typedef enum logic [1:0] {
  IDLE = 2'b00,
  ACTIVE = 2'b01,
  DONE = 2'b10
} state_t;
```

---

### `align_adaptive`

Per-member adaptive alignment instead of block-wide alignment.

```toml
[format.enum_declaration]
align_adaptive = false
```

```systemverilog
// align = true
// align_adaptive = true
typedef enum logic [1:0] {
    IDLE            = 2'b00,
    ACTIVE          = 2'b01,
    DONE            = 2'b10,
    VERY_LONG_TEXT  = 2'b11
} state_t;

// align = true
// align_adaptive = true
typedef enum logic [1:0] {
    IDLE    = 2'b00,
    ACTIVE  = 2'b01,
    DONE    = 2'b10
    VERY_LONG_TEXT = 2'b11
} state_t;
```

---

### `enum_name_min_width`

Minimum character width for the enum member name column.

```toml
[format.enum_declaration]
enum_name_min_width = 1
```

---

### `enum_value_min_width`

Minimum character width for the enum value column. Set to `0` to disable value-column alignment.

```toml
[format.enum_declaration]
enum_value_min_width = 0
```

---

## `[format.modport]`

Controls alignment of modport declarations inside interfaces.

### `align`

Align direction and signal columns across modport members.

```toml
[format.modport]
align = true
```

```systemverilog
// align = true
modport master (
  input  clk,
  input  rst_n,
  output valid
);

// align = false
modport master (
  input clk,
  input rst_n,
  output valid
);
```

---

### `align_adaptive`

Per-line adaptive alignment for modport entries.

```toml
[format.modport]
align_adaptive = false
```

---

### `direction_min_width`

Minimum character width for the direction column (`input`, `output`).

```toml
[format.modport]
direction_min_width = 1
```

---

### `signal_min_width`

Minimum character width for the signal name column.

```toml
[format.modport]
signal_min_width = 0
```

---

## Disable regions

The formatter respects inline disable comments. Everything between `// verilog_format: off` and `// verilog_format: on` is passed through verbatim. `` `define `` macro bodies are also passed through unchanged.

```systemverilog
// verilog_format: off
assign weird_spacing   =    preserved;
// verilog_format: on
```
