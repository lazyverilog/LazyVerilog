# Design & Filelist

Cross-file features (go to definition, references, inlay hints, workspace symbols, AutoInst,
completion) need your source files indexed. Point `lazyverilog.toml` at a filelist:

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]
```

| Option | Type | Description |
|--------|------|-------------|
| `vcode` | string | Filelist path, relative to `lazyverilog.toml` |
| `define` | string[] | Preprocessor defines for every design file |

## Filelist format

One source file per line, with paths relative to the filelist.

```text
rtl/m_alu.sv
rtl/m_adder.sv
-f ../shared/shared.vc
+incdir+rtl/include
vendor/uvm/src/uvm_pkg.sv
```

| Syntax | Effect |
|--------|--------|
| `// ...` or `# ...` | Comment |
| `-f <filelist>` | Load a nested filelist, its paths relative to itself |
| `+incdir+<dir>` | Add an include directory. `+incdir+<a>+<b>` adds several |
| other `+<option>`, `-<flag>` | Ignored |

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
