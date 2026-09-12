# Edit-path performance

`startup-perf.md` covers the cold project index — the wait before the editor
answers at all.  This covers the other half: the steady-state edit loop, and it
is a different cost model.  Start-up is one large batch on background threads.
The edit loop is a few tens of milliseconds of work per keystroke on the one
thread that answers requests, and the user feels every one of them.

## What an editor actually sends per keystroke

Neovim's LSP client, with this plugin's default `on_attach`, does the following
for every `didChange` it sends (debounced at 150 ms by `debounce_text_changes`):

| Trigger | Request | Sent by |
|---|---|---|
| `didChange` | `textDocument/foldingRange` (whole file) | `vim.lsp.foldexpr`, via the `LspNotify` autocmd in `vim/lsp/_folding_range.lua` |
| `didChange` | `textDocument/inlayHint` (whole file) | `vim.lsp.inlay_hint`, same autocmd |
| `didChange` | — | the server's own reparse, then lint, then `publishDiagnostics` |

Two things follow from this that are easy to get wrong:

* **`foldingRange` is a per-keystroke request, not a per-open one.**  It is
  whole-file, and its cost is paid on the edit path.
* **The editor asks in the window it just opened.**  Neovim issues the request
  from the `didChange`/`didOpen` notification itself, so it reliably arrives
  while the parse that notification started is still running.  A handler that
  gives up when `DocumentState::tree` is null answers *every* editor request
  with nothing.  See "Answering before the parse lands" below.

Requests are answered one at a time (`RemoteEndPoint(..., max_workers = 1)` in
`src/server.cpp`), so an expensive request does not just cost its own latency —
it delays the completion or hover the user is actually waiting on.

## Measuring

```bash
tools/edit_latency_bench.py <project-root> <file-in-it>
tools/edit_latency_bench.py ~/work/chip rtl/core/alu.sv --cpus 0
```

Every number it prints is a client-observed round trip, so a slow number there
is the server's fault and a fast one points at the editor or the transport.

Three things this does **not** measure, which matter just as much:

* **Reply size.**  The client decodes the JSON on its main loop.  A 13k-line RTL
  file produces ~3800 folding ranges and 327 KiB on the wire.  Measure this rather
  than assume it: on that payload Neovim spends ~4 ms in `vim.json.decode` and ~2 ms
  in the fold handler's walk, which is small next to the request it arrived on.
* **What the client does with the reply.**  Neovim's
  `vim.lsp._folding_range.State:evaluate()` walks `for row = startLine, endLine`
  for *every* range it is handed, so the client-side cost tracks the sum of the
  range spans, not the range count.  The 3803 ranges above cover 42684 rows.
* **Whether the reply was right.**  An empty reply is the fastest possible one.

That last point is the trap.  Benchmark an edit loop against a server that
answers `[]` mid-reparse and every number looks excellent.

## Round 1: `foldingRange` was quadratic twice over

Measured on a 4-core VM, `Release`, on generated RTL of the shape a real block
has (state enums, case statements, nested if/else, loops, `always_ff`,
functions).  Client-observed round trip, median of 7, and the fold output is
byte-identical on both sides across 127 files.

| File | CPUs | before | after |
|---|---|---|---|
| 4k lines | 1 | 25.0 ms | 16.8 ms |
| 4k lines | 4 | 24.0 ms | 14.1 ms |
| 13k lines | 1 | 163.4 ms | 50.4 ms |
| 13k lines | 4 | 190.6 ms | 63.2 ms |

In-process, excluding JSON serialization, the same change is 9.3 ms → 5.4 ms at
840 folds and 1361 ms → 162 ms at 13441 folds: the gap widens with size, which
is what removing a quadratic term looks like.

Two causes, both invisible on the small files the unit tests use:

1. **`emit()` built a line table over the whole buffer for each fold it
   produced.**  `LineTable` scans the file to index line starts, and `emit()` is
   called once per fold, so producing N folds for an M-byte file cost O(N x M).
   On the 13k-line file this was ~80 ms of the ~170 ms, most of it inside
   `IfElseChainVisitor`, which emits the most folds.  Fixed by building one line
   table per request and handing it in.

2. **`normalize_folds()` answered its grouping questions by scanning the fold
   list.**  Four passes did this, one of them nested two deep — quadratic in the
   fold count, which grows linearly with the file.  ~45 ms of the ~170 ms.  Fixed
   with hash indexes and, for the declaration-containment pass, a sort plus a
   running maximum.

The guard is `foldingRange: cost grows with the file, not with its square` in
`tests/test_folding_ranges.cpp` (`[folding][scaling]`).  It is a ratio between
two inputs of the same shape and different size, never a millisecond budget:
doubling the file should roughly double the cost.  Measured at 2.1-2.5 across a
16x range of sizes; the quadratic version it replaced measures 3.3-3.8 and fails
it.

### Answering before the parse lands

Round 1 also fixed something the timings had been hiding.  `didChange` installs a
text-only `DocumentState` and hands the parse to a worker, and
`provide_folding_range()` returned `{}` for any snapshot without a tree.  Since
the editor asks from the notification itself, that was the normal case, not an
edge case:

