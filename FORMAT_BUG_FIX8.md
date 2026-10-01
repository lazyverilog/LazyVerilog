# Formatter Bug Report — Round 8

Findings from an eighth stress test of `lazyverilog-fmt` on `fix/formatter-stress-bugs`
(`597f5c6` plus the round-7 fixes). **All 14 are now fixed**, one commit each; see
"Status" at the end. Each issue was reproduced on a minimal snippet; the "current output"
blocks below are from before the fixes.

## What was tested

18 new RTL files in styles rounds 1–7 did not cover, each under 17 configs (**none**,
**repo** = the repo's `lazyverilog.toml`, **alt** = the round-5 alt config, and 14
single-option configs). Every file was re-run as three whitespace variants (indentation
stripped, random tabs, CRLF) under none/repo/alt. About 25 single-option repros followed.

- **Legacy Verilog-95**: non-ANSI ports with body-level `input`/`output`, `reg` outputs,
  gate primitives and instance arrays, `defparam`, `specify` blocks, UDP tables.
- **Classes**: parameterized `virtual class … extends … implements`, `extern` methods and
  out-of-block bodies, constraints (`foreach`, `solve … before`, `soft`, `dist`, `unique`,
  implication), `randomize() with`, `fork … join_any`/`join_none`, `disable fork`.
- **SVA**: sequences and properties with formals and local variables, `strong`/`weak`,
  `s_eventually`, `until`, `nexttime`, `accept_on`, `#-#`/`#=#`, deferred and final
  assertions, `expect`, `restrict`, `cover sequence`.
- **Interfaces**: modports with `import task` prototypes and port expressions, clocking
  blocks with skews, `default clocking`, instances following a clocking block.
- **Coverage and checkers**: `covergroup` with formals and a sampling event, bins,
  crosses, `checker … endchecker`.
- **FSM styles**: leading-comma ports, tab indentation, one-line `begin … end` case
  items, nested brace-less `if`/`else`, `unique case (1'b1)`.
- **Comments**: several block comments on one line, a block comment after a list comma,
  `//` inside call arguments, comment-only files, doc-comment banners.
- **Misc**: escaped identifiers, every literal form, delays and intra-assignment timing,
  system tasks with empty arguments, attributes, DPI imports/exports, `config` blocks,
  `timeunit`, tagged unions.
- **Edge files**: an empty file, a UTF-8 BOM with CRLF and no final newline, form feeds
  and tab-only lines, a file of comments and one `` `define``.

## Safety results

- No crashes.
- With whitespace ignored, the token stream is unchanged in all 306 runs (18 files × 17 configs).
- **One file is not idempotent, under every config** — Q-1 below. The other 17 files are
  idempotent under all 17 configs.
- The three whitespace variants format identically to the original, apart from the bytes
  inside verbatim regions (a `` `define`` body, multi-line comments).

So Q-1 is an idempotency bug; Q-2…Q-14 are wrong layout. None loses or reorders a token.

## Excluded (settled in earlier rounds, by design, or pinned by tests)

Not reported: everything on the round-7 excluded list; `begin_newline` moving a
`struct`/`union` brace (P-21, by design); `line_length` counting indentation (P-4, by
design); `dist {` expanding; a case item's `if` body indent (M-3); the lone `,` of an
empty argument (K-5 / P-19); a **single** leading block comment moving to its own line;
the multi-name port layout `a , b, c` (I-15); a file with one declaration not being
name-aligned; long expressions and assignment patterns not being wrapped.

Also looked at and judged consistent, so not reported: `always #5 clk = ~clk;` becomes
`always #5` ⏎ `clk = ~clk;`. That is the same rule that puts the body of
`always @(posedge clk) q <= d;` on its own line; `forever #5 clk = ~clk` differs only
because there the delay belongs to the body statement.

## Summary

