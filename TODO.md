# TODO LIST
## 0. rewrite README.md.
- Current README.md is too hard for beginners. Must be rewritten user-friendly. Even for people who don't know what LSP is.
- Current README.md does not look attractive enough to choose Lazyverilog over other LSPs.
## 1. Cache the initial parse AST in directory .lazyverilog for faster initial start-up time for large project.
- This needs running `stats` for each file in .f file. -> Is this really better than reparsing all files?

## 2. Provide option for positional argument when doing AutoInst. Current implementation of AutoInst only provides named argument.
## 3. Update TOML file value sanity check.
## 4. Implement formatter option 'max_lengh_per_line'.

Example:
```systemverilog
// Input:
assign y = a + b + c + d + e;

// Formatted:
assign y = a + b + c
         + d + e;
```

Implementing feature needs careful DAG change of formatter.

## 5. Renew the docs.
## 6. Renew `lazyverilog.toml`.
Look for options that is implemented in the source code but not listed in `lazyverilog.toml` and vice versa.
## 7. Add VSCODE client Support. Current supports only NeoVIM.
## 8. Add command :Diagram -> draws a diagram in ascii art.
- This will need slang AST of current buffer -> already available and exposed by analyzer.  
- Need to think how to pretty place blocks and wires.  

example demo:
```text
                    +----------------------+
                    |      cpu_top         |
                    |                      |
i_clk ------------->|                      |
i_rst_n ----------->|                      |
                    |                      |
                    |   +------------+     |
i_instr ----------->|-->| Decoder    |-----+
                    |   +------------+     |
                    |                      |
                    |   +------------+     |
                    |   | ALU        |<----+
                    |   +------------+     |
                    |                      |
                    |   +------------+     |
                    |   | Register   |-----+-----> o_data
                    |   | File       |     |
                    |   +------------+     |
                    +----------------------+

```