```
  settled:                3803 ranges in 183 ms
  right after keystroke 0:   0 ranges in 7 ms
  right after keystroke 1:   0 ranges in 1 ms
```

Neovim applies that empty set to the whole buffer and asks again only on the next
change — which lands in the same window.  So folds collapsed on the first
keystroke and did not come back, and a file whose first `didOpen` request lost
the race opened unfoldable and stayed that way.  Measured in headless Neovim on a
4k-line file: 4005 of 4008 lines carried a fold level after open, and 0 after
typing five characters.

The first fix for this was a `FoldingRangeCache` that served the previous
document's folds while a reparse was in flight, with the token scan as the
fallback for a buffer that had nothing cached yet.  Round 2 replaced it.

## Round 2: folding left the AST entirely

The cache answered the reparse window, but it was only ever *read* in that
window — `lookup()` sat inside the `if (!state->tree)` branch, so once the parse
landed the request path recomputed everything.  Measured in headless Neovim on a
42k-line file, the steady-state cost of a keystroke's `foldingRange` was ~80 ms
on the one thread that answers requests.

clangd solves the same problem by never deriving folds from the AST at all:

```cpp
// clang-tools-extra/clangd/SemanticSelection.h
/// This version uses the pseudoparser which does not require the AST.
llvm::Expected<std::vector<FoldingRange>>
getFoldingRanges(const std::string &Code, bool LineFoldingOnly);
```

`provide_folding_range()` now does the same: `token_folds(state->text)`, with no
tree check, no cache and no fallback path.  The answer for a given text is the
same whether or not its parse has landed, which is what the two
"parse in flight" tests in `tests/test_folding_ranges.cpp` pin — they catch the
reparse window and `REQUIRE` having caught it, so they cannot pass by quietly
measuring a settled document instead.

### What this costs

Not much time — the AST passes were ~17 ms of the ~80 ms; the token scan,
`normalize_folds()` and JSON are the rest.  What it costs is folds that
SystemVerilog cannot resolve lexically, because `my_type_t state;` and
`my_child u_inst (...);` are the same token shape:

| gone | consequence |
|---|---|
| `instance` folds | a `#(...)` fold now starts on the instance line, so `zc` there hides only the parameter overrides |
| identifier-led declaration runs | `state_e state_q;` and friends produce no fold |
| non-ANSI port declarations in a run | the run starts at the first keyword-led declaration |
| module-header list trimming | `#(...)` and `(...)` share the `)(` line, so Neovim merges them into one header fold |

Measured against the AST version over the RTL corpora — share of the fold set
that changes: `bp_processor.sv` loses 11 of 29 folds, `cva6.sv` 27% differs,
`ibex_core.sv` 18%, `ibex_pkg.sv` 2%.  Generated register files are nearly
unaffected in count but not in kind: `pinmux_reg_top.sv` keeps 2593 of 2595
ranges while 1400 of them change from `instance` to `region`.

clangd accepts a narrower loss for the same design, because C++ folding only
ever folds bracket pairs, `#if` regions and comments — it never attempts
declaration runs or instance regions.  Note also that clangd's lex-only
implementation is quadratic in practice (measured 3.5-3.9x per doubling, where
this project's `[folding][scaling]` guard demands under 3.0); ours is not, and
the guard stays.

## The editor half

Some of the per-keystroke cost is not the server's to fix.  The plugin's
`on_attach` turns on two features that each add a whole-file request per change,
and both are now switchable (`lua/lazyverilog/config.lua`):

```lua
require("lazyverilog").setup({ folding = false, inlay_hints = false })
```

`folding = false` leaves `foldmethod` alone, which takes fold computation — the
request, the reply, and Neovim's own `evaluate()` walk — out of the edit loop
entirely.  `inlay_hints = false` stops Neovim requesting hints on every change.

The server half of the hint switch works through capability negotiation, and it
is worth knowing where that reaches.  `caps.inlayHintProvider` is built from
`[inlay_hint].enable`, and with the capability off Neovim sends *no* inlayHint
requests at all — measured 0 against 7 over five keystrokes — even though
`vim.lsp.inlay_hint.enable(true)` was called and `is_enabled()` reports true.
But capabilities are exchanged once and never revised, so this only works when
`initialize` can find the config:

| where `lazyverilog.toml` is, relative to the client's `root_dir` | found at `initialize`? |
|---|---|
| at it | yes |
| above it | yes — `initialize` walks up, as didOpen does |
| below it | **no** |

The last row is not an oversight.  `initialize` knows only `rootUri`; it does not
know which file is about to be opened, and this project's config rule is "walk up
from the opened file".  Searching downward through a large repository is neither
cheap nor unambiguous.  It is also the common case, because Neovim's
`vim.fs.root` resolves a flat marker list by marker order rather than proximity,
so the plugin's default `{ ".git", "lazyverilog.toml" }` roots at the repository
whenever the config lives in a subdirectory.

LSP's answer for exactly this is dynamic registration —
`client/unregisterCapability` after the config turns up.  Neovim accepts it for
`inlayHint` (`dynamicRegistration = true`) but not for `foldingRange` (`false`),
and `foldingRangeProvider` has no config option to revoke in the first place.
