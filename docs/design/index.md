# Design & Filelist

Design-wide features (go-to-definition, find references, inlay hints, workspace symbols, AutoInst) require the full module set to be indexed. Provide a filelist to load the design.

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

One file path per line. Paths are relative to the filelist file's directory. Lines starting with `//` are comments.

```
// filelist for the alu design
rtl/m_alu.sv
rtl/m_adder.sv
rtl/m_multiplier.sv
```