| ID | Severity | Kind | Config | Issue |
|----|----------|------|--------|-------|
| Q-1 | High | idempotency | none (every config) | Two or more block comments on one line before code are split one per formatter run |
| Q-2 | High | align | `var_declaration.align` | An expression statement that starts with a select (constraint, sequence, property) is aligned as a declaration |
| Q-3 | Medium | spacing | none | The second argument's select gets a space when the first argument has two selects: `f(w[i][j], a [i])` |
| Q-4 | Medium | align | `var_declaration.align` (port align off) | Body-level non-ANSI `input`/`output` declarations are padded into ragged columns |
| Q-5 | Medium | layout | none | The first instance after `endclocking` is not laid out as an instance |
| Q-6 | Medium | comment | none | A block comment after a list comma moves in front of the next port / enum item |
| Q-7 | Medium | align | `instance.align` | Trailing space inside the connection parens when the widest connection has a select or a call |
| Q-8 | Medium | wrap | `function_call.break_policy = "always"` | `covergroup` and `checker` formals are broken like call arguments |
| Q-9 | Medium | align | `statement.align` | A `localparam`/`parameter` with a user type is skipped and splits the alignment group |
| Q-10 | Low | wrap | none | A `//` comment inside call arguments leaves a half-broken list |
| Q-11 | Low | align | `statement.align` | A DPI alias `import "DPI-C" c_name = function …` is aligned as an assignment |
| Q-12 | Low | align | `modport.align` | The comma after a multi-line `import task` prototype is padded far to the right |
| Q-13 | Low | spacing | none | `strong (`, `weak (`, `tagged Pair'{`, `timeunit 1ns / 1ps` |
| Q-14 | Low | align | `var_declaration.align` | A declaration led by an attribute is left out of the alignment |

---

## Q-1 — Several leading block comments are split one per run (idempotency)

Input:
```systemverilog
module m;
/* a */ /* b */ /* c */ assign x = 1;
endmodule
```
Current output — pass 1, then pass 2, then pass 3:
```systemverilog
  /* a */
  /* b */ /* c */ assign x = 1;
```
```systemverilog
  /* a */
  /* b */
  /* c */ assign x = 1;
```
```systemverilog
  /* a */
  /* b */
  /* c */
  assign x = 1;
```
Expected (in one pass, and stable):
```systemverilog
  /* a */
  /* b */
  /* c */
  assign x = 1;
```

## Q-2 — Expression statements aligned as declarations

Config: `var_declaration.align = true` (shown with section widths 20/20/20/16; the
default-width aligner does the same).

Input:
```systemverilog
class c;
rand bit [7:0] data[4];
constraint c_d { data[0] < data[1]; foreach (data[i]) { if (i > 0) data[i] > data[i-1]; } }
endclass
module m;
sequence s; a[0] ##1 b[1]; endsequence
property p; a[0] |-> b[1]; endproperty
endmodule
```
Current output:
```systemverilog
  constraint c_d {
    data                [0] <               data                [1]             ;
    foreach (data[i]) {
      if (i > 0)
        data                [i] >               data                [i-1]           ;
    }
  }
...
  sequence s;
    a                   [0] ##1             b                   [1]             ;
  endsequence
  property p;
    a                   [0] |->             b                   [1]             ;
  endproperty
```
Expected:
```systemverilog
  constraint c_d {
    data[0] < data[1];
    foreach (data[i]) {
      if (i > 0)
        data[i] > data[i-1];
    }
  }
...
  sequence s;
    a[0] ##1 b[1];
  endsequence
  property p;
    a[0] |-> b[1];
  endproperty
```

## Q-3 — A later argument's select is spaced like a declaration

Input:
```systemverilog
f(w[i][j], a[i], a[j]);
x = g(w[i], a[i][j], b[k]);
and g1(w[i][j], a[i], a[j]);
```
Current output:
```systemverilog
f(w[i][j], a [i], a[j]);
x = g(w[i], a [i][j], b[k]);
and g1(w[i][j], a [i], a[j]);
```
Expected: the input, unchanged.

