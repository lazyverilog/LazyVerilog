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

Controls spacing inside index and selection expressions.

```systemverilog
a[i+1]      // true
a[i + 1]    // false
```

### `keyword_case`

| type | default | values |
|------|---------|--------|
| string | `"preserve"` | `"preserve"`, `"lower"`, `"upper"` |

Controls output casing for SystemVerilog keywords.

### `blank_lines_between_items`

| type | default |
|------|---------|
| int | `1` |

Maximum number of blank lines preserved between items.

### `default_indent_level_inside_module_block`

| type | default |
|------|---------|
| int | `1` |

Extra indentation levels applied inside `module ... endmodule`.

### `tab_align`

| type | default |
|------|---------|
| bool | `false` |

Stored in config and used by some alignment-related behavior in the broader
formatter configuration.

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

## `[format.port]`

```toml
[format.port]
non_ansi_port_per_line_enabled = false
non_ansi_port_per_line = 1
non_ansi_port_max_line_length_enabled = false
non_ansi_port_max_line_length = 80
```

Controls formatting of non-ANSI module header port lists.

### `non_ansi_port_per_line_enabled`
### `non_ansi_port_per_line`

When enabled, emits a fixed number of ports per line.

### `non_ansi_port_max_line_length_enabled`
### `non_ansi_port_max_line_length`

When enabled, packs non-ANSI ports until the configured maximum line length
would be exceeded.
