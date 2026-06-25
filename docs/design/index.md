# Design & Filelist

Design-wide features (go-to-definition, find references, inlay hints, workspace symbols, AutoInst, completion) require the relevant source files to be indexed. Provide a filelist to load the design.

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]
```

| Option | Type | Description |
|--------|------|-------------|
| `vcode` | string | Path to filelist (`.f`) relative to the `lazyverilog.toml` file |
| `define` | string[] | Preprocessor defines passed to the parser for all design files |

## Filelist format

One source file path per line. Paths are relative to the filelist's directory.
Nested filelists are supported with `-f`; nested relative paths are resolved
relative to the parent filelist that contains the `-f` entry.

```text
rtl/m_alu.sv
rtl/m_adder.sv
-f ../shared/shared.vc
+incdir+rtl/include
vendor/uvm/src/uvm_pkg.sv
```

Parsing rules:

| Syntax | Effect |
|--------|--------|
| `// ...` | Line comment |
| `# ...` | Line comment |
| `-f <filelist>` | Load a nested filelist; relative paths are resolved from the parent filelist |
| `+incdir+<dir>` | Add include search directory; `<dir>` is relative to the `.f` file |
| `+incdir+<dir_a>+<dir_b>` | Add multiple include search directories |
| `+<option>` | Other compiler options are silently ignored |
| `-<flag>` | Compiler flags are silently ignored |

`+incdir+` entries are **not** parsed as source files. They are passed to slang's include resolver so explicit source files can resolve `` `include "..." `` directives.

Environment variables in filelist, source, and include-directory paths are expanded
in `$VAR` and `${VAR}` form when the variable is defined. Undefined variables are
left unchanged.

## Include-heavy libraries

For libraries such as UVM, list the package/source file and use `+incdir+` for headers:

```text
# demo/vcode.f
+incdir+./uvm-core/src
./uvm-core/src/uvm_pkg.sv
```

With that setup, `uvm_pkg.sv` is the explicit indexed source file, and slang resolves lines such as:

```systemverilog
`include "base/uvm_base.svh"
`include "comps/uvm_comps.svh"
```

through the configured include directory. This avoids parsing each UVM `.svh` as a separate filelist source while still allowing the package parse to discover classes, typedefs, methods, and macros for completion.
