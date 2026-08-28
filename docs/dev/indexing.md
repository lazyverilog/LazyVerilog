# Indexing philosophy

LazyVerilog follows the same high-level split used by clangd: **current file
uses AST, cross-file facts use indexes**.

The final goal is to never keep full ASTs for the whole project alive.  The
current/open buffer is parsed into a live `SyntaxTree` so editor requests can
answer from exact unsaved text.  Project files from `.f` are parsed
asynchronously into compact per-file index shards, then exposed through
published index snapshots.

In practice there are three distinct layers:

- **Current request file**: use the live `DocumentState` / `SyntaxTree` first.
  Do not answer current-file facts from `opened_files_index_cache_`; that cache
  intentionally excludes the current URI so stale or duplicate current-file facts
  do not leak into completions, code actions, lint, or references.
- **Other open buffers**: use optional `SyntaxIndex` summaries derived from their
  live AST snapshots.  This preserves unsaved cross-file edits, for example when
  `memory.sv` is open and edited but `top.sv` is the file currently requesting
  AutoInst or named-port completion.
- **Closed/project files**: use background-published `SyntaxIndex` shards from
  the filelist/project cache.  These files should not retain full ASTs.

So a request in `top.sv` should see:

```text
top.sv                -> live AST
memory.sv open        -> other-open-buffer SyntaxIndex summary
alu.sv closed in .f   -> project SyntaxIndex shard
```

Additional rules:

- request handlers should not walk closed project-file ASTs;
- whole-design features may build a structural view from the current AST, but
  should use index data for project files.

`opened_files_index_cache_` is keyed by the current request URI:

```text
opened_files_index_cache_["file:///top.sv"]
  = merged SyntaxIndex from other open buffers only
  = excludes file:///top.sv
```

The exclusion is intentional because the current file is already represented by
its exact live AST.

## Locking and snapshot principle

Analyzer maps and caches are protected by `map_mutex_`, but expensive index work
must not run while that mutex is held.  Hold the mutex only long enough to copy
cheap immutable snapshots, such as `shared_ptr<const DocumentState>` values or
already-published `SyntaxIndex` shards, then release it before walking ASTs,
building dynamic indexes, or consulting many shards.

This works because open documents are immutable snapshots: `didChange` replaces
the whole `DocumentState`, and older requests can safely finish against the
state they already copied.  Request paths such as completion, hover,
definition, code actions, RTL tree, and auto-wire should avoid turning
project/open-file indexing into a global serialization point.  If a live
filelist entry already has an index in `extra_cache_`, prefer that cached shard
and attach the live `DocumentState` separately for callers that truly need AST
access.

## Current-file structural index cache

Some features need index-shaped facts even for the currently open file: RTL
hierarchy, references by `SymbolID`, inlay hints, stale-autoinst lint, and some
automatic code-generation commands.  Those features should call
`get_structural_index(state)` instead of rebuilding the same AST-derived index
manually.

The cache is stored on the immutable `DocumentState` snapshot.  A `didChange`
creates a new `DocumentState`, so cache invalidation is automatic: old requests
can safely finish against the old snapshot while new requests see a fresh cache.
Do not retain full ASTs for closed project files to get similar behavior; closed
files should continue to participate through compact `SyntaxIndex` shards only.

## Shared `include`d headers

A header is not a filelist entry, so it has no shard of its own until something
`include`s it.  The background indexer gives it one: the first worker whose file
pulls the header in claims it and builds its shard.  That shard is built from a
parse of the header **on its own**, not from the includer's tree — a restricted
build of an includer keeps every declaration it finds, so deriving the header's
shard from one produced a copy of that includer under the header's URI.

A header that cannot stand alone — a port list, a module opened in one file and
closed in another — has no tree of its own to index, and only for those is the
shard still derived from the includer that pulled them in.  The parse itself is
the test: a tree, and no error-severity parse diagnostic.

**The header's shard is where its declarations live.**  Once it stands alone, the
rest of the indexing burst is served a projection of the header holding only its
preprocessor directives, with every other line blanked out (line numbers are
preserved, so a macro still reports the line it is defined on).  Directives are
the part that is genuinely per-includer — what a `` `define `` means depends on
where it expands — while declarations do not vary by includer at all, and
re-reading them once per file costs `O(files x header)`.

Two consequences worth knowing before writing a feature:

- An includer's shard no longer carries the header declarations it mentions.
  Resolve those through the header's own shard in the published snapshot.
- The saving is bounded, not total: the projection can only be installed after
  some file has proved the header stands alone, so the files already parsing at
  that moment still read it whole.  The header's bulk is therefore read at most
  once per background worker — one read on a single-core slice — instead of once
  per includer.  Those few includers keep their mentioned header declarations,
  which is a duplicate of what the header shard holds, not a different answer.

The open-buffer path is deliberately not projected.  An edit needs the current
file's own AST to be exact, including whatever its headers splice into it, and
that path has its own cache (`OpenParseHeaderCache`), which outlives an indexing
burst instead of being scoped to one.

That cache never touches the filesystem.  It follows the same event-driven rule
as the project shards: entries live until `refresh_changed_extra_files()` — fed
by `workspace/didChangeWatchedFiles` — reports the file changed, or until
`Analyzer::close()` drops a header that was open as a buffer, or until a config
reload clears everything.  Nothing stats, nothing polls, so an included header
costs zero metadata calls per keystroke.

The trade is explicit: a client that does not deliver watched-file events keeps
serving cached header text until the project configuration changes or the server
restarts.  Only extensions in `kWatchedSourceExtensions` are cached — that array
is also what builds the watcher glob registration in `server.cpp`, so a path the
client is not asked to watch is one the cache refuses to hold.

## Published project index snapshot

The published project index is shard-based.  A `ProjectIndexSnapshot` contains
immutable per-file `SyntaxIndex` shard references plus lightweight global lookup
maps such as module name -> shard/module entry.  Publishing a changed project
index therefore does not copy every symbol/reference from every file into one
flat index.  Feature paths should use `Analyzer::project_index_snapshot()` and
either consult its global lookups or iterate the relevant shard references rather
than materializing a compatibility merge.

## Project-index refresh notifications

Features that depend on definitions from filelist/project files may produce partial
answers before the background indexer publishes its first merged snapshot.  When
the project snapshot is republished, the server requests `workspace/inlayHint/refresh`
so clients can re-query inlay hints without waiting for a user edit.  The same
publish callback also republishes foreground diagnostics for open documents,
because cross-file lint rules such as stale-AutoInst can change when a module
port list in another file changes.  Keep this refresh path lightweight: project
indexing still happens in the background, and request handlers should not
synchronously merge or parse project files.


### How Go-to-def works

┌────────────────┬──────────────────────┬────────────────────────────┬─────────────────────────┐
│   File type    │ Module/Port/Instance │ Typedef/Variable (generic) │ Function/Task (generic) │
├────────────────┼──────────────────────┼────────────────────────────┼─────────────────────────┤
│ Current file   │ ✓ (AST)              │ ✓ (AST)                    │ ✓ (AST)                 │
├────────────────┼──────────────────────┼────────────────────────────┼─────────────────────────┤
│ Open .f file   │ ✓ (index)            │ ✓ (index, faster)          │ ✓ (index)               │
├────────────────┼──────────────────────┼────────────────────────────┼─────────────────────────┤
│ Closed .f file │ ✓ (index)            │ ✓ (index, new)             │ ✓ (index)               │
└────────────────┴──────────────────────┴────────────────────────────┴─────────────────────────┘
