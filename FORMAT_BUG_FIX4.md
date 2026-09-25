# Formatter Bug Report — Round 4

Findings from a fourth stress test of `lazyverilog-fmt` at `d20cbf6` (tip of
`fix/formatter-stress-bugs`, after the K-1..K-15 and R1..R6 fixes).

## What was tested

14 new RTL files plus 7 whitespace edge-case files, each run under the same
three configs as round 3 (no config, the repo's `lazyverilog.toml`, and the
aggressive "alt" config).  Every finding below was then cut down to a minimal
repro and re-run.

The styles avoid what rounds 1–3 covered:

- escaped identifiers (`\clk~ `, `\bus[0] `, `\a/b `) in ports, instances, selects;
- preprocessor: `` `" `` stringification, ` `` ` token pasting, default macro
  arguments, multi-line `` `define ``, `` `ifdef `` inside parameter lists, port
  lists and brace-less bodies, `` `line ``, `` `celldefine ``, `` `resetall ``;
- covergroups: `iff`, `with`, `wildcard`/`illegal_bins`/`ignore_bins`,
  transition bins, `cross` with `binsof ... intersect`, `with function sample`;
- design units: `checker`, `program`, `config`, `let`, clocking blocks,
  `global clocking`, modport `clocking`/`import task`, `alias`, `timeunit`,
  nested modules, generic `interface` ports;
- expressions: streaming `{<<8{}}`, casts `type(a)'()`, `32'()`, `inside`,
  `==?`/`!=?`, `**`/`<<<`, `with` on array methods, `$unit::`, `$root`;
- classes: DPI import/export, `interface class`, `implements`, `soft`, `dist`,
  `unique {}` constraints, UVM macros with and without `;`;
- processes: `fork`/`join_any`/`join_none` with labels, `wait_order`, `->>`,
  `@ev`, `case ... matches` with `&&&`, `unique0 case inside`;
- nets and gates: drive strengths, `trireg (large)`, min:typ:max delays,
  switch primitives, `specify` with conditional paths;
- SVA: sequence/property arguments, `throughout`/`within`, `[->n]`/`[=n]`,
  `s_eventually always`, `accept_on`, `nexttime`, `expect`, deferred `assert #0`;
- attributes on ports, declarations, `case`, instances, functions and operands;
- whitespace: empty file, blank-only file, comment-only file without newline,
  form feed / vertical tab, mixed CRLF/LF, UTF-8 BOM + Hangul comment.

## Safety results

- No crashes, no aborted formats.
- **Every file and every repro is idempotent under every config.**
- The token stream is preserved everywhere (escaped identifiers keep their
  terminating space).  The only byte-level loss is a leading UTF-8 BOM (L-17).

So, as in rounds 2 and 3, the issues below are **layout or spacing** bugs —
except L-17, which is in the CLI.

## Summary

| ID | Severity | Config | Issue |
|----|----------|--------|-------|
| L-1 | High | all | A generic `interface` port opens an indent scope that never closes — every later line in the file, including the next module, is shifted |
| L-2 | High | all | A nested module's `endmodule` is outdented and shifts the rest of the outer module |
| L-3 | High | all | `always` inside a property is treated as a procedural `always` — the assertion is broken mid-expression |
| L-4 | Medium | all | `` `else `` branch of an `` `ifdef `` inside a brace-less body loses its indent |
| L-5 | Medium | all | An instance that is a brace-less generate body, or carries an attribute, is laid out as a call; an instance array there is aligned as a variable declaration |
| L-6 | Medium | all | An over-long line is "fixed" by wrapping a call whose `(` is already past the limit |
| L-7 | Medium | all | Multi-dimensional unpacked declarators (`b[2][3]`) miss the J-15 spacing |
| L-8 | Medium | repo | Declaration alignment pads inside a macro's argument list |
| L-9 | Medium | all | Leading-comma `` `ifdef `` parameters/ports: the `,` is put on its own line |
| L-10 | Medium | all | Second item of a comma list in a `bins` declaration is outdented to the `bins` column |
| L-11 | Medium | repo | Event-control spacing depends on the contents: `@ ( posedge clk );` vs `@(ev1 or ev2);` |
| L-12 | Low | all | A statement label right after `end : name` is spaced `lbl : x = 3;` |
| L-13 | Low | all/alt | Attribute spacing: `sub(* a *)`, `function(* a *)`; alt strips `(* *)` padding |
| L-14 | Low | alt | SVA repetition brackets asymmetric: `b[*1 : 3 ]`, `c[->2 ]` |
| L-15 | Low | repo/alt | Empty `for` clauses asymmetric: `for ( ; ;)`, `for(; ; )` |
| L-16 | Low | alt | `binary_operator_spacing = "none"` glues an operator to a following comment: `a+// c`, `a&&/* c */ b` |
| L-17 | Low | CLI | `lazyverilog-fmt -i` rewrites LF files as CRLF on Windows; a leading BOM is dropped |

