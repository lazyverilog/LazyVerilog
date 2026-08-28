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

## Editing a header: what an unsaved keystroke refreshes

Typing in a header used to queue every file that `include`s it, on every
keystroke, and bump the background generation each time.  That generation is
what scopes `HeaderTextCache` and the directives-only projection built from it,
so each character threw both away and made the resulting storm re-read and
re-preprocess the whole header once per includer.  On a one-core slice that
storm runs on the same CPU the buffer's own parse needs.

Two rules replace it:

- **The fanout waits for the typing to stop.**  `parse_worker_loop()` holds the
  reparsed URIs and fans out once the parse queue has been idle for
  `kLiveShardIdleDelay`, the same deferral the live shard rebuild already used.
- **Unsaved edits reach open buffers; disk changes reach the project.**  Every
  *open* buffer that includes the header is requeued, because it is on screen.
  Closed project files get one requeue between them, enough to re-claim the
  header, and are otherwise left to the save path: the watcher reports the saved
  file and `refresh_changed_extra_files()` re-queues every includer against the
  text that actually reached disk.

The trade is that between an edit and a save, a closed includer's shard reflects
the saved header.  That is consistent with the rest of the model — a closed
file's own text is read from disk too.

## Include directories are resolved once, not per parse

A `SourceManager` is built per parse, and `addUserDirectories()` globs each
configured directory and then canonicalizes every match — a stat per path
component, per directory, on every keystroke and once per project file.  With a
few hundred `+incdir+` entries that dominated the edit path, and on a shared
filesystem each of those stats is a round trip.

The glob now runs once, where the config is set, and the result is handed to
slang as `PreprocessorOptions::additionalIncludePaths`.  Search order is
unchanged: slang tries the including file's own directory first, then these.
A directory added after the config was loaded is picked up on the next config
reload, the same event that already re-parses the project.

## Editing a file that includes a large header

The open-buffer path has its own header cache (`OpenParseHeaderCache`), which
outlives an indexing burst instead of being scoped to one.  Caching the text is
not enough on its own: the text was never the expensive part.  A 214-line module
including a 14 005-line header measured **38.7 ms per keystroke** on one core
against 0.4 ms for the same module without the include, and `fromText()` was
29 ms of a 29 ms `make_state()` — pure lex, preprocess and parse of the header,
re-done for every character typed.

So the edit path projects too, under two conditions:

- **The header is past `kDirectivesOnlySeedBytes` (64 KB).**  Below that the
  exact tree is worth more than the microseconds; the threshold is what keeps
  this change confined to the shape that actually hurts.
- **Its own shard was built from a parse of it on its own**
  (`standalone_header_uris_`).  That is what makes the header's declarations
  recoverable from somewhere else.

Measured on the same case: 38.7 ms → 2.1 ms per keystroke.

An open buffer's includes are claimed too.  The indexer skips an open buffer's
*own* shard because its text is unsaved, but a header's shard is built from the
header, so nothing about it is unsaved.  Two paths had to learn this: the live
branch of `background_index_loop()`, and the closed branch when the buffer opened
while the disk parse was in flight — that one discarded the whole parse, claim
included, and nothing re-queues a file that is now open.  A header only the open
buffer includes therefore earns a shard as well: 38.7 ms → 5.7 ms there, the
difference being that this file's own shard is the discarded one.

Header shards are in **both** snapshots.  `build_extra_file_snapshot_locked()`
listed only `extra_cache_`, so `definition_of` and everything else reading that
snapshot could find a header's declarations only through an includer parsed
before the projection was installed — a copy that is not guaranteed to exist.

Two consequences, both pinned by `tests/test_open_header_projection.cpp`:

- The buffer's own tree stops carrying that header's declarations, so features
  answer for them from the header's shard, exactly as they already do for a
  closed file.  Its macros still expand — directives are what the projection
  keeps.
- Semantic diagnostics are unaffected.  `BackgroundCompiler` builds its own
  `SourceManager` and its own trees from the filelist and open-buffer text
  (`background_compiler.cpp`); it never reads `DocumentState::tree`, so it still
  sees every header in full.

`OpenParseHeaderCache` never touches the filesystem.  It follows the same event-driven rule
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
