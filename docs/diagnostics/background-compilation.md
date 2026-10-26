# Background compilation

LazyVerilog reports diagnostics in two ways:

1. **Fast diagnostics** on every edit: syntax, preprocessor, and lint.
2. **Background compilation**, off by default: a full slang elaboration that adds semantic diagnostics
   such as unknown modules, width mismatches, and type errors.

```toml
[compilation]
background_compilation = true
```

It can be slow on a weak machine, so enable it where you have the CPU and memory to spare.

## How it behaves

- It starts 1.5 seconds after you stop typing, and only the newest edit is compiled.
- It compiles the files in `[design].vcode` plus your open buffers, with your unsaved text, using the
  project's own `define` and `+incdir+` entries.
- Each open project is compiled on its own, one after another, so memory stays at one project's worth.
  The setting is read per project.
- Results appear for open files only. Requests such as hover never wait for it.
- The worker runs at low priority on Linux. Worker count and the 1.5 second delay are fixed.

## Warnings from another project's macros

A warning that slang raises while expanding a macro written in another project is not shown. One
example is UVM's `` `uvm_field_int `` (implicit conversions inside UVM code). "Another project" means
the macro's file belongs to a different `lazyverilog.toml` than the file the warning lands in, or to
none at all. A warning on your own macro argument is dropped only when it also points into the
library's body, such as an assignment to a variable the macro declares. A warning wholly inside your
argument, such as the truncation in `` `LIB_DO(n = w) ``, is still shown.

- Errors from such macros are always shown.
- A sibling project's macros count as foreign. A library vendored under your own `lazyverilog.toml`,
  such as a copy of UVM inside the project tree, does not, and its warnings are still shown.
  Give the library its own `lazyverilog.toml`, or keep it outside the project, to treat it as foreign.
