# Lint autofix

Date: 2026-11-03. Branch: `feat/lint-autofix` (from `e72f8a6`).

## Goal

Diagnostics that have a mechanical repair carry that repair. The same repair is
offered by the language server as a quick fix and applied by `lazyverilog-lint`
on request. Width and type mismatches (slang's conversion warnings) are the
required core; the catalog below extends the same machinery to most slang
warnings and lazyverilog lint rules that have an unambiguous fix.

## Decisions (from the interview)

- Width mismatch is fixed with a size cast to the target width: `a = 8'(b);`
  for both truncation and expansion.
- Type mismatch covers enum/struct implicit conversion, signed/unsigned,
  float/int, and int-to-bool conversion.
- `lazyverilog-lint` gets three modes: no fixing (default, unchanged),
  `--fix` (apply automatically), `--fix-interactive` (ask per fix, `git add -p`
  style). `--fix-unsafe` widens `--fix` to unsafe fixes.
- One shared fix model: the server's code actions and the CLI use the same
  fixes. The three existing quick fixes move onto it.
- Each fix is graded `safe` (keeps behavior, states intent) or `unsafe` (may
  change behavior). `--fix` applies safe fixes only; `--fix-unsafe` applies all;
  interactive mode offers all and tags unsafe ones `[unsafe]`; the server offers
  all and marks safe ones `isPreferred`.
- Delivered as four PRs, in order, each reviewed before merge.

## Model

In `src/document_state.hpp`:

```cpp
struct FixEdit {
    int start_line{0}, start_col{0};   // 0-based, LSP UTF-16 columns
    int end_line{0}, end_col{0};       // end exclusive; start == end inserts
    std::string new_text;
};
struct FixInfo {
    std::string title;                 // "Cast to 8 bits", shown in UI and CLI
    bool safe{true};
    std::vector<FixEdit> edits;        // all in the diagnostic's own file
};
// ParseDiagInfo gains:
    std::vector<FixInfo> fixes;        // usually empty; at most a few
```

All edits of a fix live in the diagnostic's file (`ParseDiagInfo::uri`, or the
owning document when empty). A fix that would need another file is not offered.
`ParseDiagInfo` is not part of the shard cache, so no `kFormatVersion` bump.

Wrapping fixes are pairs of insertions (`8'(` at the start, `)` at the end), so
two fixes on the same expression compose (`signed'(8'(b))`) instead of
conflicting. Only replacements and deletions can overlap.

## Producers

### slang diagnostics

`convert_diagnostic()` in `src/background_compiler.cpp` (semantic) and
`collect_parse_diagnostics()` in `src/analyzer.cpp` (parse) already hold the live
`slang::Diagnostic`. A new `src/features/fix_slang.{hpp,cpp}` exposes

```cpp
std::vector<FixInfo> slang_fixes(const slang::SourceManager&, const slang::Diagnostic&,
                                 std::string_view file_text);
```

called from both. It reads the diagnostic's ranges and arguments (for example
`WidthTruncate << actualWidth << targetWidth`, `ImplicitConvert << sourceType
<< targetType`, with `op.sourceRange` as the range) and, where the catalog
needs syntax, the syntax tree reachable from the diagnostic's location.

Guards (no fix when any fails):
- every edited location is a file location (`sm.isFileLoc`) in the diagnostic's
  own buffer; nothing inside a macro expansion or a macro argument;
- the range is valid and non-empty where the fix needs one;
- the text at the range still matches what the fix assumes (for example the
  literal being rewritten).

### lint rules

`run_lint()` rules attach fixes where they emit a diagnostic, using the live
syntax tree they already walk (`src/features/lint.cpp`).

## Consumers

### Language server

`src/features/code_action.cpp`: for each diagnostic in the request, find the
current document's diagnostic (lint from `run_lint`, parse, and semantic from
`Analyzer::semantic_diagnostics`) with the same start position and code, and
turn each of its fixes into a `quickfix` code action (`isPreferred` when safe).
Semantic diagnostics are used only when their version matches the open
buffer's, so a stale fix is never offered. The three message-matching branches
are removed.

