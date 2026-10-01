# Formatter Bug Report — Round 7

Findings from a seventh stress test of `lazyverilog-fmt` on `fix/formatter-stress-bugs`
at `597f5c6` (all of rounds 1–6 fixed). Each issue has a fix plan, collected under
"Fix plan" at the end. **19 of the 21 are now fixed**; P-21 was judged not a bug and
P-19 is deferred. See "Status" at the end.

## What was tested

20 new RTL files in styles rounds 1–6 did not cover, each under three configs
(**none**, **repo** = the repo's `lazyverilog.toml`, **alt** = the round-5 alt config).
The two edge-case files also ran under four single-option configs. Every file was then
re-run as three whitespace variants: indentation stripped, random tabs, and CRLF.
About 40 single-option repros followed.

- **Types**: packed/unpacked structs and unions, `typedef` with unpacked and associative
  dimensions, packed dimensions after a struct body, enums with ranges, forward typedefs.
- **Declarations**: every net type (`tri0`, `wand`, `supply0`, `uwire`), `genvar`, class
  properties with `local`/`protected`/`static`/`rand`, `virtual` interfaces,
  parameterized class handles, multi-declarator lines, non-ANSI and ANSI parameters.
- **Case forms**: `case (1'b1)` with select labels, `case … inside`, `casez` with `?`,
  attribute-prefixed `(* full_case *) case`, `unique0`/`priority`.
- **SVA**: `assert property` with `disable iff`, parenthesised antecedents, sequence
  match items `(req, v = d)`, repetitions, `first_match`, `throughout`/`within`.
- **Directives**: `` `ifdef``/`` `else`` branches that each open a `begin`, a directive
  inside an event control, inside an expression, inside an instance port list.
- **Format-off regions**: at module level, inside `begin…end`, inside a port list,
  unterminated, and immediately before a wrapped construct.
- **Comments**: after `(`, between `always_comb` and `begin`, own-line inside a
  continued expression, trailing on the last list item, leading-comma lists.
- **Long lines**: calls of 95–115 columns at four nesting depths, `new(...)`,
  `cls #(…)::f(...)`, calls in `assign`, `localparam`, `return`, and port connections.
- **Literals**: macro-sized (`` `WIDTH'h3``), unbased, real, time, `'x`/`'z`, strings.

## Safety results

- No crashes.
- **Every file is idempotent under every config**: 68 of 68 runs (18 files × 3 configs, 2 files × 7).
- With whitespace ignored, the token stream is unchanged everywhere.
- The three whitespace variants format identically to the original, apart from the
  bytes inside verbatim regions (format-off, multi-line comments and strings).

So all 21 findings are wrong layout. None is an idempotency or token-safety bug.

## Excluded (settled in earlier rounds, or pinned by tests)

Not reported: `wait(c)`, `'{key : value}`, `begin: label` / `endclass: name`,
`assert … else` on three lines, `0:;`, joined user line breaks in expressions, a
brace-less body moved to its own line, `` `ifdef`` at column 0, block call layout
anchored to the callee (K-1), a nested call never broken, blank lines removed inside
`()`/`{}`, statement alignment switched off inside loop `begin…end` bodies (by design,
see the comment in `AlignPass::run`), and the default-width `logic valid, ready ;` layout (N-2).

## Summary

| ID | Severity | Kind | Config | Issue |
|----|----------|------|--------|-------|
| P-1 | High | indent | none | The first wrapped list after a `// verilog_format: on` marker loses its indent: items and `)` land near column 0 |
| P-2 | High | indent | none | `` `ifdef``/`` `else`` branches that each open a `begin` count the block twice; the rest of the module is one level too deep |
| P-3 | High | align | `var_declaration.align` with section widths | A case label with a select (`x[0]: y = 1;`) is aligned as a variable declaration |
| P-4 | High | wrap | `function_call.line_length` | The length check ignores indentation, so a call is only broken at `line_length + indent + 1` columns |
| P-5 | Medium | wrap | none; worse with `break_policy = "always"` | A parenthesised expression after `)` is wrapped as a call's arguments |
| P-6 | Medium | indent | none | An attribute in front of a brace-less `case`/`if` body ends the body early: `endcase` and `else` fall back to the header's column |
| P-7 | Medium | spacing | none | The last select in a sequence or property expression is spaced like a declaration: `a \|-> b [0];` |
| P-8 | Medium | spacing | none | A macro-sized literal gets a space: `` `WIDTH 'h3`` |
| P-9 | Medium | layout | none | A packed dimension after a struct body moves to its own line |
| P-10 | Medium | align | `var_declaration.align` (default widths) | The default-width aligner skips declarations led by a user type or a qualifier (`req_t r;`, `rand int r;`) |
| P-11 | Low | align | `var_declaration.align` with section widths | Net types, `genvar`, `local`/`protected`, `virtual` and parameterized-type declarations are not aligned |
| P-12 | Low | spacing | none | Unpacked-dimension spacing differs by context: `int q[$]`, `typedef int aa_t[string]` and `(* … *) reg mem[0:255]` stay unspaced |
| P-13 | Low | indent | none | A `//` comment after a function header's `(` leaves the arguments two levels deep and `)` glued to the last one |
| P-14 | Low | indent | none | Inside an event control, the line after a `` `ifdef`` gets no continuation indent |
| P-15 | Low | indent | none | An own-line comment inside a continued expression sits at the statement's indent, left of the lines around it |
| P-16 | Low | align | `port_declaration.align` | The last port's trailing comment is not aligned with the others |
| P-17 | Low | layout | none | A leading-comma port after a trailing comment leaves `,` on a line of its own |
| P-18 | Low | align | `statement.align` | The second declarator in a `#(…)` list is padded like an assignment: `D2         = 4,` |
| P-19 | Low | wrap | `function_call.arg_count` | An empty argument is not counted, so `show("x", , da)` stays joined while `show("x", y, da)` breaks |
| P-20 | Low | spacing | none | `first_match(s)` gets a space before `(` |
| P-21 | Low | layout | `statement.begin_newline = true` | A struct/union `{` moves to its own line; the option documents only `begin` and constraint braces, and an enum `{` stays |

---

## P-1 — The first wrapped list after a format-on marker loses its indent

**Kind:** indent. **Options:** none.

Before:

```systemverilog
module m;
  // verilog_format: off
  wire   a  =  1;
  // verilog_format: on
  sub u (
    .a(a),
    .d(d)
  );
  initial begin
    // verilog_format: off
    x  =  1;
    // verilog_format: on
    foo(aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb, cccccccccccccccccccccccccc, dddddddddddd);
  end
endmodule
```

After (current):

```systemverilog
  // verilog_format: on
  sub u(
  .a(a),
  .d(d)
);
  ...
    // verilog_format: on
    foo(
   aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,
   bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,
   cccccccccccccccccccccccccc,
   dddddddddddd
 );
```

Expected:

```systemverilog
  // verilog_format: on
  sub u(
    .a(a),
    .d(d)
  );
  ...
    // verilog_format: on
    foo(
      aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,
      bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,
      cccccccccccccccccccccccccc,
      dddddddddddd
    );
```

One ordinary statement between the marker and the list (`wire b;`) makes the output correct.

**Cause.** `IndentPass`'s `line_start_of()` walks back until it sees a
`must_break_before`/`must_break_after` flag. Verbatim tokens carry no wrap flags, so the
walk runs through the `on` marker into the disabled region and returns a passthrough
token. The main loop skips passthrough tokens, so that token's `base_indent` is 0, and
the list's `base` and `name_col` are measured from there.

**Fix (IndentPass).** `line_start_of()` stops at a verbatim boundary: when `tokens[n-1]`
is a format marker or a disabled-region token (`lex.is_format_on_marker`,
`is_format_off_marker`, `is_disabled_region_body`), the line starts at `n`. Do the same
in `line_prefix_width()` so WrapPass does not count the disabled region's width into the
first line after it. An inline whitespace-sensitive macro is also passthrough but is not
a line boundary, so the test must be on the marker flags, not on `is_passthrough()`.

## P-2 — `` `ifdef``/`` `else`` branches that each open a `begin`

**Kind:** indent. **Options:** none.

Before:

```systemverilog
module m;
`ifdef A
  always_ff @(posedge clk) begin
`else
  always_ff @(posedge clk or negedge rst_n) begin
`endif
    q <= d;
  end
  assign z = 1;
endmodule
```

After (current):

```systemverilog
module m;
`ifdef A
  always_ff @(posedge clk) begin
`else
    always_ff @(posedge clk or negedge rst_n) begin
`endif
      q <= d;
    end
    assign z = 1;
endmodule
```

Expected: unchanged input. This is the standard way to write an optional async reset.

**Cause.** `IndentPass::run` saves the level at `` `ifdef`` in `branches`, but at
`` `else``/`` `elsif`` it restores it only when the branch opened a design unit
(`outer_units.size() > branches.back().outer_units`). A branch that opened a `begin`
keeps its `++level`, and the second `begin` adds another. One `end` closes one of them.

**Fix (IndentPass).** At `` `else``/`` `elsif``, always restore `level` and trim
`open_bodies` to the values saved at the `` `ifdef``; keep the `outer_units` resize
under its existing condition. Each branch then starts from the same level, and the code
after `` `endif`` continues from the last branch's result. That is correct whenever the
branches open the same number of scopes, which is the only case with a right answer.
The existing module-header test (`` `ifdef A module m (…); `else module m (…); `endif``)
is a special case of the new rule and must still pass.

## P-3 — A case label with a select is aligned as a declaration

**Kind:** align. **Options:** `var_declaration.align = true` with `section*_min_width`
set (the repo config).

Before:

```systemverilog
case (1'b1)
  x[0]: y = 1;
  mem[i][j]: y = 2;
  default: y = 0;
endcase
```

After (current):

```systemverilog
case (1'b1)
  x                   [0]:                y                   = 1             ;
  mem                 [i][j]:             y                   = 2             ;
  default: y = 0;
endcase
```

Expected: unchanged. Also hit: `P[0]:`, `x[3:0] == 4'h1:`, `pkg::C[1]:`.

**Cause.** `is_var_decl_line` (sectioned aligner in `AlignPass`) accepts a line that
starts with an identifier followed by `[`: `x` reads as the type, `[0]` as its packed
dimension and `y` as the declarator.

**Fix (AlignPass).** In `is_var_decl_line`, reject the line when any token between
`ln.first` and the `=`/`;` has `topology.is_case_item_colon` set. The scan that rejects
other assignment operators already walks that range, so the check goes in that loop. A
declaration never contains a case-item colon, so no real declaration is lost.

## P-4 — The call length check ignores indentation

**Kind:** wrap. **Options:** `function_call.line_length` (default 100).

Before (104 columns, inside `function` in `module`):

```systemverilog
    return compute_something(argument_one_long, argument_two_long, argument_three_long_xyz, argument_4);
```

After (current): unchanged. Measured longest call line left unbroken:

| Indent | Longest unbroken line |
|--------|-----------------------|
| 4 | 105 |
| 6 | 107 |
| 8 | 109 |
| 12 | 113 |

Expected:

```systemverilog
    return compute_something(
      argument_one_long, argument_two_long, argument_three_long_xyz, argument_4
    );
```

(or the layout the configured `function_call.layout` gives). The option is documented
as "break when line exceeds `line_length`".

**Cause.** WrapPass runs before IndentPass. `line_prefix_width()` sums token widths from
the line's first token, so the indent is not in the number compared with `line_length`.
The same holds for `function_declaration.line_length` and the `#(…)` fit test.

**Fix (SyntaxPass + WrapPass).** Indentation cannot be read in WrapPass, and moving the
decision after IndentPass would break pass ownership. The block depth is a syntax fact:

1. `SyntaxPass` writes a new `TopologyFacts::scope_depth`: the number of open indent
   scopes at the token, using the same `opens_indent_scope` / `closes_indent_scope`
   facts and design-unit rule IndentPass uses.
2. `line_prefix_width()` adds `scope_depth * indent_size` for the line's first token.
3. `IndentPass` is not changed.

A brace-less control body is one level deeper than `scope_depth` says, so the estimate
is short by one indent there. That is still closer than today, and it never overshoots.
The number depends only on tokens, so the result is idempotent.

**Decision needed.** This changes output for every existing line between
`line_length - indent` and `line_length + indent` columns. It is the documented
behaviour, but it will reflow user code on upgrade; it should go in its own commit with
a release-note line, and the corpus diff should be reviewed before merging.

## P-5 — A parenthesised expression after `)` is wrapped as a call

**Kind:** wrap. **Options:** none for the first example; `break_policy = "always"` for
the second.

Before:

```systemverilog
ap1: assert property (@(posedge clk) disable iff (!rst_n) (req && some_long_signal_name && another_long_signal_name) |-> ##[1:10] (gnt || timeout_signal_name));

property p; @(posedge clk) (req, v = d) |-> gnt; endproperty
```

After (current):

```systemverilog
  ap1: assert property (@(posedge clk) disable iff (!rst_n) (
                                                            req && some_long_signal_name && another_long_signal_name
                                                          ) |-> ##[1:10] (gnt || timeout_signal_name));

  property p;
    @(posedge clk) (
                   req,
                   v = d
                 ) |-> gnt;
  endproperty
```

Expected: both left on one line. Neither `(` is a call.

**Cause.** `SyntaxPass` sets `starts_argument_list` for any `(` whose previous code
token is an identifier, a macro **or a `)`**. The `)` case exists for `f(a)(b)` and
`cls #(T)::m(...)`-style chains, but it also matches the `(` after an event control
`@(...)`, after `disable iff (...)`, and after a control header.

**Fix (SyntaxPass).** When the previous token is `)`, look at what opened it. Do not set
`starts_argument_list` when the matching `(` follows `@`, `iff`, `#`/`##` (a delay or a
cycle delay), or a control keyword (`closes_control_header()` already answers the last).
All four are `TokenKind` checks on the token before the matching `(`. Chained calls and
parameterized-class calls keep their flag because their first `(` follows an identifier
or a `#(` that `starts_parameter_list` already marks.

## P-6 — An attribute in front of a brace-less body

**Kind:** indent. **Options:** none.

Before:

```systemverilog
always @(posedge clk)
  (* full_case *) case (x)
    1: y = 1;
  endcase
always_comb (* mark *) if (a) b = 1; else b = 2;
```

After (current):

```systemverilog
  always @(posedge clk)
    (* full_case *) case (x)
      1: y = 1;
  endcase
  always_comb
    (* mark *) if (a)
      b = 1;
  else
    b = 2;
```

Expected:

```systemverilog
  always @(posedge clk)
    (* full_case *) case (x)
      1: y = 1;
    endcase
  always_comb
    (* mark *) if (a)
      b = 1;
    else
      b = 2;
```

Inside `begin…end` both are already correct.

**Cause.** `controlled_body_extents()` starts the body at the attribute's `(`.
`simple_statement_end_from()` is then asked where a statement starting with `(` ends,
and answers with the first `;` instead of the `endcase` or the `else` branch's end.

**Fix (shared helper used by IndentPass).** In `controlled_body_extents()::add`, keep
the body *start* at the attribute so it indents with the body, but compute the *end*
from the first token after the attribute run (skip tokens with
`lex.in_attribute_instance`). The procedural and `else` callers pass an explicit `end`,
so the skip belongs in a small `statement_keyword_after_attributes()` helper that all
of them call before `simple_statement_end_from()`.

## P-7 — The last select in a sequence or property is spaced like a declaration

**Kind:** spacing. **Options:** none.

Before:

```systemverilog
property p; @(posedge clk) a |-> b[0]; endproperty
sequence s; !gnt ##1 d[0] [=2]; endsequence
```

After (current):

```systemverilog
    @(posedge clk) a |-> b [0];
    !gnt ##1 d [0][=2];
```

Expected: `a |-> b[0];` and `!gnt ##1 d[0] [=2];`.

**Cause.** `is_var_declaration_trailing_dimension_open()` falls back to "two
identifier-like tokens before the bracket and no `.`" for user-defined types. `a` and
`b` are two identifiers, and the bracket is followed by `;`.

**Fix (the shared predicate, read by SpacingPass).** In the `declares` lambda, return
false when the element contains any operator token at its own depth: a binary operator
(`is_binary_op`), a property operator (`is_property_operator_keyword`, `|->`, `|=>`,
`##`), or a unary operator. A declaration's prefix is only keywords, identifiers, `::`,
`#(…)` and dimensions. The second example then needs one more rule: a repetition
bracket keeps its space after `]`, as it does after an identifier.

## P-8 — A macro-sized literal gets a space

**Kind:** spacing. **Options:** none.

Before: ``assign z = `WIDTH'h3 + `W'd1;``

After (current): ``assign z = `WIDTH 'h3 + `W 'd1;``

Expected: unchanged.

**Cause.** `SpacingPass` closes the gap before an `IntegerBase` only when the token to
its left is an `IntegerLiteral`.

**Fix (SpacingPass).** Extend that rule to a left token of kind `MacroUsage`, and to
the `)` that closes a macro call (`` `W(8)'h3``, found through `matching_token` and the
token before it). The size-walk in SyntaxPass (near `IntegerBase` at line 1836) should
accept `MacroUsage` as the size as well, so the literal is one operand to the wrap pass.

## P-9 — A packed dimension after a struct body

**Kind:** layout. **Options:** none.

Before: `typedef struct packed { logic [3:0] x; } [1:0] pair_t;`

After (current):

```systemverilog
  typedef struct packed {
    logic [3:0] x;
  }
  [1:0] pair_t;
```

Expected:

```systemverilog
  typedef struct packed {
    logic [3:0] x;
  } [1:0] pair_t;
```

**Cause.** In `WrapPass`, `close_brace_before_decl_name` keeps `}` on the line only
when the next token is an identifier.

**Fix (WrapPass).** Also keep it when the next token is `[` and the `}` closes a struct,
union or enum body (`is_struct_or_union_body_brace(matching_token)` or the enum scan
already used for `EnumBody`). SpacingPass then needs one space between `}` and `[`.

## P-10 — The default-width aligner skips user types and qualifiers

**Kind:** align. **Options:** `var_declaration.align = true`, section widths left at 0.

Before:

```systemverilog
logic [7:0] x;
int a;
req_t req_q, req_d;
pkg::cfg_t cfg;
rand int r;
```

After (current):

```systemverilog
  logic [7:0]                         x                             ;
  int                                 a                             ;
  req_t req_q, req_d;
  pkg::cfg_t cfg;
  rand int r;
```

Expected: all five lines in the same columns, as the sectioned aligner already does.

**Cause.** The default-width path (`AlignPass`, the block starting at
`if (opts_.var_declaration.align)`) accepts a line only when its first token is
`is_type_keyword` or a port direction. The sectioned path uses `is_var_decl_line`,
which also accepts `is_var_decl_leading_keyword`, `var`, and a user or scoped type.

**Fix (AlignPass).** Hoist `is_var_decl_line` above both paths and use it as the single
"is this a variable declaration" test. The default-width path keeps its own column
maths and its port-direction branch. N-10 fixed this for the sectioned path only.

## P-11 — Declaration kinds the aligner does not recognise

**Kind:** align. **Options:** `var_declaration.align = true` with section widths.

Before:

```systemverilog
logic [7:0] x;
tri0 t0;
supply0 gnd;
genvar g;
virtual my_if vif;
mailbox #(item_c) mbx;
```

After (current): only `logic [7:0] x;` is aligned; the other five lines are unchanged.
Also skipped: `wand`, `uwire`, `tri [3:0] tb;`, `local int b;`, `protected bit [3:0] p;`,
`local static int ls;`, `base_c #(int)::this_t h;`.

Expected: all aligned in the name column.

**Fix (AlignPass, with helpers at the top of `formatter_passes.hpp`).**

- Add an `is_net_type_keyword(TK)` helper (`tri`, `tri0`, `tri1`, `triand`, `trior`,
  `trireg`, `wand`, `wor`, `uwire`, `supply0`, `supply1`) and accept it, `genvar`,
  `local`, `protected` and `virtual` in `is_var_decl_line`'s `plausible_start`.
- For a user type, step over a `#(…)` after the type name before looking for the
  declarator, the same way the scoped-name loop steps over `::`.

Do not add these to `is_var_decl_leading_keyword` itself: that predicate also feeds the
spacing rule in P-12, and `virtual`/`local` also lead method declarations.
`starts_variable_declaration` already rejects a line holding `function`/`task`.

## P-12 — Unpacked-dimension spacing differs by context

**Kind:** spacing. **Options:** none.

Before:

```systemverilog
function void g();
endfunction
int q[$], da[];
typedef int aa_t[string];
typedef req_t r_t[4];
(* ram_style = "block" *) reg [7:0] mem[0:255];
logic u[4];
```

After (current):

```systemverilog
  int q[$], da[];
  typedef int aa_t[string];
  typedef req_t r_t [4];
  (* ram_style = "block" *) reg [7:0] mem[0:255];
  logic u [4];
```

Expected: a space before each unpacked dimension, as `logic u [4];` and
`typedef req_t r_t [4];` already get.

**Cause.** Three gaps in `is_var_declaration_trailing_dimension_open()`'s `element_start`
/ `declares`:

1. The backward walk does not stop at `endfunction`/`endtask`, so the element starts
   inside the previous function and its first token is not a type keyword.
2. `typedef` is not a leading keyword, and `int aa_t` has only one identifier.
3. An attribute instance is the element's first token.

**Fix (the shared predicate).** In `element_start`, stop at a token for which
`is_close_block()` or `is_outer_close()` is true, and skip a leading run of
`in_attribute_instance` tokens. In `declares`, treat `TypedefKeyword` as a leading
keyword. All three are `TokenKind` facts.

## P-13 — A comment after a function header's `(`

**Kind:** indent. **Options:** none.

Before:

```systemverilog
function int f( // after paren
    int a,
    int b
);
```

After (current):

```systemverilog
  function int f( // after paren
      int a, int b);
```

Expected:

```systemverilog
  function int f( // after paren
    int a,
    int b
  );
```

**Cause.** The header fits `function_declaration.line_length`, so `WrapPass` does not
apply a list layout. The `//` comment still ends the line, and the arguments then take
a continuation indent on top of the body level.

**Fix (WrapPass).** In the `is_decl` branch, also apply the declaration layout when the
list holds a line-ending comment: reuse the `breaks_inside` scan the `#(…)` branch has
(a `//` comment or an own-line comment between `open` and `close`).

## P-14 — A `` `ifdef`` inside an event control

**Kind:** indent. **Options:** none.

Before:

```systemverilog
always @(posedge clk
`ifdef HAS_RST
         or negedge rst_n
`endif
) begin
```

After (current):

```systemverilog
  always @(posedge clk
`ifdef HAS_RST
  or negedge rst_n
`endif
  ) begin
```

Expected:

```systemverilog
  always @(posedge clk
`ifdef HAS_RST
    or negedge rst_n
`endif
  ) begin
```

**Cause.** In WrapPass's continuation sweep, the line after `` `ifdef`` continues from
the token before the directive (`clk`). The `event_or` rule looks only at a *previous*
token of kind `or`; a line that *starts* with `or` is not covered.

**Fix (WrapPass).** Add "the line's first token is `or` or `,` and `paren_depth > 0`
(or `in_property_expr`)" to `continues`, next to the existing rule for a line led by a
binary operator.

## P-15 — An own-line comment inside a continued expression

**Kind:** indent. **Options:** none.

Before:

```systemverilog
y = a + // plus
    b - c
    // own-line in expr
    + d;
```

After (current):

```systemverilog
    y = a + // plus
      b - c
    // own-line in expr
      + d;
```

Expected: the comment at the continuation indent, level with `+ d;`.

**Cause.** The continuation sweep tests whether the *comment* continues the previous
line. A comment is not a binary operator and `c` does not end with one, so it gets no
continuation. The next line gets it because it starts with `+`.

**Fix (WrapPass).** After the sweep, give an own-line comment the `continuation` flag
of the next code token when that token starts a line. This reads comment *role*, which
the architecture allows; it does not read input whitespace.

## P-16 — The last port's trailing comment

**Kind:** align. **Options:** `port_declaration.align = true`.

Before:

```systemverilog
module m (
  input [W-1:0] d, // data in
  output reg [W-1:0] q // data out
);
```

After (current):

```systemverilog
  input                   [W-1:0]     d                       , // data in
  output      reg         [W-1:0]     q // data out
```

Expected: `// data out` in the same column as `// data in`.

**Cause.** The comment column comes from the padding in front of the comma. The last
port has no comma, so nothing is padded. N-17 fixed the same thing for enum items.

**Fix (AlignPass).** In the port aligner, give the last item's trailing comment the
padding its comma would have had plus the comma's width, as the N-17 enum fix does.

## P-17 — A leading-comma item after a trailing comment

**Kind:** layout. **Options:** none.

Before:

```systemverilog
module n (input a // c
  , input b
);
```

After (current):

```systemverilog
module n(
  input a // c
  ,
  input b
);
```

Expected:

```systemverilog
module n(
  input a // c
  , input b
);
```

The same happens for `.a(a) // c` / `,.b(b)` in an instance. A parameter list already
keeps `, parameter D = 4` together.

**Cause.** `apply_list` sets `must_break_after` on every top-level comma. Here the
comment has already ended the line before the comma, so the comma starts a line and
then ends it.

**Fix (WrapPass).** In `apply_list`, do not set `must_break_after` on a comma whose
previous token is a `//` comment or a trailing comment that ends the line. The break
between the two items already exists. Moving the comma in front of the comment would
read better, but it reorders tokens around a comment and is out of scope.

## P-18 — The second declarator in a `#(…)` list is padded like an assignment

**Kind:** align. **Options:** `statement.align = true` (the repo config).

Before:

```systemverilog
module m #(
  parameter W2 = DATA_W * 2, D2 = 4,
  parameter real R = 1.5
) (input logic clk);
```

After (current):

```systemverilog
module m #(
    parameter W2 = DATA_W * 2,
    D2         = 4,
    parameter real R = 1.5
)(
```

Expected: `D2 = 4,`.

**Cause.** The line `D2 = 4,` starts with an identifier and holds `=`, so the statement
aligner takes it as a one-line assignment group and pads it to `lhs_min_width`.

**Fix (AlignPass).** In the statement aligner's `push_line`, skip a line whose first
token has `paren_depth > 0`. `starts_variable_declaration` uses the same test for the
same reason: a later line of a wrapped list is placed by the list's own layout.

## P-19 — An empty argument is not counted

**Kind:** wrap. **Options:** `function_call.arg_count = 3`.

Before: `show("x", , da); show("x", y, da);`

After (current):

```systemverilog
    show("x", , da);
    show("x",
         y,
         da);
```

Expected: both calls broken; each has three arguments.

**Cause.** `top_level_list_items()` returns no item for an empty argument, and
`items.size()` is compared with `arg_count`.

**Fix (WrapPass).** For the `arg_count` and `break_policy = "always"` tests, count
top-level commas plus one instead of `items.size()`. IndentPass already has the code
that indents a comma-only item.

## P-20 — `first_match(s)` gets a space

**Kind:** spacing. **Options:** none.

Before: `sequence s2; first_match(s_a); endsequence`

After (current): `first_match (s_a);`

Expected: `first_match(s_a);`, as the LRM writes it.

**Fix (SpacingPass).** Add `FirstMatchKeyword` to the keywords whose `(` attaches
without a space. Check the spacing rule that puts a space after a keyword before `(`
and add the exception there; do not touch `is_control_keyword`.

## P-21 — `begin_newline` moves a struct `{` (not a bug)

> **Resolution: intended behaviour.** `begin_newline` applies to every block-opening
> brace, a `struct`/`union` body's included; an `enum` brace holds a list and stays.
> The option's documentation was what was wrong and has been corrected. No code change.
> The finding is kept below as it was reported.

**Kind:** layout. **Options:** `statement.begin_newline = true`.

Before:

```systemverilog
typedef struct packed { logic a; } s_t;
typedef enum logic { A, B } e_t;
```

After (current):

```systemverilog
  typedef struct packed
  {
    logic a;
  } s_t;
  typedef enum logic {
    A,
    B
  } e_t;
```

Expected: `typedef struct packed {` on one line. `docs/formatter/options.md` says the
option "applies to `begin` and to constraint block braces", and the enum next to it is
not moved.

**Cause.** In WrapPass the option applies to any `{` at `paren_depth == 0` that is not
an expression brace, and again to any multi-line brace construct.

**Fix (WrapPass).** Exclude `is_struct_or_union_body_brace()` from both
`begin_newline` conditions. The coverpoint test in `tests/test_formatter.cpp:3815`
pins coverpoint braces as following the option; that stays.

**Decision needed.** If struct braces are meant to follow the option, the fix is
instead to document it and move the enum `{` too.

---

## Noted, not counted

- `always #5 clk = ~clk;` becomes `always #5` / `clk = ~clk;`, while
  `forever #5 clk = ~clk;` becomes `forever` / `#5 clk = ~clk;`. The two put the delay
  on different lines. Both follow "a brace-less body gets its own line"; pick one.
- The lines of a multi-line `/* … */` comment after the first keep their original
  columns when the comment's indent changes, so a box comment loses its shape.
- `// verilog_format: off` and `on` marker lines keep their original indent.
- `obj.m1(...).m2(...).m3(` hangs its arguments at column 70 (K-1, by design).

## Fix plan

### Rules for every fix

- One commit per ID, in the pass named above. No pass writes another pass's metadata.
- Every new decision reads `TokenKind`, `SyntaxFacts`, `TopologyFacts` or comment role.
  None reads `input_trivia`.
- Each fix adds a test to a new `// Round 7 (FORMAT_BUG_FIX7.md)` section in
  `tests/test_formatter_regressions.cpp`, using `format_stable()` so idempotency is
  asserted with the layout. Each test holds the "Before" input above and one neighbour
  case that must *not* change (listed below).
- After each commit: `./build/lazyverilog-tests "[formatter]"`, then `ctest --test-dir
  build`, then format the round 1–7 corpora under none/repo/alt and diff against the
  output from before the commit. Any diff outside the lines the fix targets blocks it.

### Order

| Step | IDs | Pass | Neighbour case that must not change |
|------|-----|------|-------------------------------------|
| 1 | P-1 | IndentPass | An inline whitespace-sensitive macro in the middle of a line |
| 2 | P-2 | IndentPass | `` `ifdef A module m (…); `else module m (…); `endif`` |
| 3 | P-3, P-18 | AlignPass | `arr_t [3:0] x;` still aligned; `a = 1;` / `bb = 2;` still aligned |
| 4 | P-5 | SyntaxPass | `f(a)(b)`, `cls #(T)::get(a, b)`, `` `M(a)(b)`` still wrap as calls |
| 5 | P-6 | IndentPass helper | `(* mark *) if (a) b = 1; else b = 2;` inside `begin…end` |
| 6 | P-7, P-12 | shared predicate | `my_t m [4];`, `int da[], q[$];` → spaced; `foo.bar[3:0]` not spaced |
| 7 | P-8, P-20 | SpacingPass | `` `MACRO 'h3`` cannot occur; `` x = `A ? 1 : 0;`` unchanged |
| 8 | P-9, P-13, P-14, P-15, P-17, P-19, P-21 | WrapPass | One commit each; `} name;`, a short commentless header, `a or b` in a property |
| 9 | P-10, P-11, P-16 | AlignPass | `memory u_mem();` not aligned; N-2 and N-17 tests |
| 10 | P-4 | SyntaxPass + WrapPass | Last, alone, after the decision above |

P-4 goes last because it moves the most existing output; landing it first would hide
the other fixes' corpus diffs. P-7 and P-12 change one predicate that SpacingPass,
AlignPass and WrapPass all read, so they need the widest corpus diff.

### Risk notes

- **P-2**: a branch that *closes* a scope (`` `ifdef A end `else end `endif``) restores
  to the entry level and then closes once per branch, which is right; add it as a test.
- **P-5**: the flag also drives `ends_argument_list` and `space_inside_paren`. Check
  that `@(posedge clk) (a)` spacing does not change under `function_call.space_inside_paren`.
- **P-11**: `virtual` also leads `virtual function`; the existing `function`/`task`
  rejection covers it, but add `virtual task t();` as a must-not-align case.
- **P-4**: `scope_depth` must match IndentPass for design units when
  `default_indent_level_inside_outmost_block = 0`; read the same option.

## Reproducing

```bash
# WinLibs mingw64/bin must be first on PATH for the .exe
build/lazyverilog-fmt.exe file.sv            # prints formatted text; exit 2 = safety check failed
```

Each repro above is a complete input when wrapped in `module m; … endmodule`. Configs:

- **none**: an empty `lazyverilog.toml`.
- **repo**: the repository's `lazyverilog.toml`.
- P-3, P-11: `[format.var_declaration] align = true`, `section1..4_min_width = 20, 20, 20, 16`.
- P-10: `[format.var_declaration] align = true` only.
- P-16: `[format.port_declaration] align = true`.
- P-18: `[format.statement] align = true`.
- P-19: `[format.function_call] arg_count = 3`.
- P-21: `[format.statement] begin_newline = true`.
- P-5 (second example): `[format.function_call] break_policy = "always"`.

## Status

| Issue | Result |
|-------|--------|
| P-1 … P-18, P-20 | Fixed, each with a round-7 regression test |
| P-4 | Fixed as planned: SyntaxPass writes `TopologyFacts::scope_depth`, `line_prefix_width()` adds it. Existing lines between `line_length - indent` and `line_length` columns now break |
| P-19 | Deferred. Counting an empty argument made it an item, and an empty item renders as a lone `,` line (`` `DFLT(, "a")`` under `break_policy = "always"`). Needs empty-item support in the list renderer first |
| P-21 | Not a bug. `docs/formatter/options.md` and `CLAUDE.md` now say so, and a test pins the behaviour |

Changes from the plan as written:

- **P-1** is fixed more generally: a verbatim region ends a line for both the width
  measurement and IndentPass's line start, not only for wrapped lists.
- **P-7**'s second example now gives `d[0][=2]` (a repetition is spaced like a select).
- **P-10**: a user type wider than the keyword column widens only the run of
  declarations it sits in, so existing `logic`/`int` columns elsewhere do not move.
- **P-16** changed one existing expectation in `tests/test_formatter.cpp` ("final ANSI
  port with line comment does not gain comma"): the last port's comment column.

Verification: `./build/lazyverilog-tests "[formatter]"` 931 assertions in 347 cases, all
passed; `ctest --test-dir build` 991 of 991 passed; the 20-file × 17-config corpus is
idempotent with the token stream intact after every change.
