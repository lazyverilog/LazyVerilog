# Design Filelist and Extra-File Cache

LazyVerilog can index files outside the current editor buffer through the design
filelist configured in `lazyverilog.toml`:

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]
```

The filelist is used by cross-file features such as go-to-definition, hover,
RTL tree, inlay hints, Connect/Interface helpers, completion in explicit
cross-file contexts, and stale AutoInst lint checks.

## `.f` file handling

A `.f` file is a design filelist. Each non-empty source line points to a
SystemVerilog/Verilog source file, usually relative to the `.f` file location:

```text
./memory.sv
./register.sv
./params.svh
```

The server also recognizes simulator-style include directory entries:

```text
+incdir+rtl/include
+incdir+/abs/include
+incdir+dir_a+dir_b
```

Include directories are passed to slang's `SourceManager` for `` `include "..." ``
lookup when parsing open buffers and explicit filelist sources. They are **not**
parsed as independent extra files.

## Startup and config reload

On startup, initialization with a workspace root, or
`workspace/didChangeConfiguration`, the server:

1. reads `lazyverilog.toml`
2. resolves `[design].vcode` relative to the config root
3. reads the filelist source paths and `+incdir+` entries
4. applies defines, include directories, and filelist entries with one
   `Analyzer::set_project_config(...)` transaction

`Analyzer::set_project_config(...)` stores the normalized parse inputs, clears
the old extra-file cache, clears the old merged project-index snapshot, and
schedules at most one asynchronous background reindex for that config load.  It
does **not** synchronously parse the full filelist on the LSP request or
configuration path.

## Background project indexing

Configured project files are parsed by a single analyzer-owned background indexer
thread. The worker copies the next path and parse options while holding the
analyzer mutex, releases the mutex while slang parses/builds the per-file shard,
and reacquires the mutex only to commit the resulting `SyntaxIndex` shard.

Open editor buffers are authoritative. If a listed file is open, its in-memory
text and AST-derived dynamic shard replace the disk-backed filelist shard. The
current/open buffer keeps a `DocumentState` / `SyntaxTree`; closed filelist files
keep only compact `SyntaxIndex` shards.

Request paths should think in three layers:

```text
1. current request file  -> live DocumentState / SyntaxTree
2. other open buffers    -> cached SyntaxIndex summaries derived from live ASTs
3. closed/project files  -> background SyntaxIndex shards
```

`opened_files_index_cache_` represents layer 2 only. It is keyed by the current
request URI and excludes that URI, for example a request in `top.sv` may reuse a
cached merged index for other open files such as `memory.sv` and `pkg.sv`, but
`top.sv` itself must still be answered from its live AST. This avoids duplicate
or stale current-file facts while still preserving unsaved edits from other open
buffers.

The project index returned by `Analyzer::project_index_snapshot()` is a published
immutable shard snapshot: it keeps per-file `SyntaxIndex` shards plus lightweight
global lookup maps. Publishing a live-file change replaces the affected shard and
updates lookup maps without copying every symbol/reference from every file into a
new flat index. Request handlers consume either the snapshot's global lookups or
the individual shard references; they should not rebuild a whole-project flat
`SyntaxIndex` while answering editor requests. While the background index is
still warming, features see the latest published snapshot, which can be empty or
incomplete.

## Request path behavior

`extra_file_snapshots()` and `extra_index_snapshots()` are request-path snapshot
accessors. They return currently cached shards and do not stat the `.f` file,
stat every listed source, or synchronously parse missing files. This is deliberate
for NFS/HPC filesystems: even metadata I/O per editor request can become noisy on
large shared projects.

Consequences:

- Startup/config reload is cheap; indexing catches up asynchronously.
- Cross-file features may be temporarily incomplete until the background indexer
  publishes enough shards.
- Disk-only edits to closed listed files are not noticed automatically during a
  session unless the client sends `workspace/didChangeConfiguration` after the
  config/filelist change, or the file is opened so its live buffer shard replaces
  the cached shard.

## Invalidation and freshness

The extra-file cache is cleared and rebuilt asynchronously when:

- configuration reload calls `set_project_config(...)`
- preprocessor defines change via `Analyzer::set_defines(...)`
- include directories change via `Analyzer::set_include_dirs(...)`
- the server restarts

The server uses `set_project_config(...)` for user-facing startup/root-discovery
and `workspace/didChangeConfiguration` paths so one config reload produces one
project reindex generation instead of separate generations for defines, include
directories, and filelist entries.

For listed files that are open in the editor, `didOpen` / `didChange` update the
cached shard from unsaved in-memory text. The expensive AST-derived shard build is
done outside the analyzer mutex, then committed under the mutex.  Updating this
live shard schedules an asynchronous project-index snapshot publish; the edit path
itself does not wait for the whole-project merge.  Publish requests are
coalesced internally so rapid typing merges and refreshes diagnostics once per
burst instead of once per keystroke.

When a listed file is closed, the analyzer schedules an asynchronous disk-backed
reindex for that file so the project shard eventually returns to saved contents
without blocking the close notification.

## Stale AutoInst diagnostics

`stale_instance_diagnostic` validates an instance's named port connections
against the instantiated module's port list. For example, `demo/memory_top.sv`
instantiates `memory`, but `memory` is declared in `demo/memory.sv`, so this rule
needs the current-file structural index plus the background-published project
index.

When this rule is enabled:

```toml
[lint.instance]
stale_instance_diagnostic = true
```

the diagnostic publishing path builds a current-file lint index and consults the
latest `Analyzer::project_index_snapshot()` module lookup map. It does not
synchronously parse or merge every filelist file on the diagnostic path. If the
background project index is not warm yet, stale-AutoInst diagnostics can be
incomplete until the project snapshot is published.

When a project snapshot is published, the server republishes diagnostics for all
open documents. This is required for edits such as removing a port from
`memory.sv`: an already-open `memory_top.sv` may need its stale-AutoInst
diagnostics refreshed even though `memory_top.sv` itself did not receive a new
`didChange` notification.
