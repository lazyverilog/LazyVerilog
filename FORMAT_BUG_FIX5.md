# Formatter Bug Report — Round 5

Findings from a fifth stress test of `lazyverilog-fmt` on `fix/formatter-stress-bugs`
at `970b2ad`, **on top of the uncommitted round-4 (L-*) fixes** in the working tree.

All twelve issues are **fixed in the working tree (uncommitted)**.  Each has a
`[formatter][regression]` case in `tests/test_formatter_regressions.cpp`.  All
eleven new cases (M-10 and M-11 share one) fail against the pre-round-5
`formatter_passes.hpp` and pass with the fixes.

## What was tested

18 new RTL files, each run under three configs, plus about 40 single-option
repros:

- **none**: no `lazyverilog.toml`.
- **repo**: the repo's `lazyverilog.toml`.
- **alt**: `indent_size = 3`, tabs, `default_indent_level = 1`, `line_length = 60`,
  `space_inside_parens`, `binary_operator_spacing = "none"`, non-adaptive
  alignment everywhere, `break_policy = "always"`, `arg_count = 2`, and
  `function_declaration.layout = "hanging"` with `line_length = 40`.

Every finding was then cut down to a minimal repro and run under a
**single-option** config, so each option below is shown to be what triggers it.

The styles target areas rounds 1–4 did not stress:

