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