## Q-4 — Body-level port declarations mangled by the variable aligner

Config: `var_declaration.align = true`, `port_declaration.align` off. (With
`port_declaration.align = true` the same lines are fine.) Also happens to `input`/`output`
lines inside non-ANSI function and task bodies.

Input:
```systemverilog
module m(a, b, c, d);
input a, b;
input [7:0] c;
output d;
wire [3:0] w;
endmodule
```
Current output:
```systemverilog
  input       a,  b                       ;
  input       [7:0] c                       ;
  output             d                       ;
  wire                [3:0]               w                                   ;
```
Expected — port-direction lines are left to the port aligner:
```systemverilog
  input a, b;
  input [7:0] c;
  output d;
  wire                [3:0]               w                                   ;
```

## Q-5 — First instance after `endclocking` is not an instance

Input:
```systemverilog
module m;
clocking cb @(posedge clk); input a; endclocking
sub u2(.a(a), .b(b));
sub u3(.a(a), .b(b));
endmodule
```
Current output (with `instance.align` the first one also misses the space before `(`
and the aligned connections):
```systemverilog
  endclocking
  sub u2(.a(a), .b(b));
  sub u3(
    .a(a),
    .b(b)
  );
```
Expected:
```systemverilog
  endclocking
  sub u2(
    .a(a),
    .b(b)
  );
  sub u3(
    .a(a),
    .b(b)
  );
```

## Q-6 — A block comment after a comma moves to the next item

Input:
```systemverilog
module m (
  input logic clk,
  input logic rst_n, /* reset */
  output logic q
);
typedef enum { A, /* ea */
  B } e_t;
sub u (.a(a), /* ca */
  .b(b));
endmodule
```
Current output:
```systemverilog
module m(
  input logic clk,
  input logic rst_n,
  /* reset */ output logic q
);
  typedef enum {
    A,
    /* ea */ B
  } e_t;
  sub u(
    .a(a), /* ca */
    .b(b)
  );
```
The comment now reads as describing `q` and `B`. Under `port_declaration.align` it also
pushes that row out of the columns. The instance port list already does the right thing.
A call broken by `break_policy = "always"` has the same defect.

Expected:
```systemverilog
module m(
  input logic clk,
  input logic rst_n, /* reset */
  output logic q
);
  typedef enum {
    A, /* ea */
    B
  } e_t;
```

## Q-7 — Trailing space inside aligned connection parens

Config: `instance.align = true`.

Input:
```systemverilog
sub u (.a(a[n]), .b(b));
sub v (.a(x), .b(f(x, y)));
sub w (.c(c[3:0]), .d(d));
```
Current output:
```systemverilog
  sub u (
    .a (a[n] ),
    .b (b    )
  );
  sub v (
    .a (x       ),
    .b (f(x, y) )
  );
  sub w (
    .c (c[3:0]   ),
    .d (d        )
  );
```
Expected — the widest connection closes with no padding:
```systemverilog
  sub u (
    .a (a[n]),
    .b (b   )
  );
  sub v (
    .a (x      ),
    .b (f(x, y))
  );
  sub w (
    .c (c[3:0]),
    .d (d     )
  );
```

## Q-8 — `covergroup` / `checker` formals broken like a call

Config: `function_call.break_policy = "always"` (also `arg_count`).

Input:
```systemverilog
module m;
covergroup cg(int lo, int hi) @(posedge clk); coverpoint x; endgroup
function int f(int lo, int hi); return lo; endfunction
endmodule
checker chk(logic clk, logic a, b); endchecker
```
Current output:
```systemverilog
  covergroup cg(
               int lo,
               int hi
             ) @(posedge clk);
    coverpoint x;
  endgroup
  function int f(int lo, int hi);
...
checker chk(
          logic clk,
          logic a,
          b
        );
endchecker
```
Expected — a declaration's formals follow the declaration layout, as `function`,
`sequence`, `property` and `let` already do (N-6):
```systemverilog
  covergroup cg(int lo, int hi) @(posedge clk);
    coverpoint x;
  endgroup
...
checker chk(logic clk, logic a, b);
endchecker
```

