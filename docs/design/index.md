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
uvm/src/uvm_pkg.sv
```

| Line | Effect |
|------|--------|
| `rtl.sv` | A source file to index |
| `-f <filelist>` | Load a nested filelist, its paths relative to itself |
| `+incdir+<dir>` | Add an include directory. `+incdir+<a>+<b>` adds several |
| others | Comments (`//`, `#`) and any other `+<option>` or `-<flag>` are ignored |

`+incdir+` entries are search paths for `` `include ``, not source files. `$VAR` and `${VAR}` are
expanded when the variable is defined.

## Libraries with many headers

For a library such as UVM, list the package file and put the headers on an include path:

```text
+incdir+./uvm-core/src
./uvm-core/src/uvm_pkg.sv
```

`uvm_pkg.sv` is indexed, and its `` `include `` lines resolve through the include directory, so you do
not list every `.svh`.
