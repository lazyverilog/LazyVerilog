# Linter Options

All options live under `[lint]` in `lazyverilog.toml`.

Rules run as you type and are published as editor diagnostics. Only the file you edit is linted, not
its `` `include `` files. Slower semantic diagnostics are separate: see
[Background compilation](../diagnostics/background-compilation.md).

---

## Top-level options

### `enable`

Global kill-switch. When `false`, no lint rules run regardless of per-category settings.

```toml
[lint]
enable = true
```

---

## Severity levels

Every rule category exposes a `severity` field. The value controls how the LSP reports violations.

| Value | LSP severity | Editor appearance |
|-------|-------------|-------------------|
| `"error"` | 1 | red squiggly |
| `"warning"` | 2 | yellow squiggly |
| `"hint"` | 3 | subtle / grey |

---

## `[lint.style]`

### `trailing_whitespace`

Flags lines that end with spaces or tabs.

```toml
[lint.style]
trailing_whitespace = true
```

```systemverilog
// bad — space after semicolon
assign a = b;   

// good
assign a = b;
```

---

## `[lint.naming]`

Pattern-based naming convention checks. Each option takes a regex string. An empty string (`""`) disables that check.

```toml
[lint.naming]
enable = true
severity = "hint"
```

### `module_pattern`

Regex the module name must match.

```toml
[lint.naming]
module_pattern = "^m_.*$"
```

```systemverilog
// bad — no m_ prefix
module alu (input logic a, output logic b);
endmodule

// good
module m_alu (input logic a, output logic b);
endmodule
```

---

### `input_port_pattern`

Regex each `input` port name must match.

```toml
[lint.naming]
input_port_pattern = "^i_.*$"
```

```systemverilog
// bad
module m_top (input logic clk, input logic rst_n);
endmodule

// good
module m_top (input logic i_clk, input logic i_rst_n);
endmodule
```

---

### `output_port_pattern`

Regex each `output` port name must match.

```toml
[lint.naming]
output_port_pattern = "^o_.*$"
```

```systemverilog
// bad
module m_top (output logic valid);
endmodule

// good
module m_top (output logic o_valid);
endmodule
```

---

### `signal_pattern`

Regex each `logic` / `wire` / `var` signal declaration name must match.

```toml
[lint.naming]
signal_pattern = "^s_.*$"
```

```systemverilog
// bad
logic [7:0] data;

// good
logic [7:0] s_data;
```

---

### `register_pattern`

Regex names of signals assigned inside `always_ff` blocks must match.

```toml
[lint.naming]
register_pattern = "^r_.*$"
```

```systemverilog
// bad — assigned in always_ff but no r_ prefix
logic count;
always_ff @(posedge i_clk) begin
    count <= count + 1;
end

// good
logic r_count;
always_ff @(posedge i_clk) begin
    r_count <= r_count + 1;
end
```

---

### `interface_pattern`

Regex interface names must match.

```toml
[lint.naming]
interface_pattern = ".*_intf$"
```

```systemverilog
// bad
interface axi_bus ();
endinterface

// good
interface axi_intf ();
endinterface
```

---

### `struct_pattern`

Regex `typedef struct` names must match.

```toml
[lint.naming]
struct_pattern = ".*_t$"
```

```systemverilog
// bad
typedef struct packed {
    logic [7:0] data;
    logic       valid;
} packet;

// good
typedef struct packed {
    logic [7:0] data;
    logic       valid;
} packet_t;
```

---

### `union_pattern`

Regex `typedef union` names must match.

```toml
[lint.naming]
union_pattern = ".*_u$"
```

```systemverilog
// bad
typedef union packed {
    logic [7:0] raw;
    logic [3:0] nibbles [2];
} data_word;

// good
typedef union packed {
    logic [7:0] raw;
    logic [3:0] nibbles [2];
} data_word_u;
```

---

### `enum_pattern`

Regex `typedef enum` names must match. Empty string disables the check.

```toml
[lint.naming]
enum_pattern = ".*_e$"
```

```systemverilog
// bad
typedef enum logic [1:0] {
    IDLE, ACTIVE, DONE
} state;

// good
typedef enum logic [1:0] {
    IDLE, ACTIVE, DONE
} state_e;
```

---

### `parameter_pattern`

Regex `parameter` names must match.

```toml
[lint.naming]
parameter_pattern = "^W_.*$"
```

```systemverilog
// bad
module m_fifo #(parameter DATA_WIDTH = 8) ();
endmodule