## Q-9 — A user-typed `localparam` splits the alignment group

Config: `statement.align = true`.

Input:
```systemverilog
localparam int A = 1;
localparam my_t BBBB = 2;
localparam int CC = 3;
localparam int unsigned DDDDD = 3;
localparam pkg::t D = 4;
```
Current output:
```systemverilog
  localparam int A = 1;
  localparam my_t BBBB = 2;
  localparam int CC             = 3;
  localparam int unsigned DDDDD = 3;
  localparam pkg::t D = 4;
```
Expected:
```systemverilog
  localparam int A              = 1;
  localparam my_t BBBB          = 2;
  localparam int CC             = 3;
  localparam int unsigned DDDDD = 3;
  localparam pkg::t D           = 4;
```

## Q-10 — A `//` comment inside call arguments leaves a half-broken list

Input:
```systemverilog
foo(a, // first
  b, c);
x = bar(aaaa, bbbb, // why
  cccc, dddd, eeee);
```
Current output:
```systemverilog
    foo(a, // first
      b, c);
    x = bar(aaaa, bbbb, // why
      cccc, dddd, eeee);
```
Expected — a list that must break breaks completely, as it does when a length or count
limit forces it:
```systemverilog
    foo(
      a, // first
      b,
      c
    );
    x = bar(
          aaaa,
          bbbb, // why
          cccc,
          dddd,
          eeee
        );
```

## Q-11 — DPI alias `=` aligned as an assignment

Config: `statement.align = true`.

Input:
```systemverilog
import "DPI-C" c_name = function void sv_alias(string s);
assign aaaa = 1;
assign b = 2;
```
Current output:
```systemverilog
  import "DPI-C" c_name        = function void sv_alias(string s);
  assign aaaa                  = 1;
  assign b                     = 2;
```
Expected — the import is not an assignment and does not set the group's column:
```systemverilog
  import "DPI-C" c_name = function void sv_alias(string s);
  assign aaaa = 1;
  assign b    = 2;
```

## Q-12 — Modport comma after a multi-line prototype

Config: repo config (`modport.align = true`, `function_declaration.layout = "block"`).

Input:
```systemverilog
interface i;
modport mp (input a, import task send(input int x, output bit ok), output b);
endinterface
```
Current output:
```systemverilog
modport mp (
    input          a         ,
    import         task send(
        input int x,
        output bit ok
    )                                                    ,
    output         b
);
```
Expected:
```systemverilog
modport mp (
    input          a         ,
    import         task send(
        input int x,
        output bit ok
    ),
    output         b
);
```

## Q-13 — Small spacing inconsistencies

Input:
```systemverilog
timeunit 1ns/1ps;
property p6; req |-> strong(##[1:$] gnt); endproperty
property p7; req |-> weak(gnt[*1:$]); endproperty
initial u = tagged Pair '{.a, .b};
```
Current output:
```systemverilog
timeunit 1ns / 1ps;
    req |-> strong (##[1:$] gnt);
    req |-> weak (gnt[*1:$]);
    u = tagged Pair'{.a, .b};
```
Expected:
```systemverilog
timeunit 1ns/1ps;
    req |-> strong(##[1:$] gnt);
    req |-> weak(gnt[*1:$]);
    u = tagged Pair '{.a, .b};
```
`strong(`/`weak(` match `first_match(` (P-20). `tagged Pair '{…}` is a tag followed by
its member pattern, not a cast `Pair'{…}`; `tagged Valid .n` already keeps its space.
`1ns/1ps` is a unit/precision pair, not a division — same spirit as min:typ:max (N-14).

