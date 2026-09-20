# Comparison

LazyVerilog compared with the two most commonly used open-source SystemVerilog language servers:
[Verible](https://github.com/chipsalliance/verible/blob/master/verible/verilog/tools/ls/README.md)
and [svlangserver](https://github.com/imc-trading/svlangserver).

| | LazyVerilog | Verible LS | svlangserver |
|---|---|---|---|
| UVM, classes, packages | ✅ | ⚠️ | ❌ |
| Runtime | C++ | C++ | Node.js |
| Parser | slang | Verible | own indexer |
| Diagnostics | parse, lint, semantic | parse, lint | Verilator |
| Formatting | ✅ | ✅ | via Verible |
| Go to definition | ✅ | ✅ | ✅ |
| Find references | ✅ | ✅ | ❌ |
| Rename | ✅ | ❌ | ❌ |
| Hover | ✅ | ⚠️ | ✅ |
| Completion | ✅ | ❌ | ✅ |
| Signature help | ✅ | ❌ | ✅ |
| Inlay hints | ✅ | ❌ | ❌ |
| Symbols | ✅ | ✅ | ✅ |
| Lint autofix | ❌ | ✅ | ❌ |
| RTL code generation | ✅ | ❌ | ❌ |
| RTL hierarchy view | ✅ | ❌ | ✅ |
| Interface and connect | ✅ | ❌ | ❌ |

⚠️ means partial or experimental.