// good
module m_fifo #(parameter W_DATA = 8) ();
endmodule
```

---

### `localparam_pattern`

Regex `localparam` names must match.

```toml
[lint.naming]
localparam_pattern = "^LP_.*$"
```

```systemverilog
// bad
localparam DEPTH = 16;

// good
localparam LP_DEPTH = 16;
```

---

### `check_module_filename`

When `true`, the module name must match the file stem (filename without extension).

```toml
[lint.naming]
check_module_filename = true
```

```systemverilog
// file: m_alu.sv
// bad — name mismatch
module m_adder (...);
endmodule

// good
module m_alu (...);
endmodule
```

---

### `check_package_filename`

When `true`, the package name must match the file stem.

```toml
[lint.naming]
check_package_filename = true
```

```systemverilog
// file: m_alu_pkg.sv
// bad
package utils_pkg;
endpackage

// good
package m_alu_pkg;
endpackage
```

---

## `[lint.module]`

```toml
[lint.module]
enable = true
severity = "warning"
```

### `one_module_per_file`

Flags files that declare more than one module.

```toml
[lint.module]
one_module_per_file = true
```

```systemverilog
// bad — two modules in one file
module m_foo ();
endmodule

module m_bar ();
endmodule

// good — one module per file
module m_foo ();
endmodule
```

---

## `[lint.instance]`

```toml
[lint.instance]
enable = true
severity = "warning"
```

### `module_instantiation_style`

Enforces a consistent port connection style across all instantiations.

Values: `"positional"` | `"named"` | `"both"` | `""` (disabled)

- `"positional"` — all connections must be positional
- `"named"` — all connections must be named (`.port(signal)`)
- `"both"` — mixing positional and named in the same instance is forbidden

```toml
[lint.instance]
module_instantiation_style = "named"
```

```systemverilog
// bad — positional when "named" required
m_fifo u_fifo(i_clk, i_rst_n, i_data, o_data);

// good
m_fifo u_fifo (
    .i_clk   (i_clk  ),
    .i_rst_n (i_rst_n),
    .i_data  (i_data ),
    .o_data  (o_data )
);
```

---

### `stale_instance_diagnostic`

Flags port connections that no longer match the instantiated module's port list — missing ports, duplicate connections, or connections to ports that no longer exist.

```toml
[lint.instance]
stale_instance_diagnostic = true
```

```systemverilog
// m_fifo has ports: i_clk, i_data, o_data
// bad — o_data missing, unknown port i_unused connected
m_fifo u_fifo (
    .i_clk   (i_clk  ),
    .i_data  (i_data ),
    .i_unused(i_unused)  // port doesn't exist
    // o_data missing
);
```

---

## `[lint.statement]`

```toml
[lint.statement]
enable = true
severity = "warning"
```

### `no_raw_always`

Forbids bare `always` blocks. Use `always_comb`, `always_ff`, or `always_latch` instead.

```toml
[lint.statement]
no_raw_always = true
```

```systemverilog
// bad
always @(posedge i_clk) begin
    r_q <= i_d;
end

// good
always_ff @(posedge i_clk) begin
    r_q <= i_d;
end
```

---

### `blocking_nonblocking_assignments`

Enforces:
- `always_ff` uses nonblocking assignments (`<=`)
- `always_comb` uses blocking assignments (`=`)

```toml
[lint.statement]
blocking_nonblocking_assignments = true
```

```systemverilog
// bad — blocking in always_ff
always_ff @(posedge i_clk) begin
    r_q = i_d;
end

// bad — nonblocking in always_comb
always_comb begin
    s_next <= r_state + 1;
end

// good
always_ff @(posedge i_clk) begin
    r_q <= i_d;
end

always_comb begin
    s_next = r_state + 1;
end
```

---

### `latch_inference_detection`

Flags incomplete `if` statements inside `always_comb` that may infer a latch because not all paths assign the output.

```toml
[lint.statement]
latch_inference_detection = true
```

```systemverilog
// bad — no else branch, s_out may hold if i_en is 0
always_comb begin
    if (i_en) begin
        s_out = i_data;
    end
end

// good — all paths covered
always_comb begin
    if (i_en) begin
        s_out = i_data;
    end else begin
        s_out = '0;
    end
end
```

---

### `case_missing_default`

Requires a `default` item in every `case` statement.

```toml
[lint.statement]
case_missing_default = true
```

```systemverilog
// bad
always_comb begin
    case (r_state)
        2'b00: s_out = i_a;
        2'b01: s_out = i_b;
    endcase
