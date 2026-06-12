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
background_compilation_threads = 1
background_compilation_debounce_ms = 1500
nice_value = 10
log_timing = false
```

Increasing `background_compilation_threads` can allow a newer snapshot to start
while an older compile is still running, but that can consume more CPU. Keep it
at `1` unless the machine has enough spare resources.

`nice_value` is applied by the background worker with `setpriority()` when the
worker thread starts. Increasing it lowers the worker's scheduling priority on
platforms that honor POSIX nice values. Lowering the value below the current
process priority can require extra OS privileges and may be ignored or fail.

`log_timing` emits background compilation timing lines to the LSP log. Keep it
`false` for normal use; enable it temporarily when profiling parse or semantic
compilation latency.
