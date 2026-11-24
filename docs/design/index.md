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
| `-f <filelist>` | Load a nested filelist, its paths relative to itself. `-F`, `-f=<filelist>` and `-f<filelist>` are the same |
| `-v <file>` | A library file. It is indexed like any source file |
| `+incdir+<dir>` | Add an include directory. `+incdir+<a>+<b>` adds several |
| `-y <dir>`, `-l <file>`, `-top <name>`, `-o`, `-work`, ... | Ignored, together with their argument |
| `+define+<NAME>` | Ignored. Put defines in `[design].define` |
| others | Comments and any other `+<option>` or `-<flag>` are ignored |

Paths are relative to the filelist that names them, for `-f` and `-F` alike. A line ending in `\`
continues on the next line. Quote a path that contains spaces (`"my dir/top.sv"` or `'...'`).
`//` and `#` start a comment anywhere on the line, even inside quotes.

`+incdir+` entries are search paths for `` `include ``, not source files.

**Environment variables.** `$MY_RTL_PATH` and `${MY_RTL_PATH}` are replaced by the variable's value,
in filelists and in `vcode`. The editor must start with the variable set. A variable that is not set
is left as written.

**Wildcards.** Source paths, `-v`, `-f` and `+incdir+` entries may use shell-style wildcards,
expanded when the filelist is read:

```text
rtl/*.sv                  # every .sv in rtl/
rtl/**/*.sv               # ... and in every directory below it
ip/*/rtl/top_?.sv         # * and ? match within one path segment
rtl/u[0-3].sv             # [abc], [a-z], [!a-z]
-f blocks/*/files.f       # every matching nested filelist
+incdir+ip/*/include      # every matching directory
```

`**` matches zero or more directories. Matches are sorted by path. A wildcard source or `-v` entry
keeps only `.sv` and `.v` files, so `rtl/*` and `rtl/**` skip READMEs and headers (headers are
reached through `` `include ``); `-f` and `+incdir+` patterns are not filtered. A wildcard does not match a
leading `.` unless the pattern starts with one, and `**` skips hidden directories, does not follow
directory symlinks or junctions, and never walks one directory twice. On Windows matching ignores case. A pattern that matches nothing adds nothing, without a
warning. Only the entry as written is a pattern: the filelist's own directory, and environment
variables in the path segments before the first wildcard, are taken literally. A pattern ending in
`/` matches directories only. If a pattern matches nothing but a file is literally named that way
(`top[0].sv`), that file is used. Matches are found when the filelist
is loaded; a file created later is picked up when a `.f`/`.vf` filelist of the project or
`lazyverilog.toml` changes on disk.

**UVM library.** List the package file and put the headers on an include path. `uvm_pkg.sv` is
indexed, and its `` `include `` lines resolve through the include directory, so you do not list every
`.svh`.

## No filelist yet

Without a `lazyverilog.toml`, the server indexes the workspace folder or version-control checkout
instead. See [Without a lazyverilog.toml](../configuration.md#without-a-lazyverilog-toml).

| | Filelist (`vcode`) | Implicit project |
|---|---|---|
| Files | Exactly the listed ones | Every `.sv` and `.v` under the folder, minus tool output |
| Include directories | `+incdir+` entries | Every directory holding a header, sorted |
| Defines | `define` | None |
| Two modules with one name | Only if you list both | Both indexed; rename refuses the name |
| Whole-design compilation | `[compilation]` | Off |
| Shards | `.cache/lazyverilog` in the project | The user cache, never the tree |

Write a filelist once go to definition lands in the wrong copy of a module, or an `` `ifdef `` hides
code you work on.
