0. rewrite README.md.
- Current README.md is too hard for beginners. Must be rewritten user-friendly. Even for people who don't know what LSP is.
- Current README.md does not look attractive enough to choose Lazyverilog over other LSPs.
1. Provide option for positional argument when doing AutoInst. Current implementation of AutoInst only provides named argument.
2. Update TOML file value sanity check.
3. Implement formatter option 'max_lengh_per_line'.

Example:
```systemverilog
// Input:
assign y = a + b + c + d + e;

// Formatted:
assign y = a + b + c
         + d + e;
```

Implementing feature needs careful DAG change of formatter.

4. Renew the docs.
5. Renew `lazyverilog.toml`. Look for options that is implemented in the source code but not listed in `lazyverilog.toml` and vice versa.
6. Add VSCODE client Support. Current supports only NeoVIM.
