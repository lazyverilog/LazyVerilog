# Completion

**LSP:** `textDocument/completion`

Context-aware completion for SystemVerilog. Suggestions vary by what the cursor is inside — named port lists, member access, package scope, macro invocations, and general identifiers are all handled separately.

---

## Contexts


### Keywords

Triggered in general identifier positions. Keyword suggestions are narrowed by the nearest enclosing SyntaxTree node:

| SyntaxTree context | Example suggestions |
|---|---|
| module/interface item | `assign`, `always_comb`, `always_ff`, `initial`, `generate`, `endmodule` |
| procedural statement/block | `if`, `case`, `for`, `foreach`, `while`, `return`, `break`, `continue`, `begin`, `end` |
| class body | `function`, `task`, `constraint`, `covergroup`, `rand`, `randc`, `static`, `virtual`, `local`, `protected` |
| covergroup body | `coverpoint`, `cross`, `bins`, `illegal_bins`, `ignore_bins`, `option`, `type_option` |

```systemverilog
module top;
  always_comb begin
    |  // procedural keywords
  end
endmodule
```

The context decision is SyntaxTree-based; raw source text scanning is not used to classify SystemVerilog keyword context.

### Named port connections

Triggered by `.` inside a module instantiation argument list.

Suggests all ports from the instantiated module as `.port(port)` connections.

```systemverilog
m_fifo u_fifo (
    .i_clk(i_clk),
    .i_rst_n(i_rst_n),
    .i_data(|)  // typing . here lists all module ports
);
```

Requires the module definition to be reachable from the current file or the design filelist.

### Member access

Triggered by `.` after an identifier (outside a port list).

Suggests fields and methods of a class, or port names of a module or interface.

```systemverilog
my_obj.   // lists fields and methods of my_obj's class
my_module_inst.   // lists ports
```

Resolution is one step deep. Typedef chains longer than one step and parameterized types may not resolve.

### Package scope

Triggered by `::`.

Suggests symbol names exported by the named package.

```systemverilog
uvm_pkg::   // lists uvm_pkg exports
```

### Parameter connections

Triggered by `.` inside `#(...)`.

Suggests unassigned parameter names from the instantiated module. The declared type and default value are shown in the detail field (e.g., `int = 256`).

```systemverilog
m_fifo #(
    .DEPTH(|)   // typing . here lists parameters
) u_fifo (...);
```

### Include file completion

Triggered by `"` immediately after `` `include ``.

Suggests `.svh` and `.vh` files listed in the design filelist (`.f` file configured under `[design]` in `lazyverilog.toml`).
`+incdir+` entries in the design filelist are used for parser include resolution; those directories are not scanned as file-completion directories yet.

```systemverilog
`include "|   // lists .svh/.vh files from the design filelist
```

Requires a filelist configured under `[design]` in `lazyverilog.toml`. See [Design & Filelist](../design/index.md) for setup.

### Macros

Triggered by `` ` ``.

Suggests macro names defined in the current file and all files in the design filelist. If a listed source file includes headers through `+incdir+` resolution, macros discovered during that parse are also available. Function-like macros insert a snippet with placeholders for each parameter. Object-like macros insert the name only.

### General identifiers

Triggered on any identifier position.

Suggests visible identifiers from the current SyntaxTree scope plus global design symbols such as module, interface, class, typedef, and package names. Module ports and declarations from unrelated modules are not flattened into the current scope. Block-local declarations are visible only inside their enclosing block range. Structural snippets (`module`, `class`, `always_ff`, `function`, etc.) are also included.

---

## Ranking

Items are sorted by a combined score:

- **Prefix match**: exact case-insensitive prefix match scores highest; fuzzy match (all typed characters appear in order) scores lower; no match is excluded
- **Scope**: symbols declared in the current file score higher than symbols from the design filelist

---

## Degradation

If the file has syntax errors, context detection falls back to the `Identifier` context, which returns all indexed symbols and keywords.

---

## Known Gaps

- **Typedef chains**: only one step of typedef resolution. Multi-step chains do not resolve.
- **Parameterized classes**: `my_class #(T)` member access is not supported.
- **Hierarchical references**: `top.dut.signal` resolves only one `.` hop.
- **Anonymous structs**: fields of inline `struct { ... }` types are not indexed. Named `typedef struct` types are indexed.
