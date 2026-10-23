# lazyverilog Roadmap: v2.2.0 – v2.4.0 + MCP

Date: 2026-10-24. Baseline: v2.1.1 (tagged 2026-10-02; git tag, `docs/releases/v2.1.1.md`,
`lua/lazyverilog/version.lua`, `vscode/src/version.ts`, `vscode/package.json` agree).

## Intent

- **Outcome:** a sequenced plan for the next three minor releases plus a parallel MCP track.
- **Audience:** internal plan (this spec, with milestones and exit gates) and a trimmed public
  page `docs/roadmap.md` (themes only, no dates, no oracle details).
- **Required by the user:** stress testing of all LSP features; `lazyverilog-clk`, which
  generates `sdc.tcl` automatically; new ideas.
- **New ideas selected:** clock-domain hover (hover only, no inlay hints), semantic tokens,
  type hierarchy, `documentHighlight` + `selectionRange`, lint autofix, MCP server in a
  separate repo owned by the `lazyverilog` org.
- **Explicitly out:** CDC lint, RDC, clock tree view, call hierarchy, port-change propagation,
  `lazyverilog-fsm`, S2 incremental index publishing, latency/scale oracles in the stress
  harness (existing `[scaling]` guards stay as they are).

Bug-fix patch releases (`v2.x.y`) continue between milestones as today.

## Milestones

| Release | Theme | Contents |
|---|---|---|
| v2.2.0 | Hardening | LSP session driver + fuzzer for every request type under ASan/UBSan; correctness invariant oracles; `documentHighlight`, `selectionRange` |
| v2.3.0 | Editor depth | Semantic tokens, type hierarchy (classes incl. UVM), lint autofix quick-fixes |
| v2.4.0 | Clocks | `lazyverilog-clk` CLI writing `sdc.tcl` (SDC 2.1 subset); clock-domain hover |
| parallel | MCP | `lazyverilog/lazyverilog-mcp`, starts after v2.4.0 so the clk tool exists at launch |

Ordering rationale: the harness ships first so every later feature is fuzzed and
invariant-checked from its first commit. The cheap LSP gaps ride with v2.2.0 so the new
harness has fresh code to check immediately. Editor depth precedes clocks (user decision).

## v2.2.0 — Hardening

### Components

- **Session driver** (C++, in `tests/`): spawns or embeds the server, speaks JSON-RPC, keeps a
  model of open documents, and issues requests. A session is a list of steps (open, change,
  request, cancel, close) serializable to a session file.
- **Fuzzer:** seeded generator of sessions over a corpus. It mixes `didOpen`, full and
  incremental `didChange` (random and mutated SV, including invalid text), `$/cancelRequest`,
  and every advertised request type at random positions.
- **Crash/hang oracle:** process stays alive, no sanitizer report, every request gets a
  response or a cancel acknowledgement, no reply takes longer than 10 s (watchdog).
- **Invariant oracles:**
  - rename then references: the renamed set equals the references set;
  - definition back-link: definition of any reference result resolves back to the same symbol;
  - format: idempotent, and the non-whitespace token stream is unchanged;
  - folding: ranges are in bounds and properly nested;
  - incremental edits: state after a sequence of incremental `didChange` equals a fresh open
    of the final text (compare diagnostics, symbols, folding).
- **Replay:** a failing run writes its session file; one command replays it. Each fixed
  failure becomes a tagged regression test.
- **New features:** `documentHighlight` (same-file references of the symbol under cursor) and
  `selectionRange`. `selectionRange` follows the folding precedent: live tree or token scan,
  no new index fields, no AST pass on the request path when the tree is not ready.

### Exit gate

- 1 h or longer seeded fuzz run on the `rtl1k` corpus under ASan/UBSan: 0 crashes,
  0 sanitizer reports, 0 unanswered requests.
- Every invariant runs over every file in `tests/rtl`. Each violation has a fix commit or a
  tracked exclusion.
- `documentHighlight` and `selectionRange` have their own tests and are in the fuzzer's
  request mix from day one.
- CI: nightly fuzz job on Linux; per-commit 60 s smoke slice.

## v2.3.0 — Editor depth

- **Semantic tokens:** `full` and `range` over the live open-buffer tree. Token types:
  module, parameter, port, net, variable, type, macro, class. Latency guarded by a
  `[scaling]` ratio test, never an absolute ms budget.
- **Type hierarchy:** supertypes and subtypes for classes, across files, via index shards
  only (no retained ASTs). `SyntaxIndex` already stores `base_class`
  (`src/syntax_index.hpp`), so no new field, codec change or `kFormatVersion` bump is
  needed. Subtypes need a reverse map (base class to subclasses) built alongside the
  published index snapshot, not on the request path.
- **Lint autofix:** every naming/style rule with an unambiguous fix offers a quick-fix code
  action. Applying it clears the diagnostic and leaves the file format-stable.
  `docs/comparison.md` lint-autofix cell changes from ❌ to ✅.

### Exit gate

- All three features are in the fuzzer and pass the v2.2.0 invariants, plus: applying any
  autofix never introduces a parse error.