### lazyverilog-lint

New options (`tools/lint_file.cpp`):

- `--fix`: apply every safe fix of every reported diagnostic.
- `--fix-unsafe`: like `--fix`, including unsafe fixes.
- `--fix-interactive`: for each fix, print the diagnostic line, then the
  affected lines as `-` / `+` lines, then `Apply this fix [y,n,a,q]? ` (`a` =
  this and all later fixes, `q` = stop asking, apply what was accepted).
  Answers are read from stdin line by line; EOF means `q`. Unsafe fixes are
  shown with `[unsafe]` after the title.

The three are mutually exclusive. Suppressed diagnostics (`--nowarn`) are not
fixed. Only files that the run linted are written.

Applying: per file, convert each edit's (line, UTF-16 col) to a byte offset in
the text the run read, sort edits by position (stable, so insertions at one
point keep diagnostic order), and drop any edit that overlaps an already
accepted replacement or deletion, together with the rest of its fix. Write the
file once. Line endings and encoding are untouched because only edit ranges
change.

Output: diagnostics whose fix was applied are not printed; the rest print as
today. A final line on stderr says `fixed N issue(s) in M file(s)` and, if any,
`skipped K overlapping fix(es); run again`. The exit status is computed from the
printed (unfixed) diagnostics, so a run whose only error was fixed exits 0.
The run does not re-lint after writing.

## Catalog

`safe` keeps behavior. Each fix's test recompiles the fixed text and checks the
diagnostic is gone and no new diagnostic appears.

### PR 1: model, CLI modes, server wiring, conversions

| Code | Fix | Grade |
|---|---|---|
| width-trunc, width-expand, port-width-trunc, port-width-expand | `N'(e)` with the target width | safe |
| arith-op-mismatch, bitwise-op-mismatch, comparison-mismatch | `N'(e)` on the narrower operand, to the wider width | safe |
| sign-conversion | `signed'(e)` or `unsigned'(e)` to the target's signedness | safe |
| sign-compare | `unsigned'(e)` on the signed operand (the comparison is unsigned already) | safe |
| implicit-conv, packed-array-conv | `T'(e)` when the target type is named (typedef, or a built-in type keyword); no fix otherwise | safe |
| float-int-conv | `T'(e)` when the target is named, else `N'(e)`, or `signed'(N'(e))` for a signed target (rounds like the implicit conversion) | safe |
| int-float-conv, float-widen | `real'(e)` (`shortreal'(e)` when the target is shortreal) | safe |
| float-narrow | `shortreal'(e)` | safe |
| int-bool-conv | `(e) != '0` | safe |
| float-bool-conv | `(e) != 0.0` | safe |
| lint-statement-case-missing-default | `default: ;` before `endcase` (moved) | safe |
| lint-function-automatic, lint-function-explicit-lifetime, lint-task-explicit-lifetime | add `automatic`, or replace `static` with it (moved) | unsafe |

Call arguments and port connections. slang binds an input argument of a
function, task, or system call, and an input port connection, with the same
implicit conversion as an assignment, so the rows above fire on the argument
expression and the cast goes there: `f(8'(b))`, `.d(8'(b))`. An output or
inout argument or port is bound the other way round: the converted operand is
slang's `EmptyArgumentExpression` placeholder whose range is the actual
argument, an lvalue that cannot be cast. Those diagnostics get no fix. They are
told apart by visiting `Diagnostic::symbol` for an assignment whose right side
is an `EmptyArgumentExpression` with the diagnostic's range (or an output/inout
port connection with that range). Tests cover an input argument (width and
type), a named-argument call, a task call, an input port, and an output
argument and output port that get no fix.

`N'(e)` uses the bare width; the target's signedness is handled by the
separate sign-conversion fix when slang reports one. Parentheses are added
around `e` only where the cast syntax needs none (`N'(...)` already groups).

### PR 2: precedence and simple cleanup