Design observation (not counted as a bug): user line breaks inside an
expression are joined, and nothing re-wraps the result, so a property written
over two lines becomes one 115-column line and the 170-column `if` condition
in `e13_long.sv` is left as is.  Only calls are ever wrapped (which is how L-6
happens).

---

Each issue below uses the same layout:

- **Kind** — what goes wrong: *indent* (scope depth), *layout* (line breaks),
  *wrap* (line-length breaking), *align* (column padding), *spacing*, or *CLI*.
- **Related options** — the `lazyverilog.toml` keys that shape the bad output,
  with the values used in the repro.  "none" means the bug happens under every
  config, including no `lazyverilog.toml` at all.
- **RTL** — the smallest input that shows the bug.
- **Current** / **Expected** — the output today and what it should be.

Unless a section says otherwise, the output shown is for no config
(`indent_size = 2`).

---

## L-1 — A generic `interface` port leaks an indent scope

**Kind:** indent (the bug carries into every later line of the file).

**Related options:** none.  The size of the drift is `[format] indent_size`.

```toml
[format]
indent_size = 2
```

**RTL**

```systemverilog
module m (interface g);
wire a;
endmodule
module n;
wire c;
endmodule
```

**Current**

```systemverilog
module m(
  interface g
);
    wire a;
endmodule
  module n;
    wire c;
endmodule
```

**Expected**

```systemverilog
module m(
  interface g
);
  wire a;
endmodule
module n;
  wire c;
endmodule
```

`interface.mst g` behaves the same.  The `interface` keyword in the port list
is taken as the start of an interface declaration, and nothing closes it.
`virtual interface bus_if vif;` inside a class is fine.
**Fix:** an `interface` keyword inside a port-list `(` is a port type, not the
start of a design unit.

---

## L-2 — A nested module's `endmodule` is outdented

**Kind:** indent.

**Related options:** none.  Shown with `[format] indent_size = 2`.

**RTL**

```systemverilog
module outer;
module inner;
wire a;
endmodule
wire b;
endmodule
```

**Current**

```systemverilog
module outer;
  module inner;
    wire a;
endmodule
  wire b;
endmodule
```

**Expected**

```systemverilog
module outer;
  module inner;
    wire a;
  endmodule
  wire b;
endmodule
```

Nested modules are legal SystemVerilog (IEEE 1800 §23.4).  The inner
`endmodule` is always put at column 0 instead of at its `module`'s depth.

---

## L-3 — `always` inside a property is treated as an `always` block

**Kind:** layout + indent.

**Related options:** none.

**RTL**

```systemverilog
module m;
a1: assert property (@(posedge clk) s_eventually always a);
property p; always a; endproperty
endmodule
```

**Current**

```systemverilog
module m;
  a1: assert property (@(posedge clk) s_eventually always
    a);
  property p;
    always
      a;
  endproperty
endmodule
```

**Expected**

```systemverilog
module m;
  a1: assert property (@(posedge clk) s_eventually always a);
  property p;
    always a;
  endproperty
endmodule
```

Inside a property, `always` is a property operator (§16.12.11), not a process.
`s_always` is not affected.  `a |-> always [1:3] b` breaks the same way.
**Fix:** treat `always` as an operator inside a `property`…`endproperty`
body and inside the parentheses of `assert`/`assume`/`cover`/`restrict property`.

---