## Q-14 — An attribute-led declaration is not aligned

Config: `var_declaration.align = true`.

Input:
```systemverilog
logic [7:0] a;
(* keep *) logic dbg;
logic signed [3:0] bb;
```
Current output:
```systemverilog
  logic               [7:0]               a                                   ;
  (* keep *) logic dbg;
  logic signed        [3:0]               bb                                  ;
```
Expected — the attribute is a prefix, the declaration after it joins the columns:
```systemverilog
  logic               [7:0]               a                                   ;
  (* keep *) logic                        dbg                                 ;
  logic signed        [3:0]               bb                                  ;
```
(Alternative, equally acceptable and simpler: leave it unaligned and document it. This is
the one finding where "by design" is a reasonable answer.)

---

## Fix plan

All locations are in `src/features/formatter_passes.hpp`. Every fix stays inside the pass
that already owns the metadata involved and decides from `TokenKind` facts and
SyntaxPass topology, not from source whitespace — with the one sanctioned exception,
comment role classification (Q-1, Q-6).

Common procedure for each fix:
1. Add the regression case first to `tests/test_formatter_regressions.cpp`, asserting the
   exact expected output **and** `format(format(x)) == format(x)`.
2. Add a negative case next to it (the neighbouring construct that must not change).
3. Run `./build/lazyverilog-tests "[formatter]"`, then `ctest --test-dir build`.
4. Re-run the round-8 harness (18 files × 17 configs, three whitespace variants) and the
   round-7 snapshot, and diff against the stored baseline: only the lines named in the
   finding may change.