end

// good
always_comb begin
    case (r_state)
        2'b00:   s_out = i_a;
        2'b01:   s_out = i_b;
        default: s_out = '0;
    endcase
end
```

---

### `explicit_begin`

Requires `begin`/`end` blocks for `if`, `else`, `for`, `while`, and `foreach` bodies.

```toml
[lint.statement]
explicit_begin = true
```

```systemverilog
// bad — single-statement body without begin/end
always_comb begin
    if (i_en)
        s_out = i_data;
end

// good
always_comb begin
    if (i_en) begin
        s_out = i_data;
    end
end
```

---

## `[lint.function]`

```toml
[lint.function]
enable = true
severity = "warning"
```

### `explicit_function_lifetime`

Requires functions to explicitly declare their lifetime (`automatic` or `static`).

```toml
[lint.function]
explicit_function_lifetime = true
```

```systemverilog
// bad — no lifetime specified
function logic [7:0] clamp(input logic [7:0] val);
    return val > 8'hF0 ? 8'hF0 : val;
endfunction

// good
function automatic logic [7:0] clamp(input logic [7:0] val);
    return val > 8'hF0 ? 8'hF0 : val;
endfunction
```

---

### `explicit_task_lifetime`

Requires tasks to explicitly declare their lifetime (`automatic` or `static`).

```toml
[lint.function]
explicit_task_lifetime = true
```

```systemverilog
// bad
task drive_bus(input logic [7:0] data);
    i_data <= data;
    @(posedge i_clk);
endtask

// good
task automatic drive_bus(input logic [7:0] data);
    i_data <= data;
    @(posedge i_clk);
endtask
```

---

### `functions_automatic`

When `true`, flags any function that does not use `automatic` lifetime specifically (stricter than `explicit_function_lifetime`).

```toml
[lint.function]
functions_automatic = true
```

```systemverilog
// bad — static lifetime
function static logic [7:0] clamp(input logic [7:0] val);
    return val;
endfunction

// good
function automatic logic [7:0] clamp(input logic [7:0] val);
    return val;
endfunction
```

---

### `function_call_style`

Enforces a consistent argument style across all function and task calls.

Values: `"positional"` | `"named"` | `"both"` | `""` (disabled)

- `"positional"` — all arguments must be positional
- `"named"` — all arguments must be named (`.port(value)`)
- `"both"` — mixing positional and named in the same call is forbidden

```toml
[lint.function]
function_call_style = "named"
```

```systemverilog
// bad — positional when "named" required
logic [7:0] s_result = clamp(i_val, 8'hF0);

// good
logic [7:0] s_result = clamp(.val(i_val), .limit(8'hF0));
```

---

## Diagnostic codes

Every rule has a code such as `lint-naming-input-port`, shown in brackets by `lazyverilog-lint`. Codes
are hierarchical (`lint-naming` covers every naming rule) and `lazyverilog-lint --help` lists them
all. To silence a rule for the whole project, use its config key above. To silence it for one run, use
[`--nowarn`](cli.md#diagnostic-codes).

---

## Complete example configuration

```toml
[lint]
enable = true

[lint.style]
trailing_whitespace = true

[lint.naming]
enable = true
severity = "hint"
module_pattern        = "^m_.*$"
input_port_pattern    = "^i_.*$"
output_port_pattern   = "^o_.*$"
signal_pattern        = "^s_.*$"
register_pattern      = "^r_.*$"
interface_pattern     = ".*_intf$"
struct_pattern        = ".*_t$"
union_pattern         = ".*_u$"
enum_pattern          = ".*_e$"
parameter_pattern     = "^W_.*$"
localparam_pattern    = "^LP_.*$"
check_module_filename = true
check_package_filename = true

[lint.module]
enable = true
severity = "warning"
one_module_per_file = true

[lint.instance]
enable = true
severity = "warning"
module_instantiation_style = "named"
stale_instance_diagnostic  = true

[lint.statement]
enable = true
severity = "warning"
no_raw_always                  = true
blocking_nonblocking_assignments = true
latch_inference_detection      = true
case_missing_default           = true
explicit_begin                 = true

[lint.function]
enable = true
severity = "warning"
explicit_function_lifetime = true
explicit_task_lifetime     = true
functions_automatic        = false
function_call_style        = "named"
```
