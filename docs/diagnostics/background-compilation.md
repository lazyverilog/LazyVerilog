# Background semantic compilation

LazyVerilog has two diagnostic paths:

1. **Fast foreground diagnostics** run on `didOpen` / `didChange` and publish syntax,
   preprocessor, and LazyVerilog lint diagnostics for the edited document.
2. **Background semantic compilation** is optional and runs only when
   `[compilation].background_compilation = true`.

The background compiler builds a fresh Slang semantic compilation from:

- files listed by `[design].vcode`
- open editor buffers, using their unsaved in-memory text
- `[design].define` preprocessor defines

Semantic diagnostics are cached by URI and merged into `publishDiagnostics` for
open documents after compilation finishes.

## HPC/resource behavior

Background compilation is intentionally conservative for shared/HPC systems:

- disabled by default in code
- configurable worker count, default `1`
- debounced after edits, default `1500 ms`
- jobs are coalesced so rapid typing compiles only the newest snapshot
- stale results are discarded with a generation counter
- LSP request handlers do not wait for semantic compilation
- diagnostics are cached for all compiled files but published only for open
  documents to avoid flooding clients

Recommended HPC settings:

```toml
[compilation]
background_compilation = true
background_compilation_debounce_ms = 1500
log_timing = false
```

Worker count and thread priority are not configurable. Each worker compiles the
whole design rather than sharing one compile, so a second worker only lets a
newer snapshot start before an older one finishes — at the cost of a duplicated
full-design compilation whose result the generation check usually discards. The
binding resource is peak memory, not CPU, so the useful worker count is `1` on
every machine size.

Workers renice themselves to `10` on Linux when they start, where the nice value
is per-thread and this leaves LSP request handling untouched. It is a no-op on
macOS, where the same call would renice the whole server, and on Windows, which
has no POSIX nice. A server already started under a higher `nice` keeps that
priority: the workers only ever yield further, never ask for more.

`log_timing` emits background compilation timing lines to the LSP log. Keep it
`false` for normal use; enable it temporarily when profiling parse or semantic
compilation latency.
