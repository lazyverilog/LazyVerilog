# Completion

Suggestions depend on what the cursor is inside. Type-aware ranking puts symbols from the current file
ahead of the filelist, and enum literals and matching signals first on the right of an assignment.

| Where | Trigger | Suggests |
|-------|---------|----------|
| Module, procedural, class, covergroup body | any identifier position | Keywords valid there, plus snippets such as `always_ff` |
| Expressions | any identifier position | Visible symbols. Module names appear only where an instantiation can start |
| Class methods | any identifier position | Also the class's members and every member it inherits through `extends`, own and nearest base first |
| Instance ports | `.` inside `(...)` | Ports of the instantiated module not yet connected |
| Instance parameters | `.` inside `#(...)` | Parameters not yet assigned, with type and default |
| Members | `.` after a name | Class fields and methods, struct fields, module or interface ports and modports |
| Package scope | `::` | Members of that package only |
| Macros | `` ` `` | Macros visible in the current file, and every macro defined in its project. Function-like ones insert a snippet |
| Includes | manual completion (`<C-x><C-o>`, `Ctrl+Space`) inside `` `include "..." `` | `.svh` and `.vh` files from the filelist |

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
- A handle typed by a type parameter (`REQ req;` in `uvm_driver #(type REQ = ...)`) is resolved by following
  each `extends X #(...)` from the class you are in, or from a handle's own `X #(...)` type, up to the class
  that declares it: at most 8 steps, arguments by position or name, defaults otherwise. An `extends` or a
  handle may name a specialization typedef (`typedef X #(item) x_t;`); a package qualifier (`pa::x_t`) picks
  that package's declaration. An argument is taken as a type name: expressions and nested `#(...)` in it are
  not followed. A chain longer than 8 steps, or one through a base the index cannot follow (a typedef declared
  inside a class), offers nothing rather than the parameter's default; the default is used only when no
  argument appears anywhere in the chain.
- Inherited members on a bare identifier need the file's last parse to have taken 50 ms or less. In a slower
  file the class the cursor is in is unknown, so only the names the unparsed text shows are offered.
- Hierarchical references (`top.dut.signal`) resolve one `.` hop.
- Fields of anonymous `struct { ... }` types are not indexed. Named `typedef struct` types are.
- With syntax errors in the file, keyword context falls back to plain identifiers.
