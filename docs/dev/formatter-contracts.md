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

The default options, every aligner on, and 24 seeded random mixes drawn
from the option table. A failure names its config as `random_config(N)`;
rebuild it in a test with the same seed.

## Invariants

| Check | Holds when |
| --- | --- |
| throws | `format_source()` does not throw (token-stream safety) |
| not-idempotent | formatting the output again changes nothing |
| align-miss | every token AlignPass gave a `target_column` renders at that column, unless it starts its line |
| align-loose | with all minimum widths 0 and not adaptive, an aligned column on consecutive code lines touches at least one member: some line has only one blank (or its own spacing) before it |
| call-too-long | `break_policy = "auto"`: a call left on one line does not end past `line_length` when its `(` is inside the limit |

`align-miss` makes AlignPass keep its promises: an aligner sets a column only
where the renderer can reach it. A column a line's own text overruns (a long
initializer before `;`) gets no target on that line.

`call-too-long` mirrors WrapPass's exceptions: gate terminals, task and
function formals (they follow `[format.function_declaration]`), and calls
nested in another call or a bracket are never broken as calls. Alignment
padding is subtracted, because WrapPass runs before AlignPass and cannot
see it.

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
| every `align`, `align_adaptive`, `*_min_width`, `*_width`; `tab_align`; every `[format.spacing]` option | space, indent |
| `instance.port_indent_level` | space, indent |
| `indent_size`, `default_indent_level_inside_outmost_block` | breaks, indent, space |
| `function_call.*`, `function_declaration.*`, `module.*` | breaks, indent, space |
| `statement.begin_newline`, `statement.wrap_end_else_clauses` | all four |

Spacing and alignment options own indent only through hanging continuation
lines, which sit under a column of the line above (`compute(alpha,` /
`        beta)`). They never own breaks: WrapPass measures with
`compact_width()`, which does not depend on spacing, so a spacing option
cannot move a line break. Call spacing (`function_call.space_*`) is the
exception and owns breaks, because it widens the call WrapPass measures.

The test flips every value of every option from eight base configs (default,
all aligners on, six random mixes) and
compares each corpus file's output with the base output. Lines are matched by
their text with all whitespace removed; once those differ, the breaks
changed and nothing finer is compared.

A new option goes into the table in `knobs()` with the aspects it owns.
