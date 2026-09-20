# Completion

Suggestions depend on what the cursor is inside. Type-aware ranking puts symbols from the current file
ahead of the filelist, and enum literals and matching signals first on the right of an assignment.

| Where | Trigger | Suggests |
|-------|---------|----------|
| Module, procedural, class, covergroup body | any identifier position | Keywords valid there, plus snippets such as `always_ff` |
| Expressions | any identifier position | Visible symbols. Module names appear only where an instantiation can start |
| Instance ports | `.` inside `(...)` | Ports of the instantiated module not yet connected |
| Instance parameters | `.` inside `#(...)` | Parameters not yet assigned, with type and default |
| Members | `.` after a name | Class fields and methods, struct fields, module or interface ports and modports |
| Package scope | `::` | Members of that package only |
| Macros | `` ` `` | Macros visible in the current file. Function-like ones insert a snippet |
| Includes | `"` after `` `include `` | `.svh` and `.vh` files from the filelist |

```systemverilog
m_fifo u_fifo (
    .i_clk(i_clk),
    .i_data(|)   // typing `.` here lists the remaining ports
);
```

## Package members

Package members are not offered as plain identifiers just because the package is in the filelist. They
appear once visible:

```systemverilog
import uvm_pkg::*;          // every member is visible
import uvm_pkg::uvm_object; // only uvm_object is visible
uvm_pkg::                   // scope completion lists the package
```

## Known gaps

- Typedef chains resolve one step only.
- Members of parameterized classes (`my_class #(T)`) are not offered.
- Hierarchical references (`top.dut.signal`) resolve one `.` hop.
- Fields of anonymous `struct { ... }` types are not indexed. Named `typedef struct` types are.
- With syntax errors in the file, keyword context falls back to plain identifiers.
