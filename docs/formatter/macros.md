# Formatter macro policy

SystemVerilog macro use is intentionally conservative.  A macro token can stand
for an expression, a statement, a declaration, a control-flow construct, a block
delimiter, or text whose whitespace is semantic.  The formatter cannot expand
macros, so users can classify known macro names in `lazyverilog.toml`:

```toml
[format.macros]
object_like_expr = ["MY_WIDTH"]
function_like_expr = ["MY_CLAMP"]
statement_like = ["uvm_info", "DV_CHECK_EQ"]
declaration_like = ["uvm_component_utils"]
control_flow_like = ["MY_IF"]
block_begin_like = ["uvm_object_utils_begin"]
block_end_like = ["uvm_object_utils_end"]
whitespace_sensitive = ["DV_SPINWAIT"]
```

Names may be written with or without the leading backtick.  A macro should appear
in at most one structural role list.  `whitespace_sensitive` is an additional
safety classification: it tells the formatter not to rewrite the invocation
arguments, even if the macro also behaves like another role in source.

## General rules

- Preprocessor directives such as `` `ifdef ``, `` `else ``, `` `endif ``, and
  single-line `` `define `` directives stay on their own physical lines.
- Multiline `` `define `` bodies are emitted verbatim.
- `// verilog_format: off` regions are emitted as one raw disabled-region body;
  comments, directives, macro calls, and blank lines inside the region are not
  interpreted by formatter passes.
- Macro calls are never expanded.  The role only describes how much surrounding
  formatting is safe.

## `object_like_expr`

Use this for object-like macros that behave like one expression token and do not
take an argument list.

```systemverilog
localparam int Width = `MY_WIDTH;
assign mask = '1 << `SHIFT_AMOUNT;
```

Policy:

- Treated as an ordinary expression token.
- May participate in normal spacing and alignment around neighboring operators.
- Does not force a line break by itself.

Example:

```systemverilog
assign y = `ENABLE_MASK & data;
```

## `function_like_expr`

Use this for macro calls that behave like expression-valued function calls.

```systemverilog
assign y = `MIN(a, b);
foo(`PACK(addr, data), valid);
```

Policy:

- Treated as an expression item.
- May be used as an argument in another call or list.
- The surrounding argument list can wrap according to normal function/list
  options when the macro is not whitespace-sensitive.
- Does not force a statement boundary.

Example:

```systemverilog
assign clipped = `CLAMP(value, lo, hi);
```

## `statement_like`

Use this for macros that form a complete procedural statement.

```systemverilog
`uvm_info(`gfn, "started", UVM_LOW)
`DV_CHECK_EQ(actual, expected)
```

Policy:

- Suppresses unsafe internal alignment of the macro token itself.
- Forces a line break after the invocation, normally after the matching
  parenthesized argument list when present.
- Does not open or close an indentation scope.

Example:

```systemverilog
initial begin
  `uvm_info(`gfn, "boot", UVM_LOW)
  state = Idle;
end
```

## `declaration_like`

Use this for macros that introduce or register declarations, commonly UVM utility
macros in class bodies.

```systemverilog
class my_seq extends uvm_sequence;
  `uvm_object_utils(my_seq)
endclass
```

Policy:

- Treated as a declaration-level item.
- Forces a line break after the invocation.
- Does not open or close an indentation scope.

Example:

```systemverilog
class my_env extends uvm_env;
  `uvm_component_utils(my_env)

  function new(string name, uvm_component parent);
    super.new(name, parent);
  endfunction
endclass
```

## `control_flow_like`

Use this for macros that behave like a control-flow statement but do not
themselves delimit a formatter indentation block.

```systemverilog
`MY_IF(enable)
  do_work();
```

Policy:

- Treated conservatively as a control-flow-like line.
- Suppresses unsafe internal alignment.
- Does not force a line break solely from the role today; add the macro to
  `statement_like` instead if it should always terminate a line after its
  invocation.
- Does not open or close an indentation scope.

Example:

```systemverilog
`ASSERT_IF(reset_done)
  `DV_CHECK_EQ(status, Good)
```

## `block_begin_like`

Use this for macros that begin a logical block or declaration sub-block.

```systemverilog
`uvm_object_utils_begin(my_item)
  `uvm_field_int(addr, UVM_DEFAULT)
```

Policy:

- Forces a line break after the invocation.
- Opens an indentation scope for following lines.
- Suppresses unsafe internal alignment of the macro token.

Example:

```systemverilog
class my_item extends uvm_object;
  `uvm_object_utils_begin(my_item)
    `uvm_field_int(addr, UVM_DEFAULT)
    `uvm_field_int(data, UVM_DEFAULT)
  `uvm_object_utils_end
endclass
```

## `block_end_like`

Use this for macros that close a logical block started by a matching macro.

```systemverilog
`uvm_object_utils_end
```

Policy:

- Closes one indentation scope before the macro is rendered.
- Forces a line break after the macro.
- Suppresses unsafe internal alignment of the macro token.

Example:

```systemverilog
`uvm_component_utils_begin(my_env)
  `uvm_field_object(cfg, UVM_DEFAULT)
`uvm_component_utils_end
```

## `whitespace_sensitive`

Use this when a macro's replacement text depends on the exact spelling,
linebreaks, or indentation of the arguments.

```systemverilog
`DV_SPINWAIT(
    fork
      do_a();
      do_b();
    join
)
```

Policy:

- The macro invocation's argument contents suppress wrapping and alignment.
- Nested calls inside the argument list are not independently expanded into
  multiline formatting.
- This is the safest role for macros whose arguments contain statement blocks,
  fork/join regions, constraints, preprocessor directives, or code intended for
  another tool.

Example:

```systemverilog
`MY_RAW_MACRO(
    if (cond) begin
      a = b;
    end
)
```

## Choosing a role

- If the macro substitutes a value, use `object_like_expr` or
  `function_like_expr`.
- If the macro is a complete procedural action, use `statement_like`.
- If the macro appears where a declaration appears, use `declaration_like`.
- If the macro changes indentation structure, use `block_begin_like` and
  `block_end_like`.
- If formatting the arguments could change meaning, add it to
  `whitespace_sensitive` or wrap the region in `// verilog_format: off` /
  `// verilog_format: on`.

