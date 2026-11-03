# Lint Autofix PR 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Diagnostics carry fixes; width/type conversion warnings and the three existing lint quick fixes get them; the server offers them as quick fixes and `lazyverilog-lint` applies them in `--fix`, `--fix-unsafe` and `--fix-interactive` modes.

**Architecture:** `FixInfo` lives on `ParseDiagInfo`. slang fixes are computed while the `slang::Diagnostic` is alive (`convert_diagnostic()` and `collect_parse_diagnostics()`), lint fixes inside `run_lint()`. One pure function applies fixes to text; the CLI and the tests use it, the server turns fixes into `WorkspaceEdit`s.

**Tech Stack:** C++20, slang v12.1, lspcpp, Catch2 (`build/lazyverilog-tests`), CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-11-03-lint-autofix-design.md` (this plan covers its "PR 1" catalog, the model, both consumers, and the docs).

## Global Constraints

- Positions are 0-based lines and LSP columns; convert with `lsp_column(sm, loc)` (src/lsp_position.hpp) when producing and `lsp_position_to_byte_offset(text, line, col)` (src/string_utils.hpp) when applying. Never byte columns.
- No fix inside a macro expansion or macro argument; every edit must be `sm.isFileLoc` in the diagnostic's own buffer.
- Output/inout arguments and output/inout port connections get no cast fix (spec, "Call arguments and port connections").
- Fixes must keep behavior when `safe`. Each slang fix test recompiles the fixed text and asserts the code is gone and no new diagnostic appears.
- Windows CI has no POSIX libc: guard POSIX-only calls with `#ifdef _WIN32`, tests included. No `thread_local` with a non-trivial destructor.
- New `.cpp` files go into every CMake source list that already lists their neighbours (`LAZYVERILOG_SOURCES`, the test executable's list, `LAZYVERILOG_CLI_PROJECT_SOURCES`).
- Commit messages and docs in plain English; end each commit with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01AQmTw2tM27yHXkLBBFnH2J`.
- Local build (MinGW): `PATH="/c/Users/g1004/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH" cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j8`, run from the worktree root `C:/Users/g1004/dev/lazyverilog/.claude/worktrees/lint-autofix`. The first configure fetches deps; it is slow.

## Review Focus

1. A width warning on a signed target (`logic signed [7:0] a = b16;`): the fix must not introduce a sign-conversion warning. Test in Task 2.
2. Two warnings on one expression (width + sign): both fixes compose to `signed'(8'(b))` (or the other order) and the result compiles clean. Test in Tasks 1 and 2.
3. A file with CRLF line endings and a multibyte character before the fixed expression: the CLI writes correct bytes and keeps CRLF. Test in Tasks 1 and 5.
4. Code that changed after the semantic compile (stale semantic diagnostics): the server offers no fix from them. Test in Task 4.
5. A `--fix` run over a filelist where a header shared by two files has the same warning reported twice: the fix is applied once, not twice. Test in Task 5 (dedupe identical fixes before applying).

---

### Task 1: Fix model and the apply function

**Files:**
- Modify: `src/document_state.hpp` (add `FixEdit`, `FixInfo`, `ParseDiagInfo::fixes`)
- Create: `src/fix_apply.hpp`, `src/fix_apply.cpp`
- Modify: `CMakeLists.txt` (add `src/fix_apply.cpp` to all three lists, `tests/test_fix_apply.cpp` to the test list)
- Test: `tests/test_fix_apply.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct FixEdit { int start_line{0}, start_col{0}, end_line{0}, end_col{0}; std::string new_text; };
  struct FixInfo { std::string title; bool safe{true}; std::vector<FixEdit> edits; };
  // ParseDiagInfo: std::vector<FixInfo> fixes;
  struct FixApplyResult { std::string text; size_t applied{0}; size_t skipped{0}; };
  FixApplyResult apply_fixes(std::string_view text, const std::vector<const FixInfo*>& fixes);
  ```
  `apply_fixes` takes fixes in priority order. A fix is accepted whole or skipped whole. It is skipped when any of its edits overlaps a non-empty range (replacement or deletion) of an already accepted fix, or when its non-empty range strictly contains / overlaps an accepted edit's range. Pure insertions at the same offset never conflict; at one offset they are emitted in acceptance order for start-insertions, and in reverse acceptance order for insertions that sit at the end of an accepted wrap — simplest correct rule: sort accepted edits by (offset, then for equal offsets: an edit whose fix *opens* there before one that *closes* there; among openers, later-accepted first; among closers, earlier-accepted first), so later fixes wrap outside earlier ones. Identical fixes (same edits) count once (`applied` increments once, the duplicate is neither applied nor skipped).

- [ ] **Step 1: Write the failing tests** (`[fix-apply]`)
  - `apply_fixes: a wrap is two insertions` — text `assign a = b;`, fix edits insert `8'(` at col 11 and `)` at col 12 → `assign a = 8'(b);`, applied 1.
  - `apply_fixes: two wraps on one expression nest` — fixes A (`8'(`,`)` around `b`) then B (`signed'(`,`)` around `b`) → `assign a = signed'(8'(b));`.
  - `apply_fixes: an overlapping replacement is skipped` — two replacements over the same range → first applied, skipped 1.
  - `apply_fixes: CRLF and a multibyte column` — text `"// é\r\nassign a = b;\r\n"`, wrap at line 1 → bytes `"// é\r\nassign a = 8'(b);\r\n"`; and a line `"/* é */ x = b;"` with `b` at UTF-16 col 12 wraps the `b`.
  - `apply_fixes: an identical fix is applied once`.

- [ ] **Step 2: Build and run** `build/lazyverilog-tests.exe "[fix-apply]"` — expect compile failure (missing header).
- [ ] **Step 3: Implement** the model and `apply_fixes` (convert with `lsp_position_to_byte_offset`; apply accepted edits from the highest offset down).
- [ ] **Step 4: Run** `build/lazyverilog-tests.exe "[fix-apply]"` — all pass.
- [ ] **Step 5: Commit** `feat(fix): a fix model on diagnostics and a function that applies fixes`.

### Task 2: Conversion fixes from slang diagnostics

**Files:**
- Create: `src/features/fix_slang.hpp`, `src/features/fix_slang.cpp`
- Modify: `src/background_compiler.cpp` (`convert_diagnostic`: set `info.fixes = slang_fixes(sm, diagnostic)` after the location is known), `src/analyzer.cpp` (`collect_parse_diagnostics`: same), `CMakeLists.txt`
- Test: `tests/test_lint_fix.cpp` (`[lint-fix]`)

**Interfaces:**
- Consumes: `FixInfo`, `FixEdit`, `apply_fixes` (Task 1).
- Produces: `std::vector<FixInfo> slang_fixes(const slang::SourceManager& sm, const slang::Diagnostic& diag);` — empty when no fix applies. Text needed for checks comes from `sm.getSourceText(buffer)`.
  Test helper (in the test file, reused by Tasks 3–4 if useful): `std::vector<ParseDiagInfo> compile_and_collect(std::string_view source)` that builds a `slang::ast::Compilation` from one `SyntaxTree::fromText`, converts every diagnostic of `getAllDiagnostics()` with code (`slang_diagnostic_code`), position (`lsp_column`) and `slang_fixes`; and `std::string fix_all(std::string_view source, std::string_view code)` = apply every fix of every diagnostic with that code.

Catalog (spec "PR 1"); diagnostic payloads from `ConversionExpression::checkImplicitConversions` (slang `source/ast/expressions/ConversionExpression.cpp:601`): range 0 is the operand; `WidthTruncate/WidthExpand/PortWidthTruncate/PortWidthExpand << actualWidth << targetWidth`; `ImplicitConvert/PackedArrayConv/SignConversion/FloatIntConv/IntFloatConv/FloatNarrow/FloatWiden << sourceType << targetType`; `IntBoolConv/FloatBoolConv << type` (source/ast/ASTContext.cpp:227-246); `ArithOpMismatch/BitwiseOpMismatch/ComparisonMismatch/SignCompare` are raised in BinaryExpression with both operand ranges — read their emission site before writing that branch.

| Code | Edit | Title |
|---|---|---|
| width-trunc/expand, port-width-* | wrap operand `N'(`…`)`; for a signed target type use `signed'(N'(`…`))` only if slang would otherwise report sign-conversion — prove with the recompile test | `Cast to N bits` |
| arith-op-mismatch, bitwise-op-mismatch, comparison-mismatch | wrap the narrower operand in the wider width | `Cast <operand text> to N bits` |
| sign-conversion | `signed'(`/`unsigned'(` per target | `Cast to signed` / `Cast to unsigned` |
| sign-compare | `unsigned'(` on the signed operand | `Cast to unsigned` |
| implicit-conv, packed-array-conv | `T'(` when the target type has a printable name usable at that point (typedef alias name as written in scope, or a built-in keyword type); otherwise none | `Cast to T` |
| float-int-conv | `T'(` named, else `N'(` (signed target: `signed'(N'(`) | `Convert to <T or N bits>` |
| int-float-conv, float-widen | `real'(` (`shortreal'(` for shortreal) | `Convert to real` |
| float-narrow | `shortreal'(` | `Convert to shortreal` |
| int-bool-conv | `(` … `) != '0` | `Compare with '0` |
| float-bool-conv | `(` … `) != 0.0` | `Compare with 0.0` |

All `safe`. Output/inout guard: when the diagnostic's operand is an lvalue placeholder (spec), return none. Practical detection from the Diagnostic alone: if `diag.symbol` is set, visit it with an `ASTVisitor` looking for an `AssignmentExpression` whose `right()` is `ExpressionKind::EmptyArgument` and whose `left().sourceRange` equals the diagnostic range; for port connections, check `PortConnection` with an output/inout port whose expression range equals it. If the symbol is not available, fall back to syntax: the range sits inside an `ArgumentList`/port connection and the callee's formal is not resolvable → no fix. Pick whichever the slang API allows; the tests decide.

- [ ] **Step 1: Write the failing tests** — one per row, shape:
  ```cpp
  TEST_CASE("lint-fix: width-trunc casts to the target width", "[lint-fix]") {
      const char* src = "module m(input logic [15:0] b, output logic [7:0] a);\n"
                        "  assign a = b;\nendmodule\n";
      auto fixed = fix_all(src, "width-trunc");
      CHECK(fixed.find("assign a = 8'(b);") != std::string::npos);
      CHECK(codes_of(compile_and_collect(fixed)).count("width-trunc") == 0);
      CHECK(new_codes(src, fixed).empty());   // no diagnostic code that src lacked
  }
  ```
  Plus: width-expand; signed target (Review Focus 1); width+sign on one operand compose (Review Focus 2); input argument `f(b)` → `f(8'(b))`; named argument `f(.x(b))`; task call; input port `.d(b)`; output argument and output port → `fixes.empty()`; macro body and macro argument → empty; implicit-conv enum→enum with typedef; implicit-conv with an anonymous target type → empty; each remaining row.
- [ ] **Step 2: Run** `build/lazyverilog-tests.exe "[lint-fix]"` — fail.
- [ ] **Step 3: Implement** `slang_fixes` and wire both call sites.
- [ ] **Step 4: Run** `"[lint-fix]"` then the full suite `build/lazyverilog-tests.exe` — pass.
- [ ] **Step 5: Commit** `feat(fix): cast fixes for width, sign and type conversion warnings`.

### Task 3: Lint fixes move into run_lint

**Files:**
- Modify: `src/features/lint.cpp` (case-missing-default rule, function/task lifetime rules)
- Test: `tests/test_lint_fix.cpp`

**Interfaces:**
- Consumes: `FixInfo` (Task 1).
- Produces: `run_lint` diagnostics with codes `lint-statement-case-missing-default` (fix `Add default case`, safe), `lint-function-automatic`, `lint-function-explicit-lifetime` (fix `Add 'automatic' lifetime`, unsafe), `lint-task-explicit-lifetime` (fix `Add 'automatic' lifetime to task`, unsafe) carrying the edits that `code_action.cpp:376-460` builds today (keep its behavior: replace an existing `static`; `endcase` sharing a line gets `default: ; ` before it, else an indented `default: ;` line).

- [ ] **Step 1: Write the failing tests** — for each of the four codes, `run_lint` on a small module returns the diagnostic with one fix whose application gives the same text today's code action produces (copy the expected strings from the existing code-action tests in `tests/test_lsp_features.cpp` around lines 1380-1860).
- [ ] **Step 2: Run** — fail.
- [ ] **Step 3: Implement** in the rules.
- [ ] **Step 4: Run** `"[lint-fix]"` and `"[lint]"` — pass.
- [ ] **Step 5: Commit** `feat(fix): lint rules attach their quick fixes`.

### Task 4: Code actions come from diagnostic fixes

**Files:**
- Modify: `src/features/code_action.cpp` (replace the loop at ~391-463 and `QuickFixLocator` uses that only it needed), and whatever gives it the semantic diagnostics (`provide_code_actions` takes `Analyzer&`; use `analyzer.semantic_diagnostics(uri)`, which already returns only diagnostics of the current open-buffer version — verify, and if not, compare versions)
- Test: `tests/test_lsp_features.cpp`

**Interfaces:**
- Consumes: `ParseDiagInfo::fixes` from Tasks 2–3; `run_lint(state, config.lint, ...)`.
- Produces: for each request diagnostic, the current diagnostic with the same `range.start` and the same code (`diag.code` string from the request if present, else message containing the code in brackets — check how `server.cpp` publishes `code`) → one `quickfix` `CodeAction` per fix, edit = `make_range_edit` per `FixEdit` (one `WorkspaceEdit` with all of a fix's edits), safe fixes before unsafe, unsafe titles suffixed ` (may change behavior)`.

- [ ] **Step 1: Write the failing tests** — width-trunc semantic diagnostic on an open buffer → action `Cast to 8 bits` with the right edit; edit the buffer after the compile → no action from the stale diagnostic (Review Focus 4); lifetime fix title has the suffix and sorts after a safe fix on the same diagnostic set. Existing quick-fix tests stay green unchanged.
- [ ] **Step 2: Run** — fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** full suite — pass.
- [ ] **Step 5: Commit** `feat(code-action): offer the fixes diagnostics carry`.

### Task 5: lazyverilog-lint fix modes, smoke tests, docs

**Files:**
- Modify: `tools/lint_file.cpp`, `tests/lint_cli_smoke.cpp`, `docs/linter/cli.md`, `docs/linter/options.md` (~line 720 quick-fix table), `docs/comparison.md:24`, `README.md` (autofix row)

**Interfaces:**
- Consumes: `apply_fixes`, `ParseDiagInfo::fixes`.
- Produces (CLI contract, spec "lazyverilog-lint"):
  - `--fix`, `--fix-unsafe`, `--fix-interactive`; any two together → stderr `--fix, --fix-unsafe and --fix-interactive cannot be used together`, exit 1. Add them to `print_usage` and `print_help`.
  - Per file (group `flat` by `file`), read the file again from disk, apply, write back in binary mode only if changed. Only files in `states` are written. Identical fixes from duplicated diagnostics count once (Review Focus 5).
  - Interactive: per fix print `file:line:col: severity: message [code]`, then `  fix: <title>` (+ ` [unsafe]`), then for each affected line `-<old line>` and `+<new line>` (new line = old line with just that fix applied), then `Apply this fix [y,n,a,q]? ` on stdout, flush, read a line from `std::cin`. EOF → `q`.
  - Diagnostics whose fix was applied are not printed. stderr summary `fixed N issue(s) in M file(s)`; plus `skipped K overlapping fix(es); run again` when K > 0. Exit status from printed diagnostics only.

- [ ] **Step 1: Write the failing smoke tests** in `tests/lint_cli_smoke.cpp` following its `run_command` pattern, on temp copies:
  - `--fix` turns `assign a = b;` (16→8) into `assign a = 8'(b);`, and a rerun without flags no longer prints `[width-trunc]`.
  - `--fix` leaves a missing-`automatic` function alone; `--fix-unsafe` adds `automatic`.
  - `--fix-interactive` with stdin `y\nn\n` on a file with two width warnings applies only the first; stdout contains `Apply this fix [y,n,a,q]?` and a `-` and `+` line.
  - CRLF file stays CRLF after `--fix` (Review Focus 3).
  - `--fix --fix-unsafe` exits 1 with the message.
  (`run_command` needs stdin input: add a parameter that writes it to a temp file and redirects `< file`.)
- [ ] **Step 2: Run** `ctest --test-dir build -R lint-cli-smoke --output-on-failure` — fail.
- [ ] **Step 3: Implement** the CLI.
- [ ] **Step 4: Run** the smoke test and the full suite — pass.
- [ ] **Step 5: Docs**: cli.md section "Fixing" (three modes, grades, output, exit status, examples); options.md quick-fix table lists every PR-1 fix with its grade; comparison.md autofix row ✅ (partial wording if the table has one, e.g. "✅ (`--fix`)"); README row matches.
- [ ] **Step 6: Commit** `feat(lint-cli): --fix, --fix-unsafe and --fix-interactive`.