| ID | Pass | Cause | Fix | Regression guard |
|----|------|-------|-----|------------------|
| Q-1 | SyntaxPass (`CommentFacts`, ~1745) | `role` is `OwnLine` only when the comment `starts_original_line`. In `/* a */ /* b */ code` only the first does; the others are `Trailing` and stay glued to the code, then become line-leading on the next run. | Carry "every token since the last line start is an `OwnLine` comment" in the SyntaxPass loop; a block comment in that state is `OwnLine` too. After one run each comment starts its own line, so the second run classifies identically. | The three-comment input → final form in one pass; `x = 1; /* a */ /* b */` (trailing after code) unchanged; the existing single-leading-comment and `comma_interstitial_block` tests unchanged. |
| Q-2 | AlignPass (`is_var_decl_line`, ~4501) | The identifier-led path accepts `name [` as a type followed by a dimension and never checks what lies between it and the "name". | Reject the line when a top-level operator token (binary/relational op, `##`, `\|->`, `\|=>`, `->`, `?`) sits before the candidate name — the same token set `declares()` uses at ~1280. Hoist that set into one helper so both callers share it. | Constraint, `if`-in-constraint, sequence and property lines unchanged under both aligners; `my_t [3:0] x;` and `pkg::t q [$];` still aligned (existing P-10/P-11 tests). |
| Q-3 | SpacingPass helper `is_var_declaration_trailing_dimension_open` (`declares()`, ~1170) | `declares()` counts identifier-like tokens ≥ 2 without tracking bracket depth, so `w[i][j]` counts `w`, `i`, `j` and the later-declarator path spaces `a [i]`. | Count identifiers only at the element's own bracket/paren depth. | The three inputs unchanged; `logic a [3], b [4];` and `int x [2], y [3];` still spaced (I-/P-12 tests). |
| Q-4 | AlignPass (default-width and section aligners, ~4631–4852) | The var aligner treats a port-direction line as a declaration: the `port_decl` branch takes `type_first` with no comma handling, so `input a, b;` reads `a` as the type. | When `port_declaration.align` is off, skip lines led by a port direction in the var aligner (they belong to the port aligner, which is off). First check whether an existing test pins aligned body-level ports under `var_declaration.align` alone; if one does, instead fix the branch to take the first declarator as the name and use one name column. | The input above; a non-ANSI task body; `port_declaration.align = true` output byte-identical to today. |
| Q-5 | SyntaxPass predicate `follows_module_item_boundary` / `is_basic_sv_item_boundary` (~1415) | The boundary list has `;`, `end`, `endgenerate`, `endtask`, `endfunction` but not `endclocking`. | Add the remaining item-closing keywords: `endclocking`, `endgroup`, `endproperty`, `endsequence`, `endchecker`, `endspecify`, `endprimitive`-level ones that can precede a module item. | An instance after each added keyword expands; `endclocking : cb` with a label followed by an instance; none/`instance.align`. |
| Q-6 | WrapPass (`apply_list`) | The list puts the break after the comma; a `Trailing` block comment after it is then on the next line. Only the `;` path (~2648) and the instance list defer the break past a same-line trailing comment. | In `apply_list`, when an item's comma is followed by a `Trailing` comment (role decided by SyntaxPass), put `must_break_after` on the comment instead of the comma — one shared helper for port, enum, argument and modport lists, replacing the instance-only handling. | Port, enum and broken-call inputs; a comment that genuinely leads the next item (own line in source) stays leading; port alignment row count unchanged; idempotent. |
| Q-7 | AlignPass (instance port alignment, ~5728) | `sw = compact_width(…)` over-measures selects and calls relative to what SpacingPass renders, so the widest row is padded. | Measure with `rendered_width`, as the statement aligner does. | The three instances; a connection with a concatenation and one with a nested call; existing instance-align tests byte-identical where no select/call is the widest. |
| Q-8 | WrapPass (declaration-formals check, ~3207) | `is_decl` covers `function`/`task` and a name after `sequence`/`property`/`let`. | Add `CoverGroupKeyword` and `CheckerKeyword` to that set. | The two inputs under `always` and `arg_count`; a real call `cg_inst = new(1, 2);` still broken. |
| Q-9 | AlignPass (statement aligner `push_line`, ~4276) | `identifiers_before_assign >= 2` disables the line; it exists to keep `packet_t v = …` out, but `localparam my_t B = 2` trips it too, while `localparam int B` (a keyword type) does not. | Skip the two-identifier rule when the line's first token is `parameter`/`localparam` at paren depth 0 — the keyword already says it is a declaration, and its keyword-typed sibling is aligned today. | The five-line input; `packet_t v = f();` still not aligned; `#(parameter my_t P = 1, …)` unchanged (P-18). |
| Q-10 | WrapPass (`apply_list` for call arguments) | A trailing `//` forces one break inside the list, but the list's own break decision is still "fits". | Treat a `Trailing` line comment inside a call's argument list as a reason to break the whole list, as the length and count limits are. | The two inputs under none and `always`; a `//` after the closing `);` does not break the call; nested call rule (inner never broken) kept. |
| Q-11 | AlignPass (statement aligner `push_line`) | The DPI alias `=` is an `Equals` token at depth 0. | Disable the line when it starts with `import`/`export` — SyntaxPass already freezes DPI import lines (~403). | The input; `assign` group column no longer includes the import. |
| Q-12 | AlignPass (modport alignment, ~5841) | `sw = compact_width(sig, item.last)` measures the whole prototype, and the comma is placed at that column although the item ends on a different line. | Do not align the comma (and do not let the item widen `sigw`) when the item contains a paren whose `list_kind` was broken by WrapPass. | The input under repo config; a single-line `import task send(input int x)` still aligned; plain modports byte-identical. |
| Q-13 | SpacingPass (~6303) and `dot_keeps_space_after` neighbours (~397) | `strong`/`weak` are not in the `first_match` no-space rule; `'{` after `tagged <id>` is treated as a cast; `/` in `timeunit` is a binary operator. | Three one-line rules keyed on `TokenKind`: `StrongKeyword`/`WeakKeyword` before `(` → 0 spaces; `ApostropheOpenBrace` whose previous two tokens are `tagged <id>` → 1 space; `Slash` inside a `timeunit`/`timeprecision` statement → 0 spaces both sides. | Each input; `int'(x)`, `my_t'{…}` and `a / b` unchanged. |
| Q-14 | AlignPass (`is_var_decl_line`) | The line's first token is `(*`. | Start the declaration scan after a leading attribute instance and treat its rendered width as a prefix of section 1, the way `assign ` and case labels are prefixes in the statement aligner. If that perturbs existing snapshots, document it as intended instead. | The input; `(* keep *) sub u (...)` (an instance) untouched. |

