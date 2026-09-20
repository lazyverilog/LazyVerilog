# Contributing

Contributions are welcome.

Before sending a pull request:

1. Build the project.
2. Run the relevant tests.
3. Add or update tests for formatter, lint, LSP, or automation changes.
4. Update documentation for user-visible behavior or configuration changes.

Recommended checks:

```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

::: warning
Formatter changes should include focused cases in `tests/test_formatter.cpp` and preserve idempotency and safe-mode guarantees.
:::

## Build

Requirements: CMake and a C++20-capable compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc) --target lazyverilog-lsp
```

## Documentation

These pages are built from the `docs/` directory of the
[LazyVerilog repository](https://github.com/lazyverilog/LazyVerilog) with VitePress and the
[Catppuccin theme](https://github.com/catppuccin/vitepress). Edit a page with the "Edit this page
on GitHub" link at its foot.

User-facing pages live outside `docs/dev/`, `docs/releases/`, and `docs/i18n/`; those three
directories are not published to this site. Add every new page to the sidebar in
`docs/.vitepress/site.json`; the site's build check fails when a page is missing from it.

## License

LazyVerilog is released under the MIT License. See the
[`LICENSE`](https://github.com/lazyverilog/LazyVerilog/blob/main/LICENSE) file.
