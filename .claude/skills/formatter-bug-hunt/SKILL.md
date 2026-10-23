---
name: formatter-bug-hunt
description: Stress-test the lazyverilog formatter with unusual RTL styles and option combinations, find wrong output and idempotency bugs that earlier rounds missed, and report each one (short RTL repro, toml option, expected output, architecture-safe fix plan) in FORMATTER_BUG.html, published as an HTML artifact. Use for "test the formatter", "find formatter bugs", "formatter stress round".
---

# Formatter bug hunt

Goal: find formatter bugs that are hard to find by hand, and report them so a
later session can fix them without breaking the pass architecture.
Do not fix anything in this skill. Output is `FORMATTER_BUG.html` at the repo root, published as an
HTML artifact.

## 1. Build

```bash
cmake --build build --target lazyverilog-fmt -j8
```

The binary must match `HEAD`. A stale binary reports bugs that are already fixed.

Windows (MinGW build): if `lazyverilog-fmt` exits 127 in Git Bash, its runtime DLLs are not
on `PATH`. Run `ldd build/lazyverilog-fmt.exe`, then put the compiler's `bin` directory
(`dirname $(which g++)` from PowerShell) at the front of `PATH`.

## 2. Read what is already known

- `docs/formatter/options.md`: every option and its documented behavior.
- `CLAUDE.md` "Formatter" section: intended behaviors that are not bugs.
- Earlier rounds in `FORMAT_BUG_FIX*.md` / `FORMAT_BUG_FIX*.html` (IDs I-, J-, K-, ...).
  Skim the titles. Do not report a bug that an earlier round already lists,
  excludes, or pins as intended. Check the fix landed before you call it a regression.
- Pinned behavior: grep `tests/test_formatter.cpp` for the construct before reporting.

## 3. Write RTL that earlier rounds did not cover

Put files in the scratchpad, not the repo. Each file is one style or theme, 30-80 lines.
Pick themes that cross features, because single-feature cases are already covered:

