# `lazyverilog-clk`

Reports the clock domain of every port of one instance in an elaborated design.

```
lazyverilog-clk -f <filelist> <instance>
```

`<instance>` is a hierarchical path (`top.u_core.u_fifo`), or a module type name
when it resolves to exactly one instance. An ambiguous or unknown name fails
with the list of candidates.

## Example

```
$ lazyverilog-clk -f tests/rtl/clk_fixtures/files.f top.u_dut
top.u_dut (dut)
| Port     | Dir | Clock Domain               |
|----------|-----|----------------------------|
| clk_a    | in  | (edge source)              |
| clk_b    | in  | (edge source)              |
| rst_n    | in  | clk_a, rst_n (edge source) |
| data_i   | in  | clk_a, rst_n               |
| direct_i | in  | clk_b                      |
| dual_i   | in  | clk_a, clk_b, rst_n        |
| thru_i   | in  | (comb feedthrough)         |
| data_o   | out | clk_a, rst_n               |
| comb_o   | out | clk_b                      |
| thru_o   | out | (comb feedthrough)         |
| sub_o    | out | clk_a, rst_n               |
```

## What the trace does

- **Input ports** are followed *forward* through combinational logic to the
  first state element that captures the signal. That element's clocks are the
  port's capture domain.
- **Output ports** are followed *backward* through combinational logic to the
  state elements that drive them.
- Both directions cross child instance boundaries, descending into a child's
  body and back out again until a state element is reached.
- When several state elements are reached, every clock is listed.

Clock names are reported as spelled in the analyzed instance. A flop inside a
child clocked by that child's own `clk` port is reported under whatever net the
analyzed instance connects to it.

## Cell values

| Value | Meaning |
|-------|---------|
| `clk_a, clk_b` | Every clock that captures or drives the port |
| `(edge source)` | The port itself drives a procedural block's event control |
| `(comb feedthrough)` | The trace reached no state element |
| `(latch)` | The trace reached an `always_latch`, which exposes no clock name |
| `(interface port)` | Interface ports are not traced |
| `(depth limit)` | The hierarchy depth limit was hit; the list may be incomplete |

## Asynchronous resets are reported as domains

There is no clock-versus-reset heuristic. A block counts as sequential when its
event control carries at least one edge, and *every* edge-triggered signal in
that list is reported. So:

```systemverilog
always_ff @(posedge clk or negedge rst_n) ...
```

reports the domain `clk, rst_n`, and every port reached through that flop lists
`rst_n` alongside the real clock.

This is deliberate. Nothing in the AST distinguishes a clock from an async
reset, and every rule that tries — testing which signal gates the first `if`,
matching `rst`/`_n` name patterns — silently mislabels some real designs.
Over-reporting is deterministic and explainable; guessing is not.

For the same reason a reset port shows `(edge source)` rather than `(clock)`.

## Limitations

- **Bit granularity is not modelled.** Different bits of one port can genuinely
  have different clocks; they are unioned into a single row.
- **Level-sensitive blocks are combinational.** `always @(a or b)` has no edge,
  so it is traced through rather than treated as a state element.
- **Full elaboration is required.** Unlike the LSP server's diagnostics path,
  this tool does not use slang's lint mode, because it needs real top-level
  modules for hierarchical paths to exist. An incomplete filelist therefore
  produces real elaboration errors instead of being papered over.

  Take the warning seriously:

  ```
  warning: the design elaborated with 133 errors; clock domains may be incomplete.
  ```

  slang marks expressions that failed to elaborate as bad, and the trace walks
  straight past them. A design with errors can print a table that looks fine but
  quietly under-reports — typically as spurious `(comb feedthrough)` rows on
  ports whose driving expression failed to type-check. Fix the filelist before
  trusting the output.

## Related

- `lazyverilog-rtltree` — instance hierarchy for the same filelist.