| Code | Fix | Grade |
|---|---|---|
| bitwise-rel-precedence, arith-in-shift, logical-not-parentheses, bitwise-op-parentheses, logical-op-parentheses, conditional-precedence, consecutive-comparison | parenthesize the subexpression the way it already parses, e.g. `a & (b == c)`, `(!a) == b` | safe |
| constant-conversion, vector-overflow | replace the literal with the value slang uses, same base, e.g. `4'h1F` -> `4'hF` | safe |
| unsized-concat | size the literal as it is used: `1` -> `32'd1` | safe |
| useless-cast | `T'(e)` -> `(e)` | safe |
| pointless-void-cast | `void'(e)` -> `e` | safe |
| unsigned-arith-shift | `>>>` -> `>>`, `<<<` -> `<<` | safe |
| ineffective-sign | remove the `signed` keyword | safe |
| empty-stmt, empty-member | remove the stray `;` | safe |
| unused-result | `f(...)` -> `void'(f(...))` | safe |
| lint-style-trailing-whitespace | delete the trailing blanks | safe |

### PR 3: declarations and statements

| Code | Fix | Grade |
|---|---|---|
| unused-import, unused-wildcard-import, dup-import | remove the import item, or the whole `import ...;` when it is the only item | safe |
| unused-variable, unused-net, unused-typedef, unused-genvar | remove the declaration when it declares exactly one name, has no initializer, and is not a port | safe |
| explicit-static | insert `static` before the declaration's type | safe |
| implicit-net-port | insert `wire` after the direction | safe |
| multibit-edge | `posedge bus` -> `posedge bus[<lsb>]` | safe |
| split-distweight-op | `: =` / `: /` -> `:=` / `:/` | safe |
| nonstandard-foreach | `foreach (a[i][j])` -> `foreach (a[i, j])` | safe |
| case-enum | insert one `NAME: ;` item per missing enum value before `default`/`endcase` | safe |
| case-not-wildcard | `case` -> `casez` | unsafe |
| unnamed-generate | add `: gen_<n>` labels (`begin : gen_N`), choosing a name unused in the scope | unsafe (hierarchical names change) |
| lint-statement-explicit-begin | wrap the single statement in `begin` / `end` | safe |
| lint-statement-raw-always | `always @(*)` / `always @*` -> `always_comb`; edge-sensitive -> `always_ff`; level-sensitive with a latch shape -> `always_latch` | unsafe |
| lint-statement-assignment-kind | `=` <-> `<=` per the block kind | unsafe |

### PR 4: connections and calls (needs the project index)

| Code | Fix | Grade |
|---|---|---|
| unconnected-port, lint-instance-missing-connection | add `.p()` for each missing port | safe |
| lint-instance-duplicate-connection, lint-instance-stale-connection | remove the connection | unsafe |
| lint-instance-style | rewrite positional connections as named when the module's ports are known | safe |
| lint-function-call-style | rewrite positional arguments as named when the callee's signature is known | safe |

### Not fixed

Naming rules (the right name cannot be derived from a pattern),
latch inference (the intended assignment is unknown), and warnings whose repair
needs intent (`casez-with-x`, `finish-num`, `index-oob`, `case-overlap`, ...).

## Testing

- `tests/test_lint_fix.cpp` (`[lint-fix]`): per catalog row, source in, fixed
  text out, then recompile the fixed text and require the code gone and no new
  diagnostic. Guard cases: macro body, macro argument, include file.
- `tests/test_code_action.cpp` (or the existing code-action tests): a quick fix
  appears for a width warning, `isPreferred` follows the grade, a stale semantic
  diagnostic yields none, the three moved fixes still appear.
- `tests/lint_cli_smoke.cpp`: `--fix` rewrites a temp file and the exit status
  drops; `--fix-interactive` with `y\nn\n` on stdin applies only the first;
  `--fix` skips unsafe and `--fix-unsafe` applies it; CRLF is preserved; the
  mutually exclusive flags are rejected.

## Docs

`docs/linter/cli.md` (modes, grades, output), `docs/linter/options.md` (the
quick-fix table, regenerated from the catalog), `docs/comparison.md:24`
(autofix row), README feature row.
