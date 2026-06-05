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

The merged project index returned by `Analyzer::extra_project_index()` is a
published immutable snapshot. Request handlers read that snapshot and never merge
all filelist shards synchronously. While the background index is still warming,
features see the latest published snapshot, which can be empty or incomplete.

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
done outside the analyzer mutex, then committed under the mutex.

When a listed file is closed, the analyzer schedules an asynchronous disk-backed
reindex for that file so the project shard eventually returns to saved contents
without blocking the close notification.

## Stale AutoInst diagnostics

`stale_autoinst_diagnostic` validates an instance's named port connections
against the instantiated module's port list. For example, `demo/memory_top.sv`
instantiates `memory`, but `memory` is declared in `demo/memory.sv`, so this rule
needs the current-file structural index plus the background-published project
index.

When this rule is enabled:

```toml
[lint.module]
stale_autoinst_diagnostic = true
```

the diagnostic publishing path builds a current-file lint index and merges the
latest `Analyzer::extra_project_index()` snapshot. It does not synchronously parse
or merge every filelist file on the diagnostic path. If the background project
index is not warm yet, stale-AutoInst diagnostics can be incomplete until the
project snapshot is published.
