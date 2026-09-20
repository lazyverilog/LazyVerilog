# Installation

Pick your editor. Either plugin downloads the release binary and starts the server for Verilog and
SystemVerilog files.

| Editor | Guide |
|--------|-------|
| Neovim | [Install for Neovim](neovim.md) |
| VS Code | [Install for VS Code](vscode.md) |

Then [add a `lazyverilog.toml`](../usage/index.md) to your project.

## Which server binary is used

The editor picks the first one it finds:

1. **Explicit path**: `cmd` in Neovim, `lazyverilog.serverPath` in VS Code. Never replaced.
2. **`lazyverilog-lsp` on `PATH`**: yours to manage, never checked or updated.
3. **Managed download**: fetched into the editor's storage and replaced when the plugin updates.
   Do not put custom builds here.

The managed download supports `linux-x64`, `linux-arm64`, `darwin-x64`, `darwin-arm64`, and
`windows-x64`. On Linux, if the binary needs libraries the system lacks, the editor retries with the
`-static` build (glibc 2.35 or newer).

## Build from source

Needs CMake and a C++20 compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc) --target lazyverilog-lsp
```

Point your editor at `build/lazyverilog-lsp` with the explicit path above.