## L-4 — The `` `else `` branch in a body without `begin` loses its indent

**Kind:** indent.

**Related options:** none.

**RTL**

```systemverilog
module m;
always_comb
`ifdef FAST
  x = 1;
`else
  x = 2;
`endif
endmodule
```

**Current**

```systemverilog
module m;
  always_comb
`ifdef FAST
    x = 1;
`else
  x = 2;
`endif
endmodule
```

**Expected**

```systemverilog
module m;
  always_comb
`ifdef FAST
    x = 1;
`else
    x = 2;
`endif
endmodule
```

The single-statement indent is used up by the first branch.  The
`` `else `` branch fills the same slot, so it should get the same indent.
`if (a)` followed by `` `ifdef ``/`` `else `` does the same thing.

---

## L-5 — Instances laid out as function calls or declarations

**Kind:** layout + align.

**Related options** (repo config values):

```toml
[format.instance]
align = true
port_indent_level = 1
instance_port_name_width = 20
instance_port_between_paren_width = 30

[format.var_declaration]
align = true            # wrongly applied to the instance array

[format.function_call]
space_before_paren = false   # wrongly applied: "u_b(" instead of "u_b ("
```

**RTL**

```systemverilog
module m;
if (N > 2) begin
sub u_a (.a(a));
end
if (N > 2)
sub u_b (.a(a));
for (genvar k = 0; k < 4; k++)
sub u_c [3:0] (.a(a));
(* keep *) sub u_d (.a(a));
endmodule
```

**Current (repo config)**

```systemverilog
module m;
if (N > 2) begin
    sub u_a (
        .a                  (a                             )
    );
end
if (N > 2)
    sub u_b(.a(a));
for (genvar k = 0 ; k < 4 ; k++)
    sub                                     u_c                 [3:0] (.a(a))   ;
(* keep *) sub u_d(.a(a));
endmodule
```

**Expected:** `u_b`, `u_c` and `u_d` laid out like `u_a`: one port per line,
aligned, with a space before `(`.

Instances are only recognised at the start of a module item.  Here the
instance is the body of a generate `if`/`for` without `begin`, or has an
attribute in front of it.  This is the same kind of bug as K-12 (gate and
`bind` instances laid out as calls).

---

## L-6 — Breaking a call whose `(` is already past the line limit

**Kind:** wrap.

**Related options** (defaults; same under the repo config):

```toml
[format.function_call]
break_policy = "auto"
line_length = 100
layout = "hanging"
```

**RTL**

```systemverilog
module m;
assign y = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa + bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb + cccccccccccccccccccc + g(d);
endmodule
```

**Current**

```systemverilog
module m;
  assign y = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa + bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb + cccccccccccccccccccc + g(
                                                                                                              d
                                                                                                            );
endmodule
```

**Expected:** the line left as it is.  If the trailing `g(d)` is removed, the
same over-long line is left alone.

The call's `(` is already past column 100, so a hanging layout can only push
the arguments further right.  This was seen in real code as
`… && $countones(` / `d` / `) < 2);` inside an assertion.
**Fix:** don't break a call whose `(` is already past `line_length`.

---

## L-7 — Unpacked declarators with several dimensions miss the J-15 spacing

**Kind:** spacing.

**Related options:** none.  The space before an unpacked dimension on a
declarator is a fixed rule (J-8/J-15).  `space_inside_dimension_brackets` only
affects the inside of the brackets.

**RTL**

```systemverilog
module m;
logic a [4];
logic b [2][3];
int e [2], f [3][4];
endmodule
```

**Current**

```systemverilog
module m;
  logic a [4];
  logic b[2][3];
  int e [2], f[3][4];
endmodule
```

**Expected**

```systemverilog
module m;
  logic a [4];
  logic b [2][3];
  int e [2], f [3][4];
endmodule
```

`is_var_declaration_trailing_dimension_open()` checks the token after the
first `]`, and only accepts `;` `,` `=` `)`.  A second `[` fails that check.
**Fix:** skip any further `[...]` groups before checking the next token.

---

## L-8 — Declaration alignment pads inside a macro's arguments

**Kind:** align.

**Related options** (repo config):

