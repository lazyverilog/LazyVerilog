# Completion

**LSP:** `textDocument/completion`

Context-aware completion for SystemVerilog. Suggestions vary by what the cursor is inside — keyword positions, identifier expressions, named port lists, member access, package scope, macro invocations, and include directives are handled separately.

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

### Identifiers

Triggered in ordinary identifier positions.

Suggests symbols visible from the current syntax context. Module/interface names
are available where an instantiation can start, but they are hidden in procedural
expression contexts such as `state = |`.

Block-local declarations are visible only inside their enclosing lexical range.
Local/current-file symbols rank ahead of symbols from the design filelist.
When completing the right-hand side of an assignment, enum literals and signals
with a compatible syntactic type are ranked higher.

Package-owned classes, typedefs, enum literals, values, functions, and tasks are
not flattened into ordinary identifier completion merely because their package
source is listed in `vcode.f`. They become ordinary identifier candidates only
when visible through package syntax:

```systemverilog
import uvm_pkg::*;          // wildcard import: package members are visible
import uvm_pkg::uvm_object; // explicit import: only uvm_object is visible
uvm_pkg::                   // explicit package-scope completion
```

### Named port connections

Triggered by `.` inside a module instantiation argument list.

Suggests only ports from the instantiated module that are not already connected.
If every port is connected, no named-port items are returned. Items insert as
snippets, for example `.clk(${1:clk})`, and the insertion text omits the already
typed dot.

```systemverilog
m_fifo u_fifo (
    .i_clk(i_clk),
    .i_rst_n(i_rst_n),
    .i_data(|)  // typing . here lists remaining unconnected ports
);
```

Requires the module definition to be reachable from the current file or the design filelist.

### Member access

Triggered by `.` after an identifier (outside a port list).

Suggests fields and methods of a class, inherited class members, fields of named `typedef struct` types, and ports/signals/modports of modules or interfaces.

```systemverilog
my_obj.          // lists fields and methods of my_obj's class
pkt_struct.      // lists fields from a named typedef struct
bus_if_inst.     // lists interface signals and modports
```

The member-access trigger itself is detected from the SyntaxTree dot token and its immediately-adjacent left-hand name. Type resolution is still syntactic and best-effort; complex expressions and long typedef chains may not resolve.

Member definitions can come from the current file or from files listed in
`vcode.f`.

### Package scope

Triggered by `::`.

Suggests symbol names exported by the named package only. Completion is backed by
the slang `SyntaxTree` index: classes, typedefs, enum literals, parameters,
variables, functions, and tasks are tagged with their declaring package while the
package tree is walked, then `pkg::` filters to that package membership. There is
no fallback that returns unrelated global classes or typedefs.

```systemverilog
package pkg_a;
    typedef enum { A_IDLE, A_DONE } a_state_t;
    class a_object;
    endclass
endpackage

package pkg_b;
    class b_object;
    endclass
endpackage

pkg_a::   // lists a_state_t, A_IDLE, A_DONE, a_object; not b_object
```

Include-heavy packages such as UVM work the same way when the package source is
listed in `vcode.f` and its headers are reachable through `+incdir+` entries.
Package-scope completion remains available after go-to-definition opens the
package source and the user returns to the original buffer.

### Parameter connections

Triggered by `.` inside `#(...)`.

Suggests unresolved parameter names from the instantiated module. The declared
type and default value are shown in the detail field (e.g., `int = 256`). If all
parameters are assigned, no parameter items are returned.

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

Suggests macro names visible from the current file's preprocessing context,
including macros from headers included by the current file. Extra-file macros are
not flattened into unrelated buffers. Function-like macros insert a snippet with
placeholders for each parameter; object-like macros insert the name only. slang
built-in macros with no real source location, such as `SV_COV_ERROR`, are hidden
from completion, hover, and go-to-definition.

### General identifiers

Triggered on any identifier position.

Suggests visible identifiers from the current SyntaxTree scope plus appropriate global design symbols such as package names and module/interface names in instantiation-capable contexts. Package members from filelist libraries are filtered by visible `import` declarations, module ports and declarations from unrelated modules are not flattened into the current scope, and block-local declarations are visible only inside their enclosing block range. Structural snippets (`module`, `class`, `always_ff`, `function`, etc.) are also included.

Snippet suggestions are context-aware. For example, `always_comb` is offered at
module/interface item level, but not inside a class or covergroup body.

---

## Ranking

Items are sorted by a combined score:

- **Prefix match**: exact case-insensitive prefix match scores highest; fuzzy match (all typed characters appear in order) scores lower; no match is excluded
- **Scope**: symbols declared in the current file score higher than symbols from the design filelist
- **Type hints**: enum literals and same-type signals score higher in assignment contexts

---

## Degradation

If the file has syntax errors, context detection falls back to the `Identifier`
context. Visibility filters still apply where possible.

---

## Known Gaps

- **Typedef chains**: only one step of typedef resolution. Multi-step chains do not resolve.
- **Parameterized classes**: `my_class #(T)` member access is not supported.
- **Hierarchical references**: `top.dut.signal` resolves only one `.` hop.
- **Anonymous structs**: fields of inline `struct { ... }` types are not indexed. Named `typedef struct` types are indexed.
