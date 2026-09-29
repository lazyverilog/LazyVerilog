# Formatter Bug Report — Round 6

Findings from a sixth stress test of `lazyverilog-fmt` on `fix/formatter-stress-bugs`
at `03cdcbf`, **plus the uncommitted port/var section-5 change** in the working tree.

## What was tested

12 new RTL files in styles rounds 1–5 did not cover. Each ran under three configs
(**none**, **repo** = the repo's `lazyverilog.toml`, **alt** = the round-5 alt config),
then again as CRLF + tab-indented copies. About 25 single-option repros followed.

- **UVM testbench**: `` `uvm_*`` macros, `uvm_config_db#(T)::get`, `randomize() with`,
  `constraint` with `dist`/`solve…before`/`soft`/`foreach`, `fork…join_any`, `randcase`.
- **Generated netlists**: CIRCT/Chisel (`_RANDOM[/*Zero width*/ …]`, nested `` `ifdef``,
  long flat ternary chains) and Yosys (escaped identifiers, `(* src *)`, `\$paramod…`,
  `SB_LUT4 #(…)`, gate primitives with delays).
- **UDP `primitive`/`table`, `specify`** with min:typ:max delays and timing checks.
- **Expressions**: streaming `{<<8{…}}`, casts `T'()`/`type(a)'()`, `inside` ranges,
  `+:`/`-:`, `'{default:…}`, `'{4{…}}`, compound assignments, relational `<=`.
- **Interface / clocking / modport import / program / checker / config**, plus `let`,
  `sequence` and `property` with formal arguments.
- **Generate**: `for`/`if`/`case` generate with labels, instance arrays, `.*`,
  positional instances with empty ports, `defparam`, and a header `import` before the port list.
- **Directives**: multi-line `` `define `` with `` `` `` pasting, `` `" ``, `` `line``,
  `` `pragma``, `` `ifdef`` inside a brace-less `if`.
- **Procedural**: intra-assignment timing (`q <= repeat (2) @(posedge clk) d`),
  `->>`, `wait fork`, `force`/`release`, DPI import/export.
- **Legacy Verilog-2001** with no whitespace (`always@(posedge clk)if(rst)begin…`).
- **Input styles**: Allman `begin`, one-line `begin … end`, trailing comments.

## Safety results

- No crashes.
- **Every file is idempotent under every config**: 72 of 72 runs, LF and CRLF.
- With whitespace ignored, the token stream is unchanged everywhere. That check
  is blind to **N-1**, though: on a CRLF buffer a multi-line `` `define `` is
  rewritten, and the output no longer preprocesses the same way.

## Excluded (settled in earlier rounds, or pinned by tests)

Not reported: `wait(c)`, `'{key : value}`, `begin: label` / `endclass: name`,
`assert … else` on three lines, `0:;`, `blank_lines_between_items`, joined user line breaks
in expressions, a brace-less body moved to its own line (`initial`/`repeat`/`if`), `` `ifdef``
at column 0, block call layout anchored to the callee (K-1), attributes joined onto their item
(pinned in `test_formatter_regressions.cpp`), and non-adaptive alignment with `lhs_min_width`.

## Summary

| ID | Severity | Kind | Config | Issue |
|----|----------|------|--------|-------|
| N-1 | **Critical** | layout (semantics) | none (CRLF input) | A multi-line `` `define `` in a CRLF buffer is reformatted: the `\` moves to its own line and `` `` `` gets spaces. This breaks the macro |
| N-2 | High | align | `var_declaration.align` (default widths) | With `section1_min_width = 0` (the default), names don't line up and a multi-name declaration puts its **last** name in the name column |
| N-3 | High | wrap | `function_call` auto + `line_length` | A named parameter override `.P(16)` in a long `#(…)` is broken like a call and hung at column 98 |
| N-4 | Medium | layout | none | Intra-assignment `q <= repeat (2) @(posedge clk) d;` is split like a `repeat` loop |
| N-5 | Medium | wrap | `function_call.line_length` | A call in a brace-less `if`/`for`/`always` body is measured on the header's line, so it breaks even though its own line fits |
| N-6 | Medium | wrap | `break_policy = "always"` / `arg_count` | `sequence`, `property` and `let` formal-argument lists are broken like call arguments |
| N-7 | Medium | align | hanging layouts + `space_inside_paren(s)` | Hanging continuation lines sit one column left of the first argument |
| N-8 | Medium | indent | `default_indent_level_inside_outmost_block = 1` (default) | After a header `import`, the port-list `(` is indented to body level, level with its ports |
| N-9 | Low | spacing | none | After a macro line with no `;`, a select target is spaced like a declaration: `mem [0] = 1;` |
| N-10 | Low | align/spacing | `var_declaration.align` | `rand`/`randc` properties are skipped by declaration alignment and J-15 spacing (`rand int a[];`) |
| N-11 | Low | spacing | `binary_operator_spacing` / `assignment_operator_spacing` | A relational `<=` on the right-hand side is spaced as an assignment |
| N-12 | Low | spacing | `procedural_event_control_at_spacing = "none"` | `@` gets glued to what precedes it: `q <=@(posedge clk) d`, `wait(a)@(…)` |
| N-13 | Low | spacing | none | `'{4{8'hAA}}` → `'{4 {8'hAA}}`, while `{4{8'hAA}}` is left alone |
| N-14 | Low | spacing | none | min:typ:max colons are spaced like a ternary: `#(1 : 2 : 3)` |
| N-15 | Low | spacing | `binary_operator_spacing = "none"` | A unary operator after a binary one keeps a space: `a* -b+c& ~d` |
| N-16 | Low | spacing | none | Instance array `u_arr[3:0] (` vs `u_one(`: space before `(` is inconsistent |
| N-17 | Low | align | `enum_declaration.align` | The last enum item's trailing comment is not aligned with the others |

Kinds follow rounds 4–5: *indent*, *layout* (line breaks), *wrap* (line-length or policy
breaks), *align* (column padding), *spacing*. "none" means no `lazyverilog.toml`.

---

## N-1 — Multi-line `` `define `` in a CRLF buffer is rewritten

**Kind:** layout (changes preprocessing). **Options:** none. The trigger is `\r\n` line endings.
LF input is fine, and so are tabs.

Since L-17 the CLI reads the file in binary mode, so this is `format_source()` on a CRLF
buffer. That is exactly what VS Code on Windows sends.

Before (CRLF):
```systemverilog
`define REG(n) \
  logic n``_q; \
  logic n``_d;
```
After (current):
```systemverilog
`define REG(n) \
logic n `` _q;
\
 logic n `` _d;
```
Now the macro ends after its first body line. A stray `\` and `` logic n `` _d; `` are left
as module text, and `` `` `` has spaces around it. The output no longer compiles.

Expected: the macro body is verbatim, byte for byte, as it is for LF input.

Most likely cause: the continuation test expects `\` right before `\n`, so `\` `\r` `\n` is
not recognised as a continuation.

## N-2 — Declaration alignment breaks with `section1_min_width = 0` (the default)

**Kind:** align. **Options:** `[format.var_declaration] align = true` with the other keys left
at their defaults (`section1_min_width = 0`, `section2/3 = 30`, `section4 = 0`). This happens
with `align_adaptive` set either way. `section1_min_width = 0` alone is enough to trigger it.

Before:
```systemverilog
logic valid, ready;
logic [7:0] s0, s1, s2;
logic x;
int unsigned yy;
logic [3:0] mem [4];
```
After (current):
```systemverilog
  logic valid,                        ready                         ;
  logic [7:0] s0, s1,                 s2                            ;
  logic                               x                             ;
  int unsigned                      yy                            ;
  logic [3:0]                         mem [4]                       ;
```
- The **last** declarator is put in the name column. The first ones stay glued to the type.
- `[7:0]` is not placed in a packed-dimension column.
- `yy` lands 2 columns left of `x`, because a two-word type shifts the name column.

Expected: what `section1_min_width = 12` gives. The first declarator is in the name column,
packed dimensions share one column, and every name starts in the same column:
```systemverilog
  logic                                      valid                            , ready                            ;
  logic        [7:0]                         s0                               , s1 ...
  logic                                      x                                ;
  int unsigned                               yy                               ;
```

## N-3 — A named parameter override is broken as a call

**Kind:** wrap. **Options:** `function_call.break_policy = "auto"`, `line_length = 100`
(both defaults).

Before:
```systemverilog
axi_xbar #(.NUM_MASTERS(4), .NUM_SLAVES(8), .ADDR_WIDTH(64), .DATA_WIDTH(512), .ID_WIDTH(8), .USER_WIDTH(16)) u_xbar (.clk(clk));
```
After (current):
```systemverilog
  axi_xbar #(.NUM_MASTERS(4), .NUM_SLAVES(8), .ADDR_WIDTH(64), .DATA_WIDTH(512), .ID_WIDTH(8), .USER_WIDTH(
                                                                                                  16
                                                                                                )) u_xbar(
```
The `#(` list itself is never wrapped. The only `(` the line-length rule finds is the value
paren of the last named override, which it breaks as a one-argument call. A `#(` list the
user already wrote one per line is also joined back onto one line.

Expected: a parameter-value list over the limit breaks one override per line, like the port
list does. A `.NAME(value)` paren is never broken as a call.
```systemverilog
  axi_xbar #(
    .NUM_MASTERS(4),
    ...
    .USER_WIDTH(16)
  ) u_xbar(
```

## N-4 — Intra-assignment `repeat` event control is split like a loop

**Kind:** layout. **Options:** none.

Before:
```systemverilog
q <= repeat (2) @(posedge clk) d;
```
After (current):
```systemverilog
    q <= repeat (2)
      @(posedge clk) d;
```
Expected: `q <= repeat (2) @(posedge clk) d;`, unchanged. After `<=`/`=`, `repeat (n)` is an
intra-assignment timing control, not a loop statement.

## N-5 — A call in a brace-less body is measured against the header's line

**Kind:** wrap. **Options:** `function_call.break_policy = "auto"`, `line_length` (default 100).

Before:
```systemverilog
if (some_long_condition_signal && another_long_condition_signal && third_cond) report_error("ID", "failed");
```
After (current):
```systemverilog
    if (some_long_condition_signal && another_long_condition_signal && third_cond)
      report_error(
        "ID",
        "failed"
      );
```
Expected:
```systemverilog
    if (some_long_condition_signal && another_long_condition_signal && third_cond)
      report_error("ID", "failed");
```
The call-break loop in `WrapPass` runs before `apply_single_statement_control_wrap()` moves
the body to its own line. `line_prefix_width()` therefore still counts the `if (…)` header.
The same thing happens for `for`, `always @(…)`, and `` `uvm_error(…)`` (UVM code hits it
constantly).

## N-6 — `sequence` / `property` / `let` formals are wrapped as calls

**Kind:** wrap. **Options:** `function_call.break_policy = "always"`, or `"auto"` once `arg_count`
is reached.

Before:
```systemverilog
sequence s_req(sig, n);
  sig ##n !sig;
endsequence
let max(a, b) = (a > b) ? a : b;
```
After (current):
```systemverilog
  sequence s_req(
             sig,
             n
           );
  ...
  let max(
        a,
        b
      ) = (a > b) ? a : b;
```
In the alt config, statement alignment also pads the `let`: `)     = ( a>b ) ? a : b;`.

Expected: these are declarations, not calls. Leave them on one line, or use
`function_declaration.*` as for `function`/`task`.

## N-7 — Hanging continuation lines are one column short with inside-paren spacing

**Kind:** align. **Options:** any hanging layout plus inside-paren spacing:
- `function_call.layout = "hanging"` + `function_call.space_inside_paren = true`
- `function_declaration.layout = "hanging"` (it reads the same inside-paren spacing;
  DPI `import` prototypes too)
- `module.parameter_layout = "hanging"` + `spacing.space_inside_parens = true`

Before:
```systemverilog
x = foo(a, b);
function int add(int alpha, int beta); ... endfunction
module exprs #(parameter int W = 32, parameter type T = logic) (...);
```
After (current):
```systemverilog
    x = foo( a,
            b );
  function int add ( int alpha,
                    int beta );