```toml
[format.var_declaration]
align = true
align_adaptive = true
section1_min_width = 20
section2_min_width = 20
section3_min_width = 20
section4_min_width = 16
```

**RTL**

```systemverilog
module m;
logic [3:0] `CAT(foo, _q);
logic [7:0] bar;
endmodule
```

**Current (repo config)**

```systemverilog
module m;
logic               [3:0] `CAT(foo,     _q)                                 ;
logic               [7:0]               bar                                 ;
endmodule
```

**Expected**

```systemverilog
module m;
logic               [3:0]               `CAT(foo, _q)                       ;
logic               [7:0]               bar                                 ;
endmodule
```

The macro call is the declarator's name and should move as one unit.  Today
the name-column padding goes after the comma inside the macro.

---

## L-9 — Leading-comma `` `ifdef `` lists

**Kind:** layout.

**Related options:**

```toml
[format.module]
parameter_layout = "block"   # "hanging" splits the comma the same way

[format.port_declaration]
align = true                 # ANSI ports: the same split
```

**RTL**

```systemverilog
module m #(
  parameter int W = 8
`ifdef WIDE
  , parameter int X = 64
`endif
) ();
endmodule
```

**Current**

```systemverilog
module m #(
  parameter int W = 8
`ifdef WIDE
  ,
  parameter int X = 64
`endif
)();
endmodule
```

**Expected**

```systemverilog
module m #(
  parameter int W = 8
`ifdef WIDE
  , parameter int X = 64
`endif
)();
endmodule
```

A leading comma is the usual way to make the last entry optional.  Today the
comma ends up alone on its own line.

---

## L-10 — `bins` comma lists are split and outdented

**Kind:** layout + indent.

**Related options:** none.

**RTL**

```systemverilog
module m;
covergroup cg @(posedge clk);
cp: coverpoint a {
bins tr = (1 => 2), (4 => 5);
bins s = {1, 2}, t = {3};
}
endgroup
endmodule
```

**Current**

```systemverilog
module m;
  covergroup cg @(posedge clk);
    cp: coverpoint a {
      bins tr = (1 => 2),
      (4 => 5);
      bins s = {1, 2},
      t = {3};
    }
  endgroup
endmodule
```

**Expected**

```systemverilog
module m;
  covergroup cg @(posedge clk);
    cp: coverpoint a {
      bins tr = (1 => 2), (4 => 5);
      bins s = {1, 2}, t = {3};
    }
  endgroup
endmodule
```

A top-level `,` inside a `bins` item is treated like a statement boundary.
Under the alt config this also drops the next `bins` out of the alignment
group.

---

## L-11 — Event-control spacing depends on what's inside

**Kind:** spacing.

**Related options** (repo config):

```toml
[format.spacing]
procedural_event_control_at_spacing = "both"
space_inside_event_control_parens = true
```

**RTL**

```systemverilog
module m;
initial begin
@(posedge clk);
@(ev1 or ev2);
@(negedge clk) x = 1;
end
endmodule
```

**Current (repo config)**

```systemverilog
module m;
initial begin
    @ ( posedge clk );
    @(ev1 or ev2);
    @(negedge clk) x = 1;
end
endmodule
```

**Expected**

```systemverilog
module m;
initial begin
    @ ( posedge clk );
    @ ( ev1 or ev2 );
    @ ( negedge clk ) x = 1;
end
endmodule
```

These are three spellings of the same construct.  All of them should follow
the options, as `always @ ( posedge clk )` already does.

---

## L-12 — A statement label right after `end : name`

**Kind:** spacing.

**Related options:** none.

**RTL**

```systemverilog
module m;
initial begin
begin : blk end : blk
lbl: x = 3;
end
endmodule
```

**Current**

```systemverilog
module m;
  initial begin
    begin: blk
    end: blk
    lbl : x = 3;
  end
endmodule
```

**Expected:** `lbl: x = 3;`, which is what every other statement label gets.
The end label's colon affects the next colon.

---

## L-13 — Spacing around attributes

**Kind:** spacing.

**Related options:**

```toml
[format.function_call]
space_before_paren = false         # "(*" is spaced as a call paren

[format.spacing]                   # alt config
binary_operator_spacing = "none"   # strips the space inside "(* ... *)"
space_inside_parens = true         # would ask for the opposite
```

