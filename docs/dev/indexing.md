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