module exprs #( parameter int W = 32,
               parameter type T = logic ... )(
```
Expected: continuation lines align with the first argument, which starts one column further
right because of the inserted space:
```systemverilog
    x = foo( a,
             b );
  function int add ( int alpha,
                     int beta );
```

## N-8 — The port-list `(` after a header `import` is indented to body level

**Kind:** indent. **Options:** `default_indent_level_inside_outmost_block = 1` (the default).
With `0` it is correct.

Before:
```systemverilog
module pkg_user
  import my_pkg::*;
(
  input logic a
);
```
After (current):
```systemverilog
module pkg_user
  import my_pkg::*;
  (
  input logic a
);
```
Expected: `(` at column 0, as `#(` is in the same position (`module gen_top` / `import …;` /
`#(`), with ports one level in. The opening paren and its ports now share a column, and `);`
doesn't line up with `(`.

## N-9 — A select after a `;`-less macro line is spaced like a declaration

**Kind:** spacing. **Options:** none. Declaring the macro as `statement_like` or
`declaration_like` in `[format.macros]` does not help.

Before:
```systemverilog
initial begin
  `INIT_PROLOG
  mem[0] = 1;
end
`MY_DECL_MACRO
mem2[1] = 3;
```
After (current):
```systemverilog
    `INIT_PROLOG
    mem [0] = 1;
  ...
  `MY_DECL_MACRO
  mem2 [1] = 3;
```
Expected: `mem[0] = 1;` and `mem2[1] = 3;`. The macro and the next line are read as one
`type name [dim]` declaration. This is what happens to CIRCT's
`` `INIT_RANDOM_PROLOG_ `` followed by `_RANDOM[/*Zero width*/ 1'b0] = `RANDOM;`.

## N-10 — `rand` / `randc` properties are not declarations to the aligner

**Kind:** align + spacing. **Options:** `var_declaration.align = true` (repo config). The
spacing half needs no config.

Before:
```systemverilog
class c;
  int da[];
  rand int ra[];
  rand bit [7:0] rb;
  static int s;
endclass
```
After (current, repo config):
```systemverilog
    int                                     da                  []              ;
    rand int ra[];
    rand bit [7:0] rb;
    static int                              s                                   ;
```
With no config, you get `int da [];` next to `rand int ra[];`.

Expected: `rand`/`randc` are qualifiers like `static`. The line is aligned, and `ra []` gets
the J-15 space.

## N-11 — A relational `<=` is spaced as an assignment

**Kind:** spacing. **Options:** `binary_operator_spacing = "none"`, or
`assignment_operator_spacing = "none"`.

Before:
```systemverilog
y = a <= b;
y = (a <= b) && c;
assign z = a <= b;
```
After (current, `binary_operator_spacing = "none"`):
```systemverilog
    y = a <= b;
    y = (a<=b)&&c;
  assign z = a <= b;
```
After (current, `assignment_operator_spacing = "none"`): `y=a<=b;`, `assign z=a<=b;`.

Expected: a `<=` on the right of `=` or `assign` is a comparison. Space it as `binary_operator_spacing`
does `>=` (`y = a<=b;` / `y=a <= b;`). As written, it only counts as a comparison inside parentheses.

## N-12 — `procedural_event_control_at_spacing = "none"` glues `@` to its left

**Kind:** spacing. **Options:** `spacing.procedural_event_control_at_spacing = "none"`.

Before:
```systemverilog
q <= @(posedge clk) d;
q = @(negedge clk) d;
wait (a) @(posedge clk) q = 2;
```
After (current):
```systemverilog
    q <=@(posedge clk) d;
    q =@(negedge clk) d;
    wait(a)@(posedge clk) q = 2;
```
Expected: the option controls `always @(…)`. After an assignment operator, the operator's own
spacing should win (`q <= @(posedge clk) d;`). A statement-level `@` after `wait (…)` keeps its
separating space.

## N-13 — Replication inside an assignment pattern gets a space

**Kind:** spacing. **Options:** none.

Before: `arr = '{4{8'hAA}};` and `arr = '{2{a, b}};`

After (current): `arr = '{4 {8'hAA}};` and `arr = '{2 {a, b}};`. `cat = {4{8'hAA}};` is left alone.

Expected: `'{4{8'hAA}}`, spaced like the plain replication.

## N-14 — min:typ:max colons are spaced like a ternary

**Kind:** spacing. **Options:** none. `range_colon_spacing` does not apply.

Before:
```systemverilog
bufif0 #(1:2:3, 4:5:6) b1 (o1, i1, en);
assign #(1:2:3) w = x;
(posedge CK => (Q +: D)) = (0.15:0.2:0.25, 0.18:0.22:0.3);
```
After (current):
```systemverilog
  bufif0 #(1 : 2 : 3, 4 : 5 : 6) b1(o1, i1, en);
  assign #(1 : 2 : 3) w = x;
    (posedge CK => (Q +: D)) = (0.15 : 0.2 : 0.25, 0.18 : 0.22 : 0.3);
```
Expected: `#(1:2:3, 4:5:6)`, as written. A min:typ:max triple is one value. Spacing it like
`range_colon_spacing` would also be fine.

## N-15 — A unary operator after a binary one keeps a space under `"none"`

**Kind:** spacing. **Options:** `spacing.binary_operator_spacing = "none"`.

Before: `y = a * -b + c & ~d;` and `y = ~&a | ~|b;`

After (current): `y = a* -b+c& ~d;` and `y = ~&a| ~|b;`

Expected: `a*-b+c&~d`, `~&a|~|b`. The space is only required where the two operators would
lex as one token (`a- -b`, `a+ +b`).

## N-16 — Space before `(` differs for an instance array

**Kind:** spacing. **Options:** none (`instance.align = false`).

Before:
```systemverilog
stage u_arr [3:0] (.a(a));
stage u_one (.a(a));
```
After (current):
```systemverilog
  stage u_arr[3:0] (
  ...
  stage u_one(
```
Expected: the same rule for both, either `u_arr[3:0](` or `u_one (`.

## N-17 — The last enum item's trailing comment is not aligned

**Kind:** align. **Options:** `enum_declaration.align = true` (repo config).

Before:
```systemverilog
typedef enum logic [1:0] {
  S_IDLE = 2'b00, // idle
  S_ERR  = 2'b11  // error
} st_t;
```
After (current, repo config):
```systemverilog
typedef enum logic [1:0] {
    S_IDLE     = 2'b00       , // idle
    S_ERR      = 2'b11 // error
} st_t;
```
Expected: the value section is padded on the last item too, so `// error` lines up with
`// idle`.

---

## Noted, not counted

- A call nested inside another call's arguments is never broken, even when that line
  overflows `line_length`. For example, `` `uvm_info(get_type_name(), $sformatf("…", a, b, c, d), UVM_MEDIUM) ``
  leaves a 104-column `$sformatf` line. This is deliberate (`nested_argument_open` in `WrapPass`).
  Listed only because UVM code hits it all the time.
- `uvm_config_db#(T)::get` → `uvm_config_db #(T)::get`. It matches `extends uvm_driver #(T)`, so
  it's left as style.

## Reproducing

Inputs, configs and the harness are in the session scratchpad under `r6/`:
- `run.py <dir>` formats every file twice per config and reports NONIDEMPOTENT /
  TOKENS_CHANGED.
- `rp.sh <name> <cfg...> < snippet` formats a snippet under the named config directories
  (`c_*` hold the single-option configs used above).

`src/` holds the corpus, `crlf/` the CRLF + tab copies, and `out/<cfg>/` the outputs. The binary
needs WinLibs `mingw64/bin` ahead of Git's on `PATH`.

## Status

Each finding is its own commit on `fix/formatter-stress-bugs` (`fix(formatter): N-<n> ...`),
with a regression test in `tests/test_formatter_regressions.cpp` ("Round 6" section). Every
test was checked to fail without its fix and pass with it.

| ID | Fixed | Owner | Regression test |
|----|-------|-------|-----------------|
| N-1 | yes | lexer `find_multiline_define_end` | backslash-CRLF continues a define |
| N-2 | yes | AlignPass legacy var aligner | default-width declarations align on the first declarator |
| N-3 | yes | WrapPass `#(` branch, `nested_argument_open` | a long override list breaks per override and not inside one |
| N-4 | yes | SyntaxPass `is_intra_assignment_repeat` | an intra-assignment repeat is not a loop |
| N-5 | yes | WrapPass pass order | a call in a brace-less body is measured from its own line |
| N-6 | yes | WrapPass declaration check | sequence property and let formals are declarations and not calls |
| N-7 | yes | IndentPass hanging column | hanging items line up with the first item after a spaced paren |
| N-8 | yes | IndentPass header port list | the port list paren after a header import sits with its header |
| N-9 | yes | `is_var_declaration_trailing_dimension_open` | a select after a semicolonless macro line is not a declaration |
| N-10 | yes | `is_var_decl_leading_keyword` | rand and randc properties are declarations |
| N-11 | yes | SpacingPass `is_relational_less_equal` | a relational less-equal on the right is spaced as a comparison |
| N-12 | yes | SpacingPass `@` rule | procedural event at spacing none does not glue an intra-statement event |
| N-13 | yes | SyntaxPass `is_replication_brace` | a replicated assignment pattern binds its multiplier |
| N-14 | yes | SyntaxPass `is_min_typ_max_colon` | min typ max colons are spaced as a range and not a ternary |
| N-15 | yes | SpacingPass `operators_would_merge` | binary spacing none separates a unary operator only where it would merge |
| N-16 | yes | SpacingPass instance `(` | an instance array spaces its paren like a plain instance |
| N-17 | yes | AlignPass enum body | the last enum item's trailing comment is aligned |

- The `[formatter]` suite has 327 cases and 885 assertions, all passing.
  `ctest --test-dir build` passes 971 of 971.
- Pinned expectations changed, each incidental to its test's subject:
  - N-10: `rand int payload [4];` and `rand int q [4];` in the two constraint-layout tests.
  - N-16: `sub u_c[3:0](` in "instances as generate bodies or with attributes are instances".
- Every fix was diffed across the corpus under all 16 configs. Every output difference was
  reviewed and intended. The final corpus is idempotent and token-preserving under every config.
- Docs: `procedural_event_control_at_spacing` (N-12) and `range_colon_spacing` (N-14) in
  `docs/formatter/options.md`.
- N-9 keeps one TokenKind-undecidable case as a declaration: a macro-named type followed by
  `;` (`` `WORD_T m [4]; ``). A macro in front of an *assigned* select is read as a statement.
