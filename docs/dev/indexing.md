# Indexing philosophy

LazyVerilog follows the same high-level split used by clangd: **current file
uses AST, project files use index**.

The final goal is to never keep full ASTs for the whole project alive.  The
current/open buffer is parsed into a live `SyntaxTree` so editor requests can
answer from exact unsaved text.  Project files from `.f` are parsed
asynchronously into compact per-file index shards, then exposed through
published index snapshots.

In practice:

- current file requests should inspect the current `SyntaxTree` first;
- cross-file lookups should use opened-file/project index shards;
- request handlers should not walk closed project-file ASTs;
- whole-design features may build a structural view from the current AST, but
  should use index data for project files.

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

## Project-index refresh notifications

Features that depend on definitions from filelist/project files may produce partial
answers before the background indexer publishes its first merged snapshot.  When
the project snapshot is republished, the server requests `workspace/inlayHint/refresh`
so clients can re-query inlay hints without waiting for a user edit.  Keep this
refresh path lightweight: project indexing still happens in the background, and
request handlers should not synchronously merge or parse project files.

