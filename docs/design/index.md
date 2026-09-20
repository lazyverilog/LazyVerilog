# Design & Filelist

Cross-file features (go to definition, references, inlay hints, workspace symbols, AutoInst,
completion) need your source files indexed. Point `lazyverilog.toml` at a filelist:

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]
```

| Section | Option | Type | Description |
|---------|--------|------|-------------|
| `design` | `vcode` | string | Filelist path, relative to `lazyverilog.toml` |
| `design` | `define` | string[] | Preprocessor defines for every design file |

## Filelist format

One source file per line, with paths relative to the filelist.

```text
rtl/alu.sv
rtl/adder.sv
-f ../my_rtl_list.f
+incdir+rtl/include

# environment variable
${MY_RTL_PATH}/top.sv
+incdir+${MY_RTL_PATH}/include

# UVM library
+incdir+uvm/src
uvm/src/uvm_pkg.sv
```

| Line | Effect |
|------|--------|
| `rtl.sv` | A source file to index |
| `-f <filelist>` | Load a nested filelist, its paths relative to itself |
| `+incdir+<dir>` | Add an include directory. `+incdir+<a>+<b>` adds several |
| others | Comments (`//`, `#`) and any other `+<option>` or `-<flag>` are ignored |

`+incdir+` entries are search paths for `` `include ``, not source files.

**Environment variables.** `$MY_RTL_PATH` and `${MY_RTL_PATH}` are replaced by the variable's value,
in filelists and in `vcode`. The editor must start with the variable set. A variable that is not set
is left as written.

**UVM library.** List the package file and put the headers on an include path. `uvm_pkg.sv` is
indexed, and its `` `include `` lines resolve through the include directory, so you do not list every
`.svh`.
