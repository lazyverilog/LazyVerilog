# Background semantic compilation

LazyVerilog has two diagnostic paths:

1. **Fast foreground diagnostics** run on `didOpen` / `didChange` and publish syntax,
   preprocessor, and LazyVerilog lint diagnostics for the edited document.
2. **Background semantic compilation** is optional and runs only when
   `[compilation].background_compilation = true`.

The background compiler builds **one fresh Slang compilation per open project**,
each from that project's own:

- files listed by its `[design].vcode`
- open editor buffers under its root, using their unsaved in-memory text
- its `[design].define` preprocessor defines and `+incdir+` entries

A Slang compilation has one preprocessor and one flat module namespace, so it can
only ever belong to one project: compiling every open project together gave one
elaboration two projects' `fifo` and handed one project's defines to the other's
parse.  The set of files a `.f` names is the scope SystemVerilog binds an
instantiation over, so that is what each compilation is built from.

`background_compilation` is read **per project**, the same way `[lint]` is, and it
is the only key in the table.  A project with `background_compilation = false`
contributes no compilation and shows no semantic diagnostics in its buffers, even
while a project open beside it has it on.

What stays session-level is the worker itself — one debounce timer and one
thread.  It runs whenever any open project wants it, which is why the switch is
honoured where the work happens (the group a project contributes, and the
publish for its own buffers) rather than by starting or stopping the worker.

Projects compile one after another rather than in parallel: peak memory is the
binding resource, so N projects cost N times the wall clock and one project's
memory.  A file two projects both list is compiled once for each; identical
diagnostics are collapsed, and genuinely different ones are kept, because shared
IP does mean different things under two projects' defines.

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

Compilation is debounced by a fixed 1500 ms: rapid typing pushes the window out,
so what it really sets is how long the user must pause before the heaviest thing
the server runs is allowed to start. It is not configurable — one worker owns the
timer, so with two projects open a per-project setting could only ever have meant
"whichever config was read last".
