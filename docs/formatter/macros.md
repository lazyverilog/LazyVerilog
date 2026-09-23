# Formatter macro policy

The formatter cannot expand macros, so it treats them conservatively. Tell it what your macros are
in `lazyverilog.toml`:

```toml
[format.macros]
object_like_expr = ["MY_WIDTH"]
function_like_expr = ["MY_CLAMP"]
statement_like = ["uvm_info", "DV_CHECK_EQ"]
declaration_like = ["uvm_component_utils"]
control_flow_like = ["MY_IF"]
statement_terminator_like = ["SEMI"]
block_begin_like = ["uvm_object_utils_begin"]
block_end_like = ["uvm_object_utils_end"]
whitespace_sensitive = ["DV_SPINWAIT"]
```

Names work with or without the leading backtick. Put a macro in one role only.
`whitespace_sensitive` is an extra safety flag that can go with any role.

## Roles

| Role | Use for | Effect |
|------|---------|--------|
| `object_like_expr` | A macro with no arguments that stands for a value: `` `MY_WIDTH `` | Formatted like an ordinary operand |
| `function_like_expr` | A macro call that yields a value: `` `MIN(a, b) `` | An expression item that can sit in argument lists |
| `statement_like` | A complete statement: `` `uvm_info(...) `` | Line break after it |
| `declaration_like` | Declaration-level macros: `` `uvm_object_utils(T) `` | Line break after it |
| `control_flow_like` | A macro used like `if`: `` `MY_IF(en) `` | No alignment inside; no forced break. Use `statement_like` if it should end the line |
| `statement_terminator_like` | A macro that expands to a statement's `;`: `` `define SEMI ; `` | Ends the statement it closes: line break after it, and the next statement is formatted as a new one |
| `block_begin_like` | Opens a block: `` `uvm_object_utils_begin `` | Line break, then indents what follows |
| `block_end_like` | Closes that block | Un-indents, then line break |
| `whitespace_sensitive` | Arguments whose exact spacing matters | Arguments are left exactly as written |

```systemverilog
class my_item extends uvm_object;
  `uvm_object_utils_begin(my_item)
    `uvm_field_int(addr, UVM_DEFAULT)
    `uvm_field_int(data, UVM_DEFAULT)
  `uvm_object_utils_end
endclass
```

## Always true

- `` `ifdef ``, `` `else ``, `` `endif ``, and single-line `` `define `` stay on their own lines.
- Multiline `` `define `` bodies are left verbatim.
- Text between `// verilog_format: off` and `// verilog_format: on` is left verbatim.

## Choosing a role

- Substitutes a value: `object_like_expr` or `function_like_expr`.
- A complete action: `statement_like`.
- Appears where a declaration would: `declaration_like`.
- Changes indentation: `block_begin_like` and `block_end_like`.
- Formatting its arguments could change the meaning: `whitespace_sensitive`, or wrap the region in
  `// verilog_format: off` / `on`.