## v2.4.0 — Clocks

### CLI and config

- `lazyverilog-clk --top <module> -o sdc.tcl`. Reuses `tools/cli_project.cpp` like
  `lazyverilog-lint` and `lazyverilog-rtltree`.
- Output: generic SDC 2.1 subset only — `create_clock`, `create_generated_clock`,
  `set_clock_groups`, `get_ports`, `get_pins`. Pins use RTL hierarchical names; the docs
  state that post-synthesis names may differ (`q` vs `q_reg`).
- Periods: structure only. Every `create_clock` gets `-period 1.000` and a `;# TODO period`
  comment. The tool never reads periods.
- Config (`[clk]` in `lazyverilog.toml`), only two keys:
  - `icg_cells`: glob list of ICG module names, default `["*icg*", "*clock_gate*"]`;
  - `related`: list of root-name groups to merge into one synchronous group.

### Clock graph

- Built from the slang elaborated design in the compile path, one graph per
  `CompilationGroup`, published like the compilation snapshot. Hover reads the published
  graph only; no AST walk on the request path.
- Hover on a register or port shows its clock, e.g.
  `clock: u_div/clk_o (generated ÷2 from clk_i)`.

### Detection rules

Pipeline: elaborate from `--top`, collect every clock pin, walk backward from each to a
root, classify every node on the path. Matching is structural on elaborated expressions,
never by signal name (ICG module names are the one name-based rule).

- **Clock pin:** the event signal of `always_ff @(posedge|negedge X)` or
  `always @(posedge|negedge X)`. With 2 or more event signals, the one the body never tests
  in an `if` condition is the clock; the others are async resets.
- **Root:** a top-level input port that reaches a clock pin. Emits
  `create_clock -name <port> -period 1.000 [get_ports <port>]  ;# TODO period`.
- **Divider:** a flop clocked by C whose D is `~Q` (÷2), or bit `cnt[k]` of a counter
  `cnt <= cnt + 1` (÷2^(k+1)). Emits `create_generated_clock -divide_by N -source <C pin>`
  on the Q pin.
- **Mux:** `?:`, `case` or `if` on a select where 2 or more inputs trace to clocks. Emits one
  generated clock per input with `-master_clock` and `-add`; the mux outputs form a
  `set_clock_groups -physically_exclusive` group.
- **ICG:** an instance of a module matching `[clk].icg_cells`, or the RTL pattern "latch
  `en` on `~clk`, then `clk & en_l`". Pass-through: same clock, no generated clock.
- **Inverter / buffer:** `~clk` is a generated clock with `-invert`; a plain assign is a
  pass-through.
- **Black box:** a path ending at an unelaborated instance output (PLL, hard macro) emits
  `# TODO: clock source <pin> unresolved` plus a placeholder `create_clock` on that pin.
  Never a silent omission.
- **Unrecognized combinational clock logic:** `# WARN` comment naming the pin; no constraint.
- **Async groups:** each root plus everything generated from it is one group. All groups
  are `set_clock_groups -asynchronous`, except roots merged by `[clk].related`.
- **Not detected, by design:** glitch analysis, CDC false paths.
- **Output order:** roots, then generated clocks in topological order, then groups; within
  each part sorted by hierarchical name, so goldens are stable.

### Exit gate

- Golden `sdc.tcl` tests on fixtures: divider (÷2 and counter), 2:1 mux, ICG (cell and RTL
  pattern), two async roots, a clock passed through hierarchy, a black-box PLL.
- Output parses in OpenSTA `read_sdc`. OpenSTA is not in CI today
  (`.github/workflows/ci.yml`), so this is a manual release step until a CI job adds it.
- `lazyverilog-clk` and clock hover are in the fuzzer.

### Risk

- Unverified: how much of a flop's D-input dataflow slang exposes after elaboration. If the
  divider and mux matches need a dataflow pass slang does not provide, v2.4.0 grows by that
  pass. Check this first in the v2.4.0 plan.

## MCP track — `lazyverilog/lazyverilog-mcp`

- Separate repo in the `lazyverilog` GitHub org; starts after v2.4.0 ships.
- Drives the `lazyverilog-lsp` binary as an LSP client over stdio.
- Read-only tools: definition, references, hierarchy, clk (wrapping `lazyverilog-clk`).
- Gets its own spec when it starts.

## Public page — `docs/roadmap.md`

Added to the VitePress site with this content (no dates, no internal gates):

- **Hardening:** fuzzing and correctness checks for every LSP feature; document highlight
  and smart selection.
- **Editor depth:** semantic highlighting, class type hierarchy, lint quick-fixes.
- **Clocks:** `lazyverilog-clk` generates an SDC clock skeleton from RTL; hover shows a
  register's clock domain.
- **AI agents:** an MCP server exposing lazyverilog navigation and clock data.

## Implementation planning

Each milestone gets its own implementation plan when it starts (v2.2.0 first). The public
`docs/roadmap.md` page is written as part of the v2.2.0 plan.