### Order and risk

1. **Q-1** first: the only idempotency failure, one SyntaxPass classification.
2. **Q-2, Q-3, Q-5, Q-8, Q-13**: predicate-only changes, each narrows or widens a
   `TokenKind` set. Lowest regression risk.
3. **Q-7, Q-9, Q-11, Q-12**: AlignPass width/eligibility changes; diff the harness
   snapshots for every align config.
4. **Q-6, Q-10**: WrapPass list behaviour; the widest blast radius, do them last and
   one at a time with a full snapshot diff between them.
5. **Q-4, Q-14**: need a look at existing tests first, since either could be pinned
   behaviour.

## Status

All 14 findings are fixed, one commit per finding, each with a regression test in
`tests/test_formatter_regressions.cpp` that asserts the exact output and that a second
run changes nothing.

| ID | Commit | Notes |
|----|--------|-------|
| Q-1 | `5e2e42b` | As planned. |
| Q-2 | `e5830bf` | As planned; the operator set is shared through `is_expression_operator()`. |
| Q-3 | `bcdcbff` | As planned. |
| Q-4 | `49d1009` | No test pinned the old layout, so the variable aligner skips port-direction lines unless `port_declaration.align` is on. With both on the output is unchanged. |
| Q-5 | `da5403d` | Added `endclocking`, `endgroup`, `endproperty`, `endsequence`, `endchecker`, `endspecify`, `endclass`. |
| Q-6 | `ecf54ce` | SyntaxPass records a new `CommentFacts::ends_line`; WrapPass reads that fact instead of source trivia. A comment with code after it on its line (`q, /* lead */ output logic r`) still leads that code. |
| Q-7 | `c22962f` | Measured from `(` to `)`, which also fixes the widest row overrunning the column under `space_inside_parens` (a second defect found while fixing). |
| Q-8 | `b082f48` | As planned. |
| Q-9 | `b80a836` | As planned. |
| Q-10 | `dae7e83` | Applies to block and hanging layouts. A comment inside a *nested* call still changes nothing (nested calls are never broken). |
| Q-11 | `ac4c49c` | As planned. |
| Q-12 | `9c32dbc` | A multi-line item keeps its comma and no longer widens the signal column. |
| Q-13 | `c5cc4c4` | `strong(`/`weak(` and `tagged Pair '{` fixed. **`timeunit 1ns / 1ps` is left as is**: the LRM writes the pair with spaces, so that part of the finding was wrong. |
| Q-14 | `ec645c6` | Implemented rather than documented: the aligners read a line from the first token after a leading attribute. |

Verification after the last commit:

- `ctest --test-dir build`: 1005 of 1005 pass. `[formatter]`: 981 assertions in 361 cases
  (was 931 in 347).
- Round-7 and round-8 corpora, 36 files × 17 configs: no crash, token stream intact,
  **every file idempotent** (the Q-1 file included).
- After each fix the snapshot of all 612 outputs was diffed against the one before it;
  only lines the finding names changed. One knock-on is worth knowing: with Q-6 a port
  line that used to start with a moved comment is a plain port line again, so under
  default-width `var_declaration.align` its `output` keyword counts toward the shared
  keyword width as any other port line's does.
- Whitespace variants (no indentation, tabs, CRLF) still format identically apart from
  verbatim regions.
