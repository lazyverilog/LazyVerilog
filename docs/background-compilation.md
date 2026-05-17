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
- diagnostics are published only for open documents to avoid flooding clients

Recommended HPC settings:

```toml
[compilation]
background_compilation = true
background_compilation_threads = 1
background_compilation_debounce_ms = 1500
```

Increasing `background_compilation_threads` can allow a newer snapshot to start
while an older compile is still running, but that can consume more CPU. Keep it
at `1` unless the machine has enough spare resources.

`nice_value` is currently reserved; changing process priority safely requires a
future subprocess or platform-specific per-thread implementation.
