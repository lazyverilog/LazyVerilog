# Comparison

LazyVerilog compared with the two most commonly used open-source SystemVerilog language servers:
[Verible](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
and [svlangserver](https://github.com/imc-trading/svlangserver).

| | LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| **UVM / class / package support** | ✅ | ⚠️ | ❌ own docs: "doesn't understand most verification specific concepts (e.g. classes)" |
| **Performance** | ✅ native C++ | ✅ native C++ | ❌ Node.js runtime |
| Parser | [slang](https://github.com/MikePopoloski/slang) | Verible's own SystemVerilog parser | own lightweight indexer |
| Diagnostics | ✅ parse + customizable lint rules + optional semantic diagnostics | syntax errors + Verible's lint rule set | delegates to Verilator |
| Formatting | built in | built in | delegates to `verible-verilog-format` |
| Go to definition | ✅ | ✅ | ✅ |
| Find references | ✅ | ✅ | ❌ |
| Rename symbol | ✅ | ❌ (listed as planned) | ❌ |
| Hover | ✅ | ⚠️ (experimental) | ✅ |
| Completion | ✅ | ❌ | ✅ |
| Signature help | ✅ | ❌ | ✅ |
| Inlay hints | ✅ port directions on instances | ❌ | ❌ |
| Document / workspace symbols | ✅ | ✅ document outline | ✅ |
| Lint autofix code actions | ❌ | ✅ | ❌ |
| RTL code generation | ✅ AutoInst / AutoWire / AutoArg / AutoFunc / AutoFF | ❌ | ❌ |
| RTL hierarchy view | ✅ | ❌ | ✅ |
| Interface / instance connect tooling | ✅ `:Interface`, `:Connect` | ❌ | ❌ |