**RTL**

```systemverilog
module m;
sub (* keep *) u (.a(a));
function (* noinline *) int f(); return 1; endfunction
endmodule
```

**Current (no config)**

```systemverilog
module m;
  sub(* keep *) u(.a(a));
  function(* noinline *) int f();
    return 1;
  endfunction
endmodule
```

**Current (alt config):** every attribute becomes `(*keep*)`,
`(*noinline*)`.

**Expected (every config)**

```systemverilog
module m;
  sub (* keep *) u (
    .a(a)
  );
  function (* noinline *) int f();
    return 1;
  endfunction
endmodule
```

---

## L-14 — SVA repetition brackets are uneven

**Kind:** spacing.

**Related options** (alt config):

```toml
[format.spacing]
space_inside_dimension_brackets = true
range_colon_spacing = "both"
```

**RTL**

```systemverilog
module m;
sequence s; a ##1 b [*1:3] ##1 c [->2] ##1 d [=3]; endsequence
endmodule
```

**Current (alt config)**

```systemverilog
   sequence s;
      a ##1 b[*1 : 3 ] ##1 c[->2 ] ##1 d[=3 ];
   endsequence
```

**Expected:** `b[*1:3]`, `c[->2]`, `d[=3]`.  These are repetition operators,
not dimensions, so dimension options should not apply.  At the least the
spacing should be even on both sides.  J-11 removed the space after `[` but
left the one before `]`.

---

## L-15 — Empty `for` clauses

**Kind:** spacing.

**Related options:**

```toml
[format.spacing]
semicolon_spacing = "both"   # repo; "after" in alt
```

**RTL**

```systemverilog
module m;
initial for (;;) break;
endmodule
```

**Current**

| config | output |
|--------|--------|
| none | `for (; ;)` |
| repo (`"both"`) | `for ( ; ;)` |
| alt (`"after"`) | `for(; ; )` |

**Expected:** `for (;;)` (or `for(;;)` under alt).  An empty clause should
add no padding.  Today the `;` next to `(` gets its space but the `;` next to
`)` does not, or the other way round.

---

## L-16 — An operator is glued to a following comment

**Kind:** spacing.

**Related options** (alt config):

```toml
[format.spacing]
binary_operator_spacing = "none"
procedural_event_control_at_spacing = "none"
```

**RTL**

```systemverilog
module m;
assign y = a + // carry-in
  b;
assign z = a && /* gate */ b;
endmodule
```

**Current (alt config)**

```systemverilog
module m;
   assign y    =a+// carry-in
      b;
   assign z    =a&&/* gate */ b;
endmodule
```

**Expected:** `=a+ // carry-in` and `=a&& /* gate */ b`.  "No space" is meant
for the space between an operator and its operand, not between an operator
and a comment.  `always_ff /* c */ @(...)` has the same problem with `@`
(`/* c */@(`).

---

## L-17 — CLI line endings and BOM

**Kind:** CLI (not the formatter).

**Related options:** none.  `tools/fmt_file.cpp` only.

**Repro**

```sh
printf 'module m;\nwire a;\nendmodule\n' > lf.sv
lazyverilog-fmt -i lf.sv      # on Windows the file now ends lines with \r\n
```

- The `ifstream` and `ofstream` are opened in text mode, so on Windows `-i`
  turns LF into CRLF.  Stdout does the same.  Because the input is read in
  text mode too, a CRLF file reaches `format_source()` already converted to
  LF, so the CLI never tests the CRLF handling the LSP uses.
  **Fix:** open both with `std::ios::binary`.
- A leading UTF-8 BOM is dropped.  Most tools don't care, but `-i` is silently
  changing bytes.  Keep the BOM if the input has one.

---

## Reproducing

Inputs, configs and harness are in the session scratchpad under `r4/`.
`run.py` works as in round 3.  `rp.sh <name> <cfg...> < snippet` formats a
snippet under the named configs and checks idempotency.  The binary needs
WinLibs `mingw64/bin` ahead of Git's on `PATH`; otherwise it exits with
`0xC0000135`.