- selects as assignment targets with `<=`, `+=`, `pkg::` scopes;
- brace-less control bodies with own-line comments between the header and the body;
- case items whose statement is itself a control (`A: if … else …`, `B: for …`);
- ANSI port lists whose first port is an interface (`axi_if.master m`) or a user type;
- `verilog_format: off/on` regions between aligned statements;
- calls in dimensions and selects (`[clog(A,B,C)-1:0]`, ``[`MAX(4,8)-1:0]``, `mem[hash(a,b,c)]`);
- `pkg::type_t`, `var logic`, and scoped struct members in declaration groups;
- instance port alignment at the configured name width, and one character under it;
- hanging function and task declarations with packed-dimension arguments;
- drive and charge strengths, and comment-only connections `.o(/* unused */)`.

## Safety results

- No crashes.
- **Every corpus file and repro is idempotent under every config**, both before
  and after the fixes (54/54 corpus runs are ok).
- The token stream is preserved everywhere.

So, as in rounds 2–4, everything below is a **layout, indent, align or spacing**
bug.

## Excluded (already settled in earlier rounds)

These were seen again and are not reported: `wait(c)`, `mem [4]` spacing,
`assert … else` on three lines, `'{key : value}`, a leading inline block comment
moved to its own line, `0:;` / `default:;`, user line breaks inside
expressions being joined, `blank_lines_between_items` applying everywhere, block
call layout anchored to the callee (K-1), and calls inside `if` conditions
breaking under `always`/`arg_count`.  The last one is pinned by "function calls
inside if conditions use configured layout", which is why M-6 is limited to
brackets.

## Summary

| ID | Severity | Config | Issue |
|----|----------|--------|-------|
| M-1 | High | `var_declaration.align` | `mem[w] <= d;` / `cnt[i] += d;` are aligned as variable declarations: `mem     [w] <=  d       ;` |
| M-2 | Medium | all | An own-line comment between a brace-less `if`/`else`/`for`/`always` and its body is outdented to the header's column |
| M-3 | Medium | all | A case item's statement is not a controlled body: `A: if … else …` puts `else` at the label column; `B: // c` then `z = 2;` puts both at the label column |
| M-4 | High | `module.non_ansi_port_*_enabled` | An ANSI list whose first port has no direction keyword (`axi_if.master m`) is packed as a non-ANSI list |
| M-5 | Low | `statement.align` | The first statement after `// verilog_format: on` is dropped from its alignment group |
| M-6 | Medium | `function_call.break_policy = "always"` / `arg_count` | Calls inside `[]` are broken one argument per line, so a dimension spans five lines |
| M-7 | Low | `var_declaration.align` | `pkg::cfg_t x;` and `var logic v;` are skipped by declaration alignment |
| M-8 | Medium | `instance.align` (non-adaptive) | Strict alignment does not align `(`; a name one character shorter than `instance_port_name_width` glues `(` to the name |
| M-9 | Medium | `port_declaration.align` + hanging function declarations | A hanging function argument is padded with port-list dimension columns: `input logic             [7:0] b` |
| M-10 | Low | all | `wire (pull1, pull0) [3:0] w` → `wire (pull1, pull0)[3:0] w` |
| M-11 | Low | all | `.o(/* unused */)` → `.o( /* unused */)`: space on one side only |
| M-12 | Low | all | `pkg::arr[i] = d;` → `pkg::arr [i] = d;`: read as `type name [dim]` |

---

Each issue below uses the round-4 layout:

- **Kind**: *indent*, *layout* (line breaks), *wrap* (line-length or policy
  breaking), *align* (column padding), or *spacing*.
- **Related options**: the `lazyverilog.toml` keys that shape the bad output,
  with the values used in the repro.  "none" means it happens with no
  `lazyverilog.toml` at all.
- **RTL / Before / After**: the smallest input, the output at `970b2ad`
  plus the round-4 fixes, and the output with the round-5 fix, which is the
  expected output.
- **Fix**: the root cause, the pass that owns the fix, and what changed.

Every fix stays inside the pass that owns the metadata it writes, decides on
`TokenKind` and syntax facts, and never reads original whitespace.

---

## M-1 — A select with `<=` or `+=` is aligned as a declaration

**Kind:** align.  **Severity:** High: it rewrites ordinary sequential logic
into columns, and the repo's own config triggers it.

**Related options**

```toml
[format.var_declaration]
align = true
section1_min_width = 8   # any value > 0 enables the user-type path
section2_min_width = 8
section3_min_width = 8
```

**RTL**

```systemverilog
module m;
logic [7:0] q;
always_ff @(posedge clk) begin
mem[w] <= d;
cnt[i] += d;
end
endmodule
```

**Before**

```systemverilog
  logic   [7:0]   q       ;
  always_ff @(posedge clk) begin
    mem     [w] <=  d       ;
    cnt     [i] +=  d       ;
  end
```

**After / Expected**

```systemverilog
  logic   [7:0]   q       ;
  always_ff @(posedge clk) begin
    mem[w] <= d;
    cnt[i] += d;
  end
```

With the repo config the padding reaches column 60.  `mem[w] = d;` was
already rejected.

**Root cause:** `AlignPass` → `is_var_decl_line` only treats `=` as an
assignment.  With `<=` or `+=` it reads `mem` as a user type, `[w]` as a packed
dimension and `d` as the declared name.

**Fix (AlignPass, `is_var_decl_line`):** if an assignment operator
(`is_assignment_op`) appears at paren/bracket/brace depth 0 before the `=`/`;`,
the line is not a declaration.  A declaration's only top-level assignment is its
initializer `=`, which is the `eq` the function already finds.

---

## M-2 — Own-line comment before a brace-less body is outdented

**Kind:** indent.

**Related options:** none.  The size of the offset is `[format] indent_size`.

**RTL**

```systemverilog
module m;
always_comb begin
if (a)
// c1
x = 1;
else
// c2
x = 0;
end
endmodule
```

**Before**

```systemverilog
    if (a)
    // c1
      x = 1;
    else
    // c2
      x = 0;
```

**After / Expected**

```systemverilog
    if (a)
      // c1
      x = 1;
    else
      // c2
      x = 0;
```

The same happens after `for`, `while`, `always_ff @(…)`, `initial` and `forever`.

**Root cause:** `IndentPass` → `controlled_body_extents()` keys the body's extra
level on the body's first **code** token (`next_code`), so the own-line comment
in front of it sits outside the extent.

**Fix (IndentPass, `controlled_body_extents`'s `add`):** move the extent's
start back over the own-line comments (`CommentRole::OwnLine`, not passthrough)
right before the body.  A trailing comment on the header (`if (a) // why`) is
`CommentRole::Trailing`, so it stays on the header.

---

## M-3 — A case item's statement is not a controlled body

**Kind:** indent.

**Related options:** none.

**RTL**

```systemverilog
module m;
always_comb begin
case (s)
A: if (go) n = B; else n = A;
B:
// c
z = 2;
endcase
end
endmodule
```

**Before**

```systemverilog
    case (s)
      A: if (go)
        n = B;
      else
        n = A;
      B:
      // c
      z = 2;
    endcase
```

`else` lands at the label column and reads like a new case item.  After `B:`,
the statement sits at the label column too.

**After / Expected**

```systemverilog
    case (s)
      A: if (go)
          n = B;
        else
          n = A;
      B:
        // c
        z = 2;
    endcase
```

The same happens for `A: for (…) x = 1;`.

**Root cause:** `controlled_body_extents()` covers `if`/`else`/loops/`always`
bodies, but not the statement after a case item's `:`, so that statement and
everything nested in it get no level of their own.

**Fix (IndentPass, `controlled_body_extents`):** add a branch for
`TopologyFacts::is_case_item_colon`, outside `in_property_expr`: the next code
token, unless it is `;` (`default:;` is settled), starts a controlled body that
ends at `simple_statement_end_from()`.  An `if` there then nests its own branches
as usual.  With M-2, a comment before the statement follows it.  `A: begin … end`
is unchanged, because `begin` bodies already have their own extents.

---

## M-4 — ANSI list led by an interface port is packed as non-ANSI

**Kind:** layout.  **Severity:** High: the port list becomes one line.

**Related options**

```toml
[format.module]
non_ansi_port_per_line_enabled = true       # or
non_ansi_port_max_line_length_enabled = true
non_ansi_port_per_line = 3
```

**RTL**

```systemverilog
module m (axi_if.master m_axi, input logic b, output logic c);
endmodule
```

**Before**

```systemverilog
module m(
  axi_if.master m_axi, input logic b, output logic c
);
```

**After / Expected**

```systemverilog
module m(
  axi_if.master m_axi,
  input logic b,
  output logic c
);
```

`axi_if m,` and `my_pkg::req_t r,` as the first port behave the same.

**Root cause:** `WrapPass` decides "non-ANSI" from the **first** item alone:
`!is_declaration_keyword(first token)`.  An interface or user-typed port has no
keyword.

**Fix (WrapPass, `ModulePorts` list):** a list is non-ANSI only if **no** item
declares a port.  An item declares one if it starts with a declaration keyword,
or if its depth-0 name follows a type: an identifier whose previous depth-0 code
token is identifier-like or `]` (`axi_if.master m`, `t_t [3:0] x`).  A non-ANSI
item is a bare name (`a`) or `.a(b)`, so it never matches.

---

## M-5 — First statement after `verilog_format: on` is not aligned

**Kind:** align.

**Related options**

```toml
format_on_comment_pattern = "verilog_format: on"   # default
[format.statement]
align = true
lhs_min_width = 4
```

**RTL**

```systemverilog
module m;
always_comb begin
x = 1;
// verilog_format: off
q   =    r;
// verilog_format: on
yy = zz;
yyy = zzz;
end
endmodule
```

**Before**

```systemverilog
    x    = 1;
// verilog_format: off
q   =    r;
// verilog_format: on
    yy = zz;
    yyy  = zzz;
```

**After / Expected**

```systemverilog
    x    = 1;
// verilog_format: off
q   =    r;
// verilog_format: on
    yy   = zz;
    yyy  = zzz;
```

**Root cause:** the lexer emits the frozen region, up to and including the
on-marker's line, as one passthrough token (`LexemeFacts::is_disabled_region_body`).
That token carries no `must_break_after`, so `AlignPass`'s line splitter folded
`yy = zz;` into the region's line, and the passthrough token then disabled that
whole line.

**Fix (AlignPass, line-split loop):** a token right after an
`is_disabled_region_body` token starts a new line.  The region always ends at
the end of the on-marker's line, so this is a lexical fact rather than a guess.

---

## M-6 — Calls inside brackets are broken one argument per line

**Kind:** wrap.

**Related options**

```toml
[format.function_call]
break_policy = "always"     # or "auto" with arg_count = 2
```

**RTL**

```systemverilog
module m;
logic [clog(A,B,C)-1:0] w;
logic [7:0] q;
always_comb y = mem[hash(a,b,c)];
endmodule
```

**Before**

```systemverilog
  logic [clog(
           A,
           B,
           C
         )-1:0] w;
  logic [7:0] q;
  always_comb
    y = mem[hash(
              a,
              b,
              c
            )];
```

The broken dimension also drops `w` out of its declaration-alignment group.

**After / Expected**

```systemverilog
  logic [clog(A, B, C)-1:0] w;
  logic [7:0] q;
  always_comb
    y = mem[hash(a, b, c)];
```

``logic [`MAX(4, 8)-1:0]`` behaves the same.

**Root cause:** `WrapPass`'s `nested_argument_open` exempts only calls nested in
another call's arguments.  A call inside a dimension or select is treated as a
statement-level call.

**Fix (WrapPass, `nested_argument_open`):** a call is also nested when
`SyntaxFacts::bracket_depth > 0`.  Calls inside `if (…)` conditions are
deliberately **not** exempted, because that behaviour is pinned (see Excluded).
Line-length breaking of a bracket call still happens through the enclosing
statement, as before.

*Not fixed, noted:* a ternary operand, e.g. `sel ? pick(a,b,c) : '0`, is still
broken under `always`.  It is a statement-level call by the same rule as an `if`
condition, so it is left for a design decision.

---

## M-7 — `pkg::type_t` and `var` declarations are skipped by alignment

**Kind:** align.

**Related options**

```toml
[format.var_declaration]
align = true
section1_min_width = 4
section2_min_width = 4
section3_min_width = 4
```

**RTL**

```systemverilog
module m;
pkg::cfg_t cfg;
var logic vl;
logic [7:0] data_q;
endmodule
```

**Before**

```systemverilog
  pkg::cfg_t cfg;
  var logic vl;
  logic [7:0] data_q ;
```

**After / Expected**

```systemverilog
  pkg::cfg_t       cfg    ;
  var logic        vl     ;
  logic      [7:0] data_q ;
```

A struct member such as `types_pkg::op_e op2;` among aligned members is the
same case.

**Root cause:** `is_var_decl_line` takes the user type to be one identifier and
the name to be the next.  `pkg :: cfg_t` stops at `::`, and `var` is not a
plausible start.

**Fix (AlignPass, `is_var_decl_line`):**
- accept `TK::VarKeyword` as a start;
- walk `id :: id` chains as one type (`type_last`);
- require the name to come after `type_last`.  This also keeps
  `pkg::arr[i] = d;` from matching.

---

## M-8 — Strict instance alignment does not align `(`, and the name width is off by one

**Kind:** align.

**Related options**

```toml
[format.instance]
align = true
align_adaptive = false            # strict
instance_port_name_width = 1      # default; or 20 for the off-by-one
```

**RTL**

```systemverilog
module m;
sub u (.a(a), .long_port(long_signal), .z(z));
sub v (.abcdefghijklmnopqrs(x), .b(y));
endmodule
```

**Before (`align_adaptive = false`)**

```systemverilog
  sub u (
    .a (a          ),
    .long_port (long_signal),
    .z (z          )
  );
  sub v (
    .abcdefghijklmnopqrs (x),
    .b (y)
  );
```

**After / Expected**

```systemverilog
  sub u (
    .a         (a          ),
    .long_port (long_signal),
    .z         (z          )
  );
  sub v (
    .abcdefghijklmnopqrs (x),
    .b                   (y)
  );
```

**Before (`instance_port_name_width = 20`)**: the 19-character name leaves no gap.

```systemverilog
    .abcdefghijklmnopqrs(x),
    .b                  (y)
```

**Root cause:** the `(` column was `max(instance_port_name_width, own name + 2)`,
where "own name" is each port's own name.  With the default width of 1, strict
mode therefore aligned nothing.  `namew` also excluded the `.`, while the option
counts it.

**Fix (AlignPass, instance ports):**
- in strict mode, use the widest name in the instance (`max_namew`);
- in adaptive mode, keep each port's own name;
- in both modes, the column is `max(configured width, name + 2)`, with the `.`
  counted.

`docs/formatter/options.md` already describes strict mode as common columns.

**Pinned test changed:** "formatter: instance alignment strict versus adaptive"
(`tests/test_formatter.cpp`, around line 4268) asserted the misaligned output.
Its strict expectation is now the aligned block shown above.

---

## M-9 — Hanging function argument padded with port-list dimension columns

**Kind:** align.

**Related options**

```toml
[format.function_declaration]
layout = "hanging"
line_length = 40
[format.port_declaration]
align = true
align_adaptive = false
```

**RTL**

```systemverilog
module m;
function automatic logic f(input logic a, input logic [7:0] b);
return a;
endfunction
endmodule
```

**Before**

```systemverilog
  function automatic logic f(input logic a,
                             input logic             [7:0] b);
```

**After / Expected**

```systemverilog
  function automatic logic f(input logic a,
                             input logic [7:0] b);
```

**Root cause:** `AlignPass`'s paren-direction dimension loop pads any line that
starts with a port direction inside parentheses, including a function's
argument list.  That list is not laid out as a port column block.

**Fix (AlignPass, paren-direction dimension loop):** apply it only when the
line's list delimiter (the previous code token) belongs to a
`WrapListKind::ModulePorts` list.  WrapPass already records that.

---

## M-10 — Space lost between a strength and a packed dimension

**Kind:** spacing.

**Related options:** none.

**RTL**

```systemverilog
module m;
wire (pull1, pull0) [3:0] pw = 4'h0;
trireg (small) [7:0] tr;
endmodule
```

**Before**

```systemverilog
  wire (pull1, pull0)[3:0] pw = 4'h0;
  trireg (small)[7:0] tr;
```

**After / Expected**

```systemverilog
  wire (pull1, pull0) [3:0] pw = 4'h0;
  trireg (small) [7:0] tr;
```

**Root cause:** `SpacingPass` removes the space before `[` after any `)`,
because it treats the bracket as a select on a call result.

**Fix (SpacingPass):** new helpers `is_strength_keyword()` and
`closes_strength()`.  A `)` whose matching `(` opens with a drive or charge
strength keyword (`supply0` … `highz1`, `small`/`medium`/`large`) is followed by
a packed dimension, not a select, so the space stays.

---

## M-11 — Comment-only connection spaced on one side

**Kind:** spacing.

**Related options:** `[format.spacing] space_inside_parens` (default `false`).

**RTL**

```systemverilog
module m;
sub u (.a(/* unused */), .b(q));
endmodule
```

**Before**

```systemverilog
    .a( /* unused */),
```

**After / Expected**

```systemverilog
    .a(/* unused */),
```

With `space_inside_parens = true` the fixed output is `( /* unused */ )`.

**Root cause:** `SpacingPass` forces one space between `(` and any comment.

**Fix (SpacingPass):** keep the forced space except when a block comment is the
parentheses' only content (the next code token is the `(`'s matching `)`).  That
case is padded like any other first token.  The forced space still applies to
`( /*autoinst*/` and `( /*autoarg*/` before a list, which two existing tests pin,
and to `( // why`.

---

## M-12 — `pkg::arr[i]` is spaced like a declaration

**Kind:** spacing.

**Related options:** none.

**RTL**

```systemverilog
module m;
pkg::type_t arr [4];
always_comb pkg::arr[i] = d;
endmodule
```

**Before**

```systemverilog
  pkg::type_t arr [4];
  always_comb
    pkg::arr [i] = d;
```

**After / Expected**

```systemverilog
  pkg::type_t arr [4];
  always_comb
    pkg::arr[i] = d;
```

This one was found while verifying M-7; it is independent of M-7.

**Root cause:** `is_var_declaration_trailing_dimension_open()` →
`declares()` recognises `user_t name [n]` by counting at least two identifiers
before the bracket, and counts `pkg` and `arr` separately.

**Fix (shared helper used by SpacingPass):** an identifier right after `::`
continues the scoped name and is not counted again.  `pkg::type_t arr [4]` still
counts two identifiers (`pkg…`, `arr`) and keeps its J-15 space.

---

## Status

| ID | Fixed | Owner | Regression test |
|----|-------|-------|-----------------|
| M-1 | yes | AlignPass `is_var_decl_line` | a select target with <= or += is not a declaration |
| M-2 | yes | IndentPass `controlled_body_extents` | an own-line comment before a brace-less body is indented with it |
| M-3 | yes | IndentPass `controlled_body_extents` | a case item's statement is a controlled body |
| M-4 | yes | WrapPass `ModulePorts` | an ANSI list led by an interface port is not packed as non-ANSI |
| M-5 | yes | AlignPass line split | the statement after a format-off region is aligned |
| M-6 | yes (brackets) | WrapPass `nested_argument_open` | calls inside brackets are not broken per argument |
| M-7 | yes | AlignPass `is_var_decl_line` | scoped and var-led declarations are aligned |
| M-8 | yes | AlignPass instance ports | instance port name width counts the dot |
| M-9 | yes | AlignPass paren-direction loop | hanging function arguments get no port-list dimension padding |
| M-10 | yes | SpacingPass | strength parens and comment-only parens space like code |
| M-11 | yes | SpacingPass | (same case as M-10) |
| M-12 | yes | `is_var_declaration_trailing_dimension_open` | a scoped name before a select is not a declaration |

- The `[formatter]` suite has 309 cases and 835 assertions, all passing.
  `ctest --test-dir build` passes 953 of 953.
- One pinned expectation changed: the strict half of "formatter: instance
  alignment strict versus adaptive" (M-8).
- The corpus of 18 files × 3 configs is idempotent and token-preserving after
  the fixes.

Left open for a design decision:

- a ternary-operand call broken under `break_policy = "always"` (see M-6);
- ~~`localparam logic [7:0] LUT[4]` gets no J-15 space while `logic t [2]` does.~~
  **Fixed as M-13.** `is_var_declaration_trailing_dimension_open()` → `declares()`
  now accepts `parameter`/`localparam` as a leading keyword, so a parameter's
  unpacked dimension is spaced like a variable's: `LUT [4]`, `A [2]`, `B [2][3]`,
  `Y [2]` in `parameter int X = 1, Y [2] = …`, and inside `#( … )`.  Selects in
  the value (`C[1]`, `$size(A[0])`) and `parameter type T = logic [3:0]` are
  unchanged.  Two layout tests, "module parameter layout block" and "…hanging",
  had `TABLE[SIZE]` in their expected output by accident; their expectations now
  read `TABLE [SIZE]`.  Regression: "a parameter's unpacked dimension is spaced
  like a variable's".

## Reproducing

Inputs, configs and harness are in the session scratchpad under `r5/`:

- `run.py <dir>` formats every file twice per config and reports
  NONIDEMPOTENT / TOKENS_CHANGED;
- `rp.sh <name> <cfg...> < snippet` formats a snippet under the named configs;
- `snap.sh <outdir>` captures the per-issue outputs.

`snip/` holds the twelve minimal repros used above.  `before/` and `after/`
hold their outputs.  `formatter_passes.before_r5.hpp` is the file before this
round.  The binary needs WinLibs `mingw64/bin` ahead of Git's on `PATH`.
