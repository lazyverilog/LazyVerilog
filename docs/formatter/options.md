# LazyVerilog Formatter Options

All formatter options live under `[format]` or one of its child sections in
`lazyverilog.toml`.

This document covers the formatter configuration currently implemented in the
C++ server.

## `[format]`

### `indent_size`

| type | default |
|------|---------|
| int | `2` |

Number of spaces per indentation level.

### `compact_indexing_and_selections`

| type | default |
|------|---------|
| bool | `true` |

Legacy option retained for compatibility. New configs should prefer `[format.spacing].dimension_binary_operator_spacing`.

### `blank_lines_between_items`

| type | default |
|------|---------|
| int | `1` |

Maximum number of blank lines preserved between items.

### `default_indent_level_inside_outmost_block`

| type | default |
|------|---------|
| int | `1` |

Extra indentation levels applied inside outmost `module`, `interface`, and `package` blocks.

### `tab_align`

| type | default |
|------|---------|
| bool | `false` |

When enabled, alignment columns are snapped up to the next multiple of
`indent_size`. This affects aligned statements, port declarations, variable
declarations, enum declarations, modports, and fixed-mode instance port
connections. It still uses spaces; no literal tab characters are inserted.

### `align_punctuation`

| type | default |
|------|---------|
| bool | `false` |

Stored in config. This codebase currently parses the option, but the formatter
pass documented here focuses on statement, declaration, instance, port-list,
and function/task-call formatting.

### `enable_format_on_save`

| type | default |
|------|---------|
| bool | `false` |

Editor integration setting.

### `safe_mode`

| type | default |
|------|---------|
| bool | `false` |

When `true`, formatting aborts if non-whitespace content changes.

### `trailing_newline`

| type | default |
|------|---------|
| bool | `true` |

Controls whether formatted output ends with a trailing newline.

## `[format.spacing]`

```toml
[format.spacing]
control_keyword_space = true
space_inside_parens = false
space_inside_dimension_brackets = false
binary_operator_spacing = "both"
dimension_binary_operator_spacing = "none"
semicolon_spacing = "after"
range_colon_spacing = "none"
indexed_part_select_spacing = "both"
procedural_event_control_at_spacing = "before"
space_inside_event_control_parens = false
assignment_operator_spacing = "both"
```

Spacing mode values are `"none"`, `"before"`, `"after"`, and `"both"`.

| option | default | effect |
|--------|---------|--------|
| `control_keyword_space` | `true` | Space between control-flow keywords and their header paren: `if (a)` vs `if(a)`. Applies to `if`, `else if`, `for`, `foreach`, `while`, `repeat`, `case`, `casex`, and `casez`; not function/task/system calls. |
| `space_inside_parens` | `false` | Space just inside ordinary/control/grouping parentheses: `( a + b )`. Function/task/system call parens are excluded. |
| `space_inside_dimension_brackets` | `false` | Space just inside packed/unpacked dimensions and select brackets: `[ WIDTH-1:0 ]`, `arr[ i ]`. |
| `binary_operator_spacing` | `"both"` | Spacing around binary operators outside `[]`, excluding assignment and unary operators. |
| `dimension_binary_operator_spacing` | `"none"` | Spacing around binary operators inside dimensions/selects, excluding range colons, indexed part-select operators, and unary operators. |
| `semicolon_spacing` | `"after"` | Spacing around semicolons in `for`/`foreach` headers only. Statement terminators are not affected. |
| `range_colon_spacing` | `"none"` | Spacing around range/select colons inside `[]`: `[hi:lo]`. Indexed part-select `+:`/`-:` is excluded. |
| `indexed_part_select_spacing` | `"both"` | Spacing around indexed part-select operators as one unit: `arr[i +: 4]`. |
| `procedural_event_control_at_spacing` | `"before"` | Spacing around `@` in procedural event controls after `always`, `always_ff`, `always_comb`, or `always_latch`. Standalone event controls are excluded. |
| `space_inside_event_control_parens` | `false` | Space just inside procedural event-control parentheses: `always @( posedge clk )`. Normal parentheses are excluded. |
| `assignment_operator_spacing` | `"both"` | Spacing around assignment operators (`=`, `<=`): `a = b` vs `a=b`. |

Examples:

```systemverilog
// defaults
if (a + b)
logic [WIDTH-1:0] data;
arr[i+: 4]
always @(posedge clk)

// selected alternatives
if( a+b )
logic [ WIDTH - 1 : 0 ] data;
arr[ i +: 4 ]
always @ ( posedge clk )
```


## `[format.statement]`

```toml
[format.statement]
align = false
align_adaptive = false
lhs_min_width = 1
wrap_end_else_clauses = false
```

