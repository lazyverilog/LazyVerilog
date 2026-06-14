# Binary Installation

The Neovim plugin resolves the server binary in the following order:

1. **User-specified `cmd`** — if `setup({ cmd = "..." })` or `setup({ cmd = {...} })` is set and the executable exists, it is used directly.
2. **`lazyverilog-lsp` in PATH** — if the binary is found on `$PATH`, it is used directly.
3. **Managed binary** — if a previously auto-installed binary exists at `$XDG_DATA_HOME/lazyverilog/bin/lazyverilog-lsp` (or `lazyverilog-lsp.exe` on Windows), it is used directly.
4. **Auto-install** — if none of the above resolve, the plugin downloads the release binary for the detected platform.

No download is attempted when a binary is already available via steps 1–3.

---

## Auto-Install

On first use, the plugin downloads the server binary from the GitHub release matching `lua/lazyverilog/version.lua`.

The detected platform maps as:

| OS | Architecture | Asset |
|----|--------------|-------|
| Linux | x86_64 | `linux-x64` |
| Linux | aarch64 / arm64 | `linux-arm64` |
| macOS | x86_64 | `darwin-x64` |
| macOS | arm64 | `darwin-arm64` |
| Windows | x86_64 / amd64 | `windows-x64` |

---

## Static Fallback (Linux only)

After downloading a Linux binary, the plugin runs `ldd` to verify all shared library dependencies are satisfied. If any dependency is reported as `not found` (e.g. due to a glibc version mismatch), the plugin automatically retries with the static build:

| Primary asset | Static fallback |
|---------------|-----------------|
| `linux-x64` | `linux-x64-static` |
| `linux-arm64` | `linux-arm64-static` |

Static builds embed `libgcc` and `libstdc++` and have no dependency on the system C++ runtime. They require glibc ≥ 2.35 (Ubuntu 22.04 baseline).

macOS and Windows do not have static fallback builds. Download failures on those platforms are reported as errors.
