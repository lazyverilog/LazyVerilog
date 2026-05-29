# LazyVerilog 1.0

A fast SystemVerilog language server for everyday RTL editing, formatting, linting, navigation, and code generation.

LazyVerilog is a C++ Language Server Protocol (LSP) server with a companion Neovim plugin. It is built for practical hardware-design workflows: format-on-save, diagnostics, RTL tree navigation, auto-instantiation, auto-wire/auto-connect helpers, and editor features that understand real SystemVerilog projects.

## Demo

<!-- TODO: add demo GIF. -->

## Screenshots

### Formatting

<!-- TODO: add screenshot. -->

### Lint diagnostics

<!-- TODO: add screenshot. -->

### RTL tree

<!-- TODO: add screenshot. -->

### Go to definition / references / rename

<!-- TODO: add screenshot. -->

### Auto-instantiation

<!-- TODO: add screenshot. -->

### Auto-wire / connect helpers

<!-- TODO: add screenshot. -->

### Inlay hints / completion / hover

<!-- TODO: add screenshot. -->

## Why LazyVerilog?

SystemVerilog editor tooling usually falls into two categories:

- **Standalone developer tools**, such as [Verible](https://github.com/chipsalliance/verible). Verible is excellent when you want a mature parser-oriented tool suite with command-line formatting, style linting, syntax tools, and a language server. It is a broad ecosystem for SystemVerilog developer tools.
- **Small LSP-first servers**, such as [svls](https://github.com/dalance/svls). svls is lightweight and easy to install, with linter integration through svlint.

LazyVerilog is focused on a different point in the design space: an RTL-authoring assistant that combines LSP features, formatter policy, project-aware source analysis, and code-generation commands in one workflow.

| Area | LazyVerilog | Verible | svls |
| --- | --- | --- | --- |
| Primary focus | RTL editing workflow and automation through LSP | SystemVerilog developer tool suite | Lightweight SystemVerilog LSP |
| Formatter | Built-in configurable formatter with safe-mode content checks | Mature command-line formatter and LSP formatting | Not the main focus |
| Linting | Built-in RTL/style lint rules, configurable in `lazyverilog.toml` | Mature style linter with extensive rule infrastructure | svlint-based linting |
| Navigation | Definition, references, rename, hover, completion, signature help, workspace symbols | LSP support for editor integration | LSP support |
| RTL automation | Auto-instantiation, auto-arg, auto-function, auto-wire, auto-FF, connect/interface helpers | Mostly separate developer-tool commands | Minimal |
| Editor integration | Neovim plugin included; generic LSP server also works with other LSP clients | Generic LSP server and command-line tools | Generic LSP server |
| Configuration | One project-local `lazyverilog.toml` for design, lint, formatting, hints, and automation | Tool-specific flags/configs | `.svls.toml` and `.svlint.toml` |

Choose LazyVerilog if you want an editor-centered RTL workflow with formatting, diagnostics, navigation, and repetitive-code generation in a single server. Choose Verible when you need the broadest standalone SystemVerilog tool suite. Choose svls when you want a small Rust-based language server with svlint integration.

## Installation

### Neovim plugin

Using `lazy.nvim`:

```lua
{
  "hxxdev/lazyverilog",
  -- The repository contains large RTL test corpora as submodules; users do not
  -- need them for the editor plugin.
  submodules = false,
  ft = { "systemverilog", "verilog" },
  config = function()
    require("lazyverilog").setup()
  end,
}
```

On startup, the plugin resolves the server command in this order:

1. `setup({ cmd = { ... } })` — a full command array. Use this when you want
   to pass the executable and all arguments explicitly.
2. `setup({ cmd = "/path/to/lazyverilog-lsp" })` — an executable path plus
   optional `cmd_args`.
3. `lazyverilog-lsp` on `$PATH`.
4. The plugin-managed binary at
   `stdpath("data") .. "/lazyverilog/bin/lazyverilog-lsp"`.
5. If no executable is found, the plugin downloads the release binary with
   `curl`, stores it in the plugin-managed path, runs `chmod +x`, and starts it.

The auto-download path currently supports Linux and macOS on `x86_64`/`amd64`
and `aarch64`/`arm64`.

Examples:

```lua
-- Use auto-install / PATH / managed-binary resolution.
require("lazyverilog").setup()

-- Use a local build explicitly.
require("lazyverilog").setup({
  cmd = "/home/me/src/lazyverilog/build/lazyverilog-lsp",
})

-- Use a full command array.
require("lazyverilog").setup({
  cmd = { "/home/me/bin/lazyverilog-lsp" },
})
```

### Manual binary installation

Download `lazyverilog-lsp` for your platform from GitHub Releases, make it
executable, and place it on `$PATH`:

```bash
chmod +x lazyverilog-lsp
mkdir -p ~/.local/bin
mv lazyverilog-lsp ~/.local/bin/
```

Then confirm your editor can find it:

```bash
lazyverilog-lsp
```

### From source

Build the server locally and either place the generated binary on `$PATH` or
configure the Neovim plugin with `setup({ cmd = "..." })`:

```bash
cmake -B build
cmake --build build -j$(nproc)
./build/lazyverilog-lsp
```

## Build

Requirements:

- CMake
- A C++20-capable compiler
- Git

Build all targets:

```bash
cmake -B build
cmake --build build -j$(nproc)
```

Run tests:

```bash
ctest --test-dir build
```

Useful developer targets:

```bash
./build/lazyverilog-tests "[formatter]"
./build/lazyverilog-fmt path/to/file.sv
```

## Configuration

LazyVerilog reads `lazyverilog.toml` from the project root. If the server opens a file in a subdirectory, it walks upward until it finds the nearest `lazyverilog.toml`.

The configuration file controls design inputs, semantic compilation, lint rules, formatter policy, RTL tree display, inlay hints, and automation helpers.

Minimal example:

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]

[format]
enable_format_on_save = true
indent_size = 4
safe_mode = true

[lint]
enable = true
```

See:

- [`lazyverilog.toml`](lazyverilog.toml) for a complete example configuration.
- [`docs/formatter/options.md`](docs/formatter/options.md) for formatter options.
- [`docs/formatter/macros.md`](docs/formatter/macros.md) for macro formatting policy.
- [`docs/diagnostics/background-compilation.md`](docs/diagnostics/background-compilation.md) for background semantic diagnostics.

## Contributing

Contributions are welcome.

Before sending a pull request:

1. Build the project.
2. Run the relevant tests.
3. Add or update tests for formatter, lint, LSP, or automation changes.
4. Update documentation when user-visible behavior or configuration changes.

Recommended checks:

```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

Formatter changes should include focused cases in `tests/test_formatter.cpp` and should preserve the formatter's idempotency and safe-mode guarantees.

## License

LazyVerilog is released under the MIT License. See [`LICENSE`](LICENSE).
