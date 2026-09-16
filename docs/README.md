# LazyVerilog Docs

This directory contains user-facing documentation for the current C++
implementation of LazyVerilog.

## Files

- [Features](features.md): index of user-visible features.
- [Release notes](releases/v1.1.0.md): latest user-visible changes.
- [Connect](connect.md): interactive instance-to-instance wiring.
- [Interface](interface.md): two-instance and single-instance interface views.
- [Disconnect](disconnect.md): clearing Interface connections.
- `format-options.md`: formatter-related `lazyverilog.toml` options, including
  declaration alignment, spacing controls, instance formatting, and
  function/task-call formatting.

## Scope

These docs describe the options implemented in this repository today. They use
earlier LazyVerilog documentation as a reference baseline, but are intentionally limited to
the features and option names supported by this codebase.

Notable difference from older examples:

- `[format.instance]` now uses `align_adaptive`.
- The legacy name `align_instance_port_adaptive` is still accepted by the
  config loader for compatibility, but new configs should use
  `align_adaptive`.

## Project roots and the index cache

LazyVerilog finds `lazyverilog.toml` by walking up from each file you open to the
nearest one — the editor does not choose it. Index shards are written beside that
config, in `.cache/lazyverilog/index`, with a `.gitignore` written for you; a file
with no config above it uses your user cache directory instead. `[index].cache =
false` turns the cache off. See the "Index cache" section of the README.

## Developer

- [Build and tests](dev/test.md)
- [Design filelist cache](dev/files.md)
- [Indexing philosophy](dev/indexing.md) — including project-root resolution and shard storage
- [Startup performance](dev/startup-perf.md)
- [Edit-path performance](dev/edit-perf.md)
