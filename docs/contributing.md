# Contributing

Contributions are welcome. Before opening a pull request:

1. Build and run the tests.
2. Add or update tests for formatter, lint, LSP, or automation changes.
3. Update the docs for user-visible changes.

```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

::: warning
Formatter changes need focused cases in `tests/test_formatter.cpp` and must stay idempotent.
:::

## Documentation

These pages are the `docs/` directory of the
[repository](https://github.com/lazyverilog/LazyVerilog), built with VitePress. Use "Edit this page on
GitHub" at the foot of a page to change it. Add a new page to the sidebar in
`docs/.vitepress/site.json`, or the build check fails.

## License

MIT. See [`LICENSE`](https://github.com/lazyverilog/LazyVerilog/blob/main/LICENSE).