### `align`

| type | default |
|------|---------|
| bool | `false` |

Aligns consecutive assignment operators such as `=` and `<=`.

### `align_adaptive`

| type | default |
|------|---------|
| bool | `false` |

When `false`, a group of aligned assignments shares one operator column.

When `true`, each line uses at least `lhs_min_width`, but long left-hand sides
do not force the whole group wider.

### `lhs_min_width`

| type | default |
|------|---------|
| int | `1` |

Minimum left-hand-side width used by statement alignment.

### `wrap_end_else_clauses`

| type | default |
|------|---------|
| bool | `false` |

Controls whether `end else` stays on one line or breaks across lines.

## `[format.port_declaration]`

```toml
[format.port_declaration]
align = true
align_adaptive = false
section1_min_width = 10
section2_min_width = 20
section3_min_width = 20
section4_min_width = 30
section5_min_width = 30
```

Port declarations are aligned in contiguous blocks across five sections:

| Section | Content |
|---------|---------|
| 1 | direction keyword |
| 2 | type and signing |
| 3 | packed dimension |
| 4 | identifier |
| 5 | unpacked dimension and default value |

### `align`

| type | default |
|------|---------|
| bool | `true` |

Enables the port declaration alignment pass.

### `align_adaptive`

| type | default |
|------|---------|
| bool | `false` |

When `false`, a block uses shared maximum widths so columns line up vertically.

When `true`, each line uses the configured minimum widths independently. Long
content keeps only the minimum required separating spaces instead of widening
the whole block.

### `section1_min_width` ... `section5_min_width`

| type | default |
|------|---------|
| int | see example above |

Minimum widths for the five port-declaration sections.

Example:

```systemverilog
input      logic        [7:0]        i_data;
output     logic signed [15:0]       o_result;
input      i_clk;
```

## `[format.var_declaration]`

```toml
[format.var_declaration]
align = false
align_adaptive = false
section1_min_width = 0
section2_min_width = 30
section3_min_width = 30
section4_min_width = 0
```

Variable declarations are aligned in contiguous blocks across four sections:

| Section | Content |
|---------|---------|
| 1 | type and optional signing |
| 2 | packed dimension |
| 3 | declarator |
| 4 | unpacked dimension and initializer |

### `align`

| type | default |
|------|---------|
| bool | `false` |

Enables the variable declaration alignment pass.

### `align_adaptive`

| type | default |
|------|---------|
| bool | `false` |

Uses the same adaptive idea as port declarations: configured minimum widths are
applied per line, and oversized content does not force the rest of the block to
shift.

### `section1_min_width` ... `section4_min_width`

| type | default |
|------|---------|
| int | see example above |

Minimum widths for the four variable-declaration sections.

## `[format.function_call]`

Formats function and task calls.

```toml
[format.function_call]
break_policy = "auto"      # never | always | auto
line_length = 100
arg_count = null
layout = "block"           # hanging | block
indent_width = 4
space_before_paren = false
space_inside_paren = false
```

### `break_policy`

| type | default | values |
|------|---------|--------|
| string | `"auto"` | `"never"`, `"always"`, `"auto"` |

- `"never"` keeps calls on one line.
- `"always"` breaks every recognized function/task call.
- `"auto"` breaks when either `line_length` or `arg_count` says to break.

### `line_length`

| type | default |
|------|---------|
| int | `100` |

Used only when `break_policy = "auto"`. If the rendered call would exceed this
length, it breaks.

### `arg_count`

| type | default |
|------|---------|
| int or `null` | `null` |

Used only when `break_policy = "auto"`. If not `null`, calls break when the
argument count is greater than or equal to this value.

In the current loader, `null` means "disabled".

### `layout`

| type | default | values |
|------|---------|--------|
| string | `"block"` | `"block"`, `"hanging"` |

Block style:

```systemverilog
result = my_func(
    arg1,
    arg2,
    arg3
);
```

Hanging style:

```systemverilog
result = my_func(arg1,
                 arg2,
                 arg3);
```

### `indent_width`

| type | default |
|------|---------|
| int | `4` |

Used only by `layout = "block"`. Controls indentation for broken argument
lines.

### `space_before_paren`

| type | default |
|------|---------|
| bool | `false` |

Controls `my_func(` vs `my_func (`.

### `space_inside_paren`

| type | default |
|------|---------|
| bool | `false` |

Controls `my_func(a, b)` vs `my_func( a, b )` for unbroken calls.

## `[format.instance]`

```toml
[format.instance]
align = false
port_indent_level = 1
instance_port_name_width = 1
instance_port_between_paren_width = 0
align_adaptive = false
```

