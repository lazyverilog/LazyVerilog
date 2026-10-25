# Formatter macro policy

The formatter cannot expand macros, so it treats them conservatively. Tell it what your macros are
in `lazyverilog.toml`:

```toml
[format.macros]
object_like_expr = ["MY_WIDTH"]
function_like_expr = ["MY_CLAMP"]
statement_like = ["DV_CHECK_EQ"]
declaration_like = ["MY_REG_FIELD"]
control_flow_like = ["MY_IF"]
statement_terminator_like = ["SEMI"]
block_begin_like = ["MY_BLOCK_BEGIN"]
block_end_like = ["MY_BLOCK_END"]
whitespace_sensitive = ["DV_CHECK_FATAL"]
```

Names work with or without the leading backtick. Put a macro in one role only.
`whitespace_sensitive` is an extra safety flag that can go with any role.
The text between the macro's parentheses is kept byte for byte: gaps, line breaks and
comments. The invocation itself is still placed and indented by its role.

No macro is whitespace-sensitive by default. OpenTitan's `DV_CHECK_FATAL`, for example, takes
an expression it stringifies into its error message, so an OpenTitan project lists it as above.

## Built-in defaults

The formatter already knows the UVM macros below. Your lists **extend** these defaults; they do
not replace them. A built-in macro cannot be removed or moved to another role: listing it under a
different role is reported as an error ("`uvm_object_utils` is a built-in declaration_like
macro; built-in roles cannot be changed"). Listing it again under its own role is harmless.

| Role | Built-in macros |
|------|-----------------|
| `statement_like` | `uvm_info`, `uvm_warning`, `uvm_error`, `uvm_fatal`, `uvm_create`, `uvm_create_on`, `uvm_send`, `uvm_send_pri`, `uvm_rand_send`, `uvm_rand_send_with`, `uvm_rand_send_pri`, `uvm_rand_send_pri_with`, `uvm_do`, `uvm_do_with`, `uvm_do_on`, `uvm_do_on_with`, `uvm_do_pri`, `uvm_do_pri_with`, `uvm_do_on_pri`, `uvm_do_on_pri_with`, `uvm_field_int`, `uvm_field_enum`, `uvm_field_object`, `uvm_field_array_int`, `uvm_field_array_object`, `uvm_field_queue_int`, `uvm_field_queue_object`, `uvm_field_string` |
| `declaration_like` | `uvm_object_utils`, `uvm_component_utils`, `uvm_object_param_utils`, `uvm_component_param_utils`, `uvm_sequence_utils` |
| `block_begin_like` | `uvm_object_utils_begin`, `uvm_component_utils_begin`, `uvm_object_param_utils_begin`, `uvm_component_param_utils_begin`, `uvm_sequence_utils_begin` |
| `block_end_like` | `uvm_object_utils_end`, `uvm_component_utils_end`, `uvm_object_param_utils_end`, `uvm_component_param_utils_end`, `uvm_sequence_utils_end` |

`object_like_expr`, `function_like_expr`, `control_flow_like`, `statement_terminator_like` and
`whitespace_sensitive` have no built-in entries.

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
