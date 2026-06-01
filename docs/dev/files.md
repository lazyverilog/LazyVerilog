# Design Filelist and Extra-File Cache

LazyVerilog can index files outside the current editor buffer through the design
filelist configured in `lazyverilog.toml`:

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]
```

The filelist is used by cross-file features such as go-to-definition, hover,
RTL tree, inlay hints, Connect/Interface helpers, and stale AutoInst lint
checks.

## Terms

### `.f` file

A `.f` file is a design filelist. Each non-empty source line points to a
SystemVerilog/Verilog source file, usually relative to the `.f` file location:

```text
./memory.sv
./register.sv
./params.svh
```

### `mtime`

`mtime` means **modification time**. It is the filesystem timestamp that changes
when a file's contents or metadata are modified. LazyVerilog uses mtimes as a
cheap way to decide whether cached parsed/indexed files may be stale.

## Startup and first cache build

On startup or config load, the server reads `lazyverilog.toml`, resolves the
configured `.f` path, reads the filelist entries, and calls:

```cpp
analyzer_.set_extra_files(
    load_vcode_files(root_, config_),
    resolve_vcode_path(root_, config_)
);
```

At this point LazyVerilog stores:

- the list of extra source-file paths
- the resolved `.f` file path

It does **not** immediately stat and parse every listed source file. The initial
per-file mtimes are saved lazily, the first time a feature asks for extra-file
snapshots:

```cpp
extra_file_snapshots()
```

That first snapshot sees that the cached `.f` mtime is empty, refreshes the
extra-file cache, stats each listed file, parses readable files, and stores each
cache entry's source-file mtime, document state, and syntax index.

## Normal request path

After the cache is warm, `extra_file_snapshots()` normally checks only the `.f`
file's mtime:

```cpp
const auto current_mtime = file_mtime(filelist_path_);
if (current_mtime != filelist_mtime_) {
    refresh_extra_cache_locked();
    filelist_mtime_ = current_mtime;
}
```

If the `.f` mtime did not change, LazyVerilog reuses the cached extra-file
snapshots. This avoids doing one `stat()` per listed RTL file on every editor
request, which is important for large filelists and NFS/HPC filesystems.

## If the `.f` file changes

When the `.f` file's mtime changes, the next `extra_file_snapshots()` call
refreshes the cache:

1. stat each file listed by the current in-memory filelist path list
2. reuse cached parse/index entries whose source-file mtime is unchanged
3. read and reparse new or changed files
4. drop removed or unreadable files from the refreshed cache
5. save the new `.f` mtime

After that refresh, future requests return to the cheap path: check only the
`.f` mtime and reuse the cache if unchanged.

## If a listed source file changes but `.f` does not

If a source file listed in `.f` changes on disk while the `.f` file itself is
unchanged, the normal optimized path does **not** notice immediately. It checks
only the `.f` mtime, sees no filelist change, and reuses the cached parse/index
for that listed source file.

This is an intentional performance tradeoff:

- fast per-request behavior: one mtime check for the `.f` file
- downside: disk-only edits to listed files can remain stale until the cache is
  invalidated or the file is opened in the editor

The cache is refreshed or bypassed in the cases below.

## Cache refresh and bypass cases

### `.f` file changes

Changing the filelist contents changes the `.f` mtime. The next feature request
that needs extra files detects that mismatch and refreshes the extra-file cache.
This catches added, removed, and reordered filelist entries.

### Config reload calls `set_extra_files(...)`

When configuration is reloaded, the server rereads `lazyverilog.toml`, resolves
`[design].vcode`, reloads the filelist paths, and calls `set_extra_files(...)`.
That resets the cached filelist mtime, so the next `extra_file_snapshots()` call
refreshes from the newly configured filelist.

### Defines change

Preprocessor defines affect parsing. For example:

```systemverilog
`ifdef RTL_SIM
    memory u_mem3 (...);
`else
    memory u_mem4 ();
`endif
```

When `Analyzer::set_defines(...)` is called, the extra-file cache is cleared and
the filelist mtime is reset. The next extra-file snapshot reparses files using
the new define set.

### A listed file is opened in the editor

Open editor buffers override the cached disk snapshot. If a file listed in `.f`
is open, `extra_file_snapshots()` returns the live `DocumentState` and live
`SyntaxIndex` from the open buffer instead of the cached disk entry.

This means unsaved edits in an open listed file are visible to cross-file
features. For example, if `demo/memory.sv` is listed in `demo/vcode.f` and is
open in Neovim, stale AutoInst diagnostics in `demo/memory_top.sv` can use the
live in-memory port list from `demo/memory.sv`.

### Server restarts

A server restart drops all in-memory cache state: open documents, extra-file
cache entries, and saved mtimes. The next session reads config and rebuilds the
extra-file cache on demand from current disk contents.

## Stale AutoInst diagnostics

`stale_autoinst_diagnostic` validates an instance's named port connections
against the instantiated module's port list. For example, `demo/memory_top.sv`
instantiates `memory`, but `memory` is declared in `demo/memory.sv`, so this
rule needs the merged current-file plus extra-file module index.

When this rule is enabled:

```toml
[lint.module]
stale_autoinst_diagnostic = true
```

the diagnostic publishing path builds a merged lint index before running lint.
That merge calls `extra_file_snapshots()`.

Therefore, yes: when a file containing module instances is opened and diagnostics
are published, `stale_autoinst_diagnostic = true` causes the server to request
extra-file snapshots immediately during that diagnostic pass. With the normal
filelist path, a warm cache checks only the `.f` mtime; on the first pass or
after invalidation, it refreshes the extra-file cache as described above.