Formats named instance port connections into aligned multi-line blocks.

### `align`

| type | default |
|------|---------|
| bool | `false` |

Enables instance port formatting for named connections.

### `port_indent_level`

| type | default |
|------|---------|
| int | `1` |

Indent levels added for each port line inside the instance block.

### `instance_port_name_width`

| type | default |
|------|---------|
| int | `1` |

Minimum width used for the port name section.

### `instance_port_between_paren_width`

| type | default |
|------|---------|
| int | `0` |

Minimum width used for the signal text inside parentheses.

### `align_adaptive`

| type | default |
|------|---------|
| bool | `false` |

When `false`, all named ports in the instance align to common columns.

When `true`, each line falls back to the configured minimum widths
independently, so unusually long names do not widen the whole instance.

Compatibility note:

- Old configs may still use `align_instance_port_adaptive`.
- New configs should use `align_adaptive`.

## `[format.module]`

```toml
[format.module]
parameter_layout = "block"
non_ansi_port_per_line_enabled = false
non_ansi_port_per_line = 1
non_ansi_port_max_line_length_enabled = false
non_ansi_port_max_line_length = 80
```

Controls formatting of module parameter lists and non-ANSI module header port lists.

### `parameter_layout`

| type | default | values |
|------|---------|--------|
| string | `"block"` | `"block"`, `"hanging"` |

Controls formatting of `#(...)` module parameter lists.

`"block"` puts each parameter on its own indented line:

```systemverilog
module register #(
    parameter type T = logic [7:0],
    parameter int DEPTH = 8
)(
```

`"hanging"` aligns subsequent parameters under the first parameter:

```systemverilog
module register #(parameter type T = logic [7:0],
                  parameter int DEPTH = 8)(
```

### `non_ansi_port_per_line_enabled`
### `non_ansi_port_per_line`

When enabled, emits a fixed number of ports per line.

### `non_ansi_port_max_line_length_enabled`
### `non_ansi_port_max_line_length`

When enabled, packs non-ANSI ports until the configured maximum line length
would be exceeded.

## `[format.enum_declaration]`

```toml
[format.enum_declaration]
align = false
align_adaptive = false
enum_name_min_width = 1
enum_value_min_width = 0
```

Formats `typedef enum ... { ... } name;` declarations into one enumerator per
line. Enumerator lines are always indented by one formatter indent level.

When `align = false`, enumerators are left-aligned with compact assignments:

```systemverilog
IDLE,
BUSY = 2'd1,
DONE
```

When `align = true`, `enum_name_min_width` and `enum_value_min_width` define
minimum columns for enumerator names and assigned values. With
`align_adaptive = false`, long names or values widen the whole enum block. With
`align_adaptive = true`, long entries only affect their own line. If
`[format].tab_align = true`, aligned enum columns are snapped to the
`indent_size` grid.

## `[format.modport]`

```toml
[format.modport]
align = false
align_adaptive = false
direction_min_width = 1
signal_min_width = 0
```

Formats interface `modport` declarations into multi-line port lists. Port lines
are always indented by one formatter indent level.

When `align = false`, modport items are left-aligned:

```systemverilog
input clk,
output data
```

When `align = true`, `direction_min_width` and `signal_min_width` define minimum
columns for directions and signal names. With `align_adaptive = false`, long
strings widen the whole modport block. With `align_adaptive = true`, long
strings only affect their own line. If `[format].tab_align = true`, aligned
modport columns are snapped to the `indent_size` grid.

## Currently unsupported or limited SystemVerilog formatter syntax

The formatter is intentionally conservative. The following constructs are not
fully syntax-aware yet and may be left mostly as token-spaced text, or should be
wrapped in `// verilog_format: off` / `on` if the exact layout matters:

- UVM class-heavy code: macros such as `` `uvm_component_utils``, factory calls,
  constraints, and long phase/task bodies.
- Classes, covergroups, constraints, properties, sequences, checkers, and SVA
  assertion expressions beyond basic indentation/token spacing.
- Complex preprocessor macro bodies and generated code; `` `define`` bodies are
  deliberately preserved.
- Parameter/localparam declaration alignment beyond the generic token and
  assignment passes.
- Struct/union field alignment beyond generic variable declaration handling.
- Multi-line comments/docblocks: preserved, not reflowed.
- Advanced module/interface/program headers with complex parameter-port lists or
  preprocessor conditionals inside the header.
- Positional instance ports and complex named connections containing comments or
  preprocessor conditionals.
- `generate`/`genvar` formatting is basic indentation only.
- Specify blocks, primitives, UDP tables, configs, clocking blocks, net aliases,
  bind statements, and package import/export grouping are not specially aligned.
