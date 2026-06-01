# LazyVerilog Docs

This directory contains user-facing documentation for the current C++
implementation of LazyVerilog.

## Files

- [Features](features.md): index of user-visible features.
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

## Developer

- [Build and tests](dev/test.md)
- [Design filelist cache](dev/files.md)
