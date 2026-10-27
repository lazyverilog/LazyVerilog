# Formatter invariants and option contracts

`tests/test_formatter_invariants.cpp` checks properties every formatter output
must have, over a corpus and many option combinations. Example tests pin one
output each; these catch the corner cases nobody wrote an example for.

## Corpus

- `tests/fixtures/formatter_corpus/*.sv` — small style files, one theme each
  (nesting, comments, preprocessor, Verilog-95, classes, SVA, ...).
  `r_crlf.sv` keeps CRLF line endings (`.gitattributes`).
- `tests/rtl/hpc60/` — generated blocks. Files of 64 KiB or more run only
  with the default options.
- `tests/rtl/lazyverilog_formatter_corpus/rtl/` — macro-heavy header.

Add a file to `formatter_corpus/` when a bug needs a shape the corpus lacks.

## Configs

The default options, every aligner on, and 200 seeded random mixes drawn
from the option table (`kRandomConfigs`; the invariants test case stays
near 10 s in RelWithDebInfo). The tightness check runs its tight config
alone and over 8 random mixes with every aligner forced tight. A failure
names its config as `random_config(N)`; rebuild it in a test with the same
seed. The draw uses `std::mt19937` output directly, never
`std::uniform_int_distribution`, whose results differ between standard
libraries: macOS once ran other configs than Linux and Windows.

## Invariants

| Check | Holds when |
| --- | --- |
| throws | `format_source()` does not throw (token-stream safety) |
| not-idempotent | formatting the output again changes nothing |
| align-miss | every token AlignPass gave a `target_column` renders at that column, unless it starts its line |
| align-loose | with all minimum widths 0, not adaptive and `tab_align` off (tab stops pad by design), an aligned column on consecutive code lines touches at least one member: some line has only one blank (or its own spacing) before it. `var_declaration`'s type field is one width for the whole file, so a run also counts as touched when the same field (as far from its own line's indent) is touched on another line |
| call-too-long | `break_policy = "auto"`: a call left on one line does not end past `line_length` when its `(` is inside the limit |

`align-miss` makes AlignPass keep its promises: an aligner sets a column only
where the renderer can reach it. A column a line's own text overruns (a long
initializer before `;`) gets no target on that line.

An adaptive `var_declaration` section keeps its configured width, so a
name wider than section3 + section4 overruns the preferred `,`/`;` column;
that column then moves to where the text ends instead of being promised
inside the name.

`call-too-long` mirrors WrapPass's exceptions: gate terminals, task and
function formals (they follow `[format.function_declaration]`), calls
nested in another call or a bracket are never broken as calls; with
`layout = "hanging"` a one-argument call has no break that shortens it
(the argument stays after the `(`). Alignment padding is subtracted,
because WrapPass runs before AlignPass and cannot see it, except the
padding before the line's `=` that `statement_align_padding()` already
counts.

## Option contracts

Each option owns some aspects of the output. Changing it may change those
and nothing else:

| Aspect | Meaning |
| --- | --- |
| breaks | which tokens share a line |
| blank | blank lines between lines |
| indent | leading whitespace of a line |
| space | whitespace between tokens on a line |

| Options | Own |
| --- | --- |
| `blank_lines_between_items` | blank |
| every `align`, `align_adaptive`, `*_min_width`, `*_width` | space, indent |
| `statement.align`, `statement.lhs_min_width`, `var_declaration.align`, `var_declaration.section1..3_min_width`, `tab_align`, every `[format.spacing]` option | breaks, space, indent |
| `instance.port_indent_level` | space, indent |
| `indent_size`, `default_indent_level_inside_outmost_block` | breaks, indent, space |
| `function_call.*`, `function_declaration.*`, `module.*` | breaks, indent, space |
| `statement.begin_newline`, `statement.wrap_end_else_clauses` | all four |