- Constructs inside other constructs: a call inside a pattern inside a ternary inside a port connection;
  a `case` inside `fork` inside a `generate` loop; a class inside a package inside `` `ifdef ``.
- Comments in odd places: between a keyword and its `(`, before `;`, after `#`, inside `[ ]`,
  between `end` and `: label`, at end of file without newline, `/* */` spanning lines mid-expression.
- Preprocessor everywhere: `` `ifdef `` between `if` and `else`, inside a port list, in a case item list,
  `` `define `` with line continuations, macro calls as statements, as types, as port names.
- Long lines near the `line_length` boundary (exactly N, N+1 columns); deep nesting; empty bodies.
- Rare syntax: `let`, `checker`, `program`, `config`, `primitive`, `specify`, `clocking`, `covergroup`
  with `cross`/`binsof`, `randsequence`, `bind`, `alias`, `export "DPI-C"`, `interconnect`,
  streaming operators `{<<8{x}}`, `inside {[a:b]}`, `tagged`, `matches`, `$` in ranges, `'{default:0}`.
- Style variants of the same code: Verilog-95, compact one-liners, `begin`/`end` on own lines,
  tab-indented, CRLF, trailing whitespace, already-aligned code with odd widths.

## 4. Configs

Write 3 or more `*.toml` files in one config dir (each a full `[format]` section):

1. `default.toml`: empty `[format]` section.
2. `repo.toml`: the `[format*]` sections of the repo's `lazyverilog.toml`.
3. `alt*.toml`: aggressive values: `indent_size = 3`, `tab_align = true`, `begin_newline = true`,
   `wrap_end_else_clauses = false`, every spacing option at a non-default value, `function_call`
   `layout = "block"` + `break_policy = "always"`, small `line_length`, `align = true` with
   `align_adaptive = false`, `default_indent_level_inside_outmost_block = 0`.

## 5. Run

```bash
bash .claude/skills/formatter-bug-hunt/run.sh <rtl-dir> <cfg-dir> <out-dir>
```

It prints `SAFETY` (formatter refused: exit 2 means the token stream changed) and `IDEMPOTENCY`
(second pass differs). For each one, `diff <out>/<cfg>/<f> <out>/<cfg>/<f>.2`.
Then read every pass-1 output by eye (`diff -u <f>.in <f>`) and look for wrong layout:
indent leaks after a construct, lines joined or split wrongly, spacing that depends on
neighbors, alignment columns that skip a row, comments that move or lose indent.

Also sweep random option combinations for idempotency: generate ~20 `*.toml` files with every
option at a random valid value and re-run `run.sh` with that config dir. A `.err` file holding
"value error" means the random value was out of range; drop that config.

Probing one option at a time finds more than reading big outputs. The repo has a helper:

```bash
bash .claude/skills/formatter-bug-hunt/snippet.sh '[format.port_declaration]\nalign = true' <<'EOF'
module m (input int a = DEF, output logic y);
endmodule
EOF
```

Good probes: each `align` option with `align_adaptive = false` and min widths of 0 or 1 (the
measured widths decide every column then), each option at its boundary value
(`line_length` exactly at the line width, ±1), `tab_align` with each aligner, and every
documented example in `docs/formatter/options.md` (doc and behavior must agree).

## 6. Minimize

For each finding, cut the RTL to the smallest input (aim for 3-10 lines) that still shows it.
Find the smallest config: start from an empty `[format]` and add only the option that triggers it.
Confirm the repro with the binary, and copy the real output, not a guess.

## 7. Locate the cause

Use `codegraph_explore` (or `--log <dir>` on `lazyverilog-fmt` for per-pass logs) to find which pass
makes the wrong decision. Name the function and file.

## 8. Write FORMATTER_BUG.html and publish it as an artifact

Write the report as one HTML page, `FORMATTER_BUG.html` at the repo root (no Markdown report).
Then publish that file with the Artifact tool and give the user the link. To update a report
already published in this session, publish the same file path again so the URL stays the same.

Page layout (`FORMATTER_BUG.html` from an earlier round is the template; reuse its CSS):
- Light and dark theme tokens, a monospace face for code, phone-width safe (code blocks scroll
  inside their own box).
- Header: commit tested, counts (bugs, idempotency failures, crashes, configs x files).
- "What was tested" and "Not reported" (exclusions).
- Summary table: ID (links to `#vN`), severity pill, kind, option, one-line issue.
- One section per bug with a severity stripe, then the option toml, the input, and **Output**
  next to **Expected** (red and green tinted code blocks, stacked on narrow screens), then the
  cause and the fix plan.
- Fix order, the architecture rules the fixes keep, and the verification commands.
- HTML-escape code: `<=` is `&lt;=`, `&` is `&amp;`.

Per bug:

- `## V-n — title` (next unused letter after the earlier rounds).
- **Kind**: idempotency / indent / wrap / align / spacing / comment / option.
- **Option**: the toml lines that trigger it (or "none: default config").
- **Input**, **Output** (actual, copied), **Expected**: short `systemverilog` blocks.
- **Cause**: pass + function.
- **Fix plan**: which pass changes, what predicate or fact. Respect the architecture:
  - each pass writes only its own metadata; no pass reads another pass's metadata it does not own
    (WrapPass never reads `IndentMetadata`);
  - use `TokenKind` and syntax facts from SyntaxPass, never regex or string compares;
  - passes do not read `input_trivia` (only exception: comment own-line vs trailing);
  - a needed new fact goes into SyntaxPass facts (`Topology`/`CommentFacts`), not into a later pass;
  - the renderer only turns metadata into whitespace;
  - name the test to add in `tests/test_formatter.cpp` (output + idempotency).

End with a fix order (lowest risk first) and a verification plan
(`ctest --test-dir build`, `./build/lazyverilog-tests "[formatter]"`, re-run step 5).