Spacing and alignment options own indent only through hanging continuation
lines, which sit under a column of the line above (`compute(alpha,` /
`        beta)`). Alignment options do not own breaks, except the padding
statement and declaration alignment put before `=`
(`statement_align_padding()`). Spacing options do: WrapPass runs before
SpacingPass, and `compact_width()` measures every gap with `spacing_gap()`,
the same rule SpacingPass prints with, so a line is measured as it will be
printed. Inline block comments are measured too.

A call's and a function header's `line_length` counts what follows the `)`
up to the next place WrapPass can break: the `;`, the next call's `(`, a `{`,
or a break already decided (`close_tail_width()`). Operators, `?:`, `@(...)`
and `iff (...)` are not break points, so `fn(a) ? x : y;` is measured through
its `;`. The indent comes from `line_indent()`, which predicts what
IndentPass will set (scope depth, broken list items and their `)`,
brace-less bodies); a blank line BlankLinePass keeps starts a line. A call
that is past the limit even when broken stays whole, but the alignment
padding before a statement's `=` does not count there: a broken statement
is not aligned. The invariants test checks that the predicted width of
every call left on one line equals its rendered width.

The test flips every value of every option from eight base configs (default,
all aligners on, six random mixes) and
compares each corpus file's output with the base output. Lines are matched by
their text with all whitespace removed; once those differ, the breaks
changed and nothing finer is compared.

A new option goes into the table in `knobs()` with the aspects it owns.

## CST oracle

The formatter never uses slang's parser: it must format code that does not
parse, so SyntaxPass and WrapPass decide what a token is from the tokens
around it. `tests/test_formatter_cst_oracle.cpp` keeps those heuristics
honest on code that does parse. For every corpus file slang parses without
errors, each formatter token is matched by source offset to the syntax node
that owns it, and these facts are compared with the grammar:

| Fact | Grammar |
| --- | --- |
| `opens_design_unit` | keyword of a module/interface/program/package header, class, checker, primitive or config |
| `is_prototype` | `function`/`task` of a body-less prototype or DPI export; `class` of `typedef class` |
| `starts_port_list` | `(` of a unit header's ANSI or non-ANSI port list (not a modport's) |
| `is_case_item_colon` | `:` of a case, pattern-case or randcase item |
| `is_replication_brace` | inner `{` of `{n{...}}` and `'{n{...}}` |
| enum body (`WrapListKind::EnumBody`) | `{` of an enum type |
| `is_block_name_colon` | `:` of a block name |
| `starts_argument_list` | every `(` of an argument list is one (one way) |
| `starts_parameter_list` | every `(` of a parameter port list or value assignment is one (one way) |

"One way" facts are wider than the grammar node on purpose:
`starts_argument_list` is any call-shaped `(`, instance ports and gate
terminals included, and `starts_parameter_list` also opens a `#(` delay.
Only a `(` the grammar calls an argument or parameter list and the
formatter missed is a mismatch.

Tokens with no syntax node of their own -- inactive `ifdef` branches, macro
expansions, included files -- are skipped. The hpc60 blocks `include a
header the test does not resolve, so only the style files are checked.

A new topology fact gets a row here and in `facts()`.

## Broken code

The formatter must not let one syntax error spread past its own lines.
SyntaxPass drops a `(` or `[` still open at a keyword that can never stand
inside one (`endmodule`, `always_ff`, `assign`, `begin`, ...) or at a `;`
outside a `for` header, a macro argument and an assertion's property; a
unit-level keyword drops unclosed braces too. IndentPass closes scopes by
kind: `endfunction` also closes a `begin` left open inside the function, and
an extra `end` with no `begin` to close dedents nothing. Guard:
"broken code damages only its own lines" in `tests/test_formatter_regressions.cpp`.

Merge conflict markers (`<<<<<<<`, `|||||||`, `=======`, `>>>>>>>` at
column 0, then a space or the end of the line) are lexed as one token per
line and take the conditional-directive kinds, as in clang-format: the
marker lines stay verbatim at column 0 and each side is formatted as an
`` `ifdef `` branch. Guard: "merge conflict markers stay intact".
