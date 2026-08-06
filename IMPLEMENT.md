# Task: package symbol resolution (definition, hover, references)

This document is your complete brief.
Read `CLAUDE.md` and `src/AGENTS.md` first — the architecture rules there are
binding.

## Mission

Add package-scoped symbol resolution to the LSP server:

- jump to and hover `pkg::PARAM` and `pkg::type_t`
- show parameter default values, including for files that are in the project
  filelist but not open in the editor
- make find-references from a package parameter declaration return its uses
- fix a scope defect where a package-qualified name resolves to a local
  declaration instead of into the package

All "current behaviour" statements below were measured against the current tree,
not inferred from reading code.

## Before you start

Confirm the rest of the suite is green before you start, so you have a clean
baseline to compare against.

## What already works — do not rebuild

Verified working today:

- jumping to a package name, both `import pkg::*` and the `pkg` half of `pkg::NAME`
- jumping to `pkg::my_class`
- jumping to an unqualified parameter made visible by `import pkg::PARAM`
  (only its hover *value* is missing — see F5)
- hover on a module body parameter in a file open in the editor
- jumping to and hovering a closed-file module ANSI header parameter

## Shared fixture

Most acceptance tests below use this pair. `cpu_pkg.sv` is registered through
`analyzer.set_extra_files({path})` and never opened.

```systemverilog
// cpu_pkg.sv
package cpu_pkg;
    parameter  int      WIDTH = 8;
    localparam int      DEPTH = WIDTH * 2;
    typedef logic [7:0] byte_t;
    class packet_cfg;
    endclass
endpackage
```

```systemverilog
// top.sv — opened in the editor
module top;
localparam DEPTH = 1;
import cpu_pkg::DEPTH;
logic               [cpu_pkg::WIDTH-1:0] data                               ;
logic               [DEPTH-1:0]         addr                                ;
cpu_pkg::byte_t b;
cpu_pkg::packet_cfg cfg;
endmodule
```

Line and column numbers in expectations are 0-based, matching the `Location` type.

## D1 — defect: Wrong jump

**This is a correctness bug that exists independently of the features below.**

Go-to-def on demo/top.sv DEPTH lands on folding_demo.sv line 79 `parameter DEPTH`.

This is unexpected in terms of SystemVerilog syntax.

## Features

### F1 — jump to a qualified package parameter

Cursor on `WIDTH` in `cpu_pkg::WIDTH` resolves to the `WIDTH` declaration in
`cpu_pkg.sv`. Currently returns no definition.

Must work in three placements, each covered by a test: the package is a closed
extra file; the package is an open buffer; the package is declared in the same file
as the use.

### F2 — hover a qualified package parameter

Same cursor as F1. Expected `MarkupContent.value`, written as a C++ string literal
so the nested code fence is unambiguous:

```cpp
"**WIDTH** — *parameter*\n\n---\n\n```\nint = 8\n```"
```

This is the existing hover format used elsewhere in the codebase; match it exactly.
Currently returns no hover.

### F3 — jump to a package typedef

Cursor on `byte_t` in `cpu_pkg::byte_t` resolves to the typedef in `cpu_pkg.sv`.
Currently returns no definition.

### F4 — hover a package typedef

Detail line is `logic [7:0]`. Currently returns no hover.

### F5 — parameter default value in hover

Two cases currently missing the `= value` half:

- A parameter in a **closed** project file. Hover on `cpu_pkg::WIDTH` currently
  returns no hover at all; it must return the F2 string.
- An unqualified parameter reached through an import. With
  `import cpu_pkg::DEPTH;` and no shadowing local declaration, hover on a `DEPTH`
  use currently shows `int`; it must show `int = WIDTH*2`.

### F6 — find-references from a package parameter declaration

```systemverilog
package p1;
    parameter int WIDTH = 8;   // invoke find-references here
endpackage

module top;
    logic [p1::WIDTH-1:0] data;
    logic [p1::WIDTH-1:0] more;
endmodule
```

Currently returns 1 result (the declaration). Must return 3 — the declaration plus
both uses. Scoped uses need a symbol ID of the form
`package_value::<package>::<name>`, matching the existing `symbol_canonical()`
convention in `src/syntax_index_shared.cpp`.

## Design constraints

### C1 — resolution must be keyed, not a linear scan

The obvious implementation — walking `index.values` comparing strings, across the
current index and then every project shard — is too slow for the request path,
because it runs for **every identifier that fails all earlier resolution**, and
`find_references` resolves each candidate token through that same path.

Measured cost of the naive approach on 300 package files × 40 parameters (12,000
`ValueEntry`): go-to-definition on an unresolvable identifier costs **172 µs per
call, against a 62 µs baseline without it**, growing linearly with total project
values — roughly 1 ms on a 3000-file design.

`CLAUDE.md` forbids this: *"Do not move expensive whole-project merges or
closed-file AST walks onto hot request paths."*

Build lookup tables in `SyntaxIndex` at index time. Suggested shape:

```cpp
// Key convention "package\nsymbol" is already used for package_values in
// src/syntax_index_shared.cpp — match it.
std::unordered_map<std::string, size_t> package_value_by_scoped_name;  // → values[]
std::unordered_map<std::string, size_t> package_type_by_scoped_name;   // → typedefs[]
std::unordered_map<std::string, size_t> package_class_by_scoped_name;  // → classes[]

// For the unqualified import-visible lookup, so the visibility filter walks only
// same-named candidates rather than every value in the shard.
std::unordered_map<std::string, std::vector<size_t>> package_values_by_name;
```

The existing `typedef_by_name` and `class_by_name` maps are keyed on the **bare
name and are first-wins** (`try_emplace`). They cannot correctly resolve
`pkg::type_t` when two packages declare the same type name, so do not reuse them
here — that is why the scoped-key maps above are needed.

`package_names`, `package_symbols`, and `module_by_name` already exist and are
useful. Packages are stored in `modules`, with their names also listed in
`package_names`.

### C2 — no synchronous project-index work on the edit path

Do not rebuild or publish the merged project snapshot inline from the open/change
path, and never while `map_mutex_` is held. If you find that hover or definition
reads a stale shard after an edit, fix it by invalidating and rebuilding lazily
inside `project_index_snapshot()`, or by letting the existing debounced background
publish handle it. Rebuilding a whole-project structure per keystroke is a
non-starter on large designs.

Related: `project_index_snapshot()` substitutes a freshly constructed *empty*
snapshot when its cache is null. Be careful that any invalidation you add does not
leave consumers reading an empty project index mid-indexing when stale-but-usable
data would have been better.

### C3 — respect the AST/index split

Per `CLAUDE.md`: the current open file's AST is authoritative; other and closed
files are represented by `SyntaxIndex` shards. Do not design anything that requires
a closed file to retain a full AST.

### C4 — both indexers must stay in sync

`src/syntax_index.cpp` (project/static shards) and `src/dynamic_file_index.cpp`
(live open buffers) build the same structures through separate code paths. Any new
field or map must be populated in both, or the feature will work for open files and
silently fail for closed ones, or vice versa. This duplication is pre-existing; do
not try to unify it as part of this task.

### C5 — import visibility must be honoured for unqualified names

An unqualified name may resolve to a package parameter only when an import makes it
visible: matching package, the import's `parent_scope` is empty
(compilation-unit) or matches the current module, the use line falls within the
import's `start_line`/`end_line` range, and the import is either a wildcard or names
that symbol. `ImportEntry` in `src/syntax_index.hpp` carries all of these fields.

### C6 — tests must not depend on `demo/`

`demo/` is a scratch area that carries uncommitted local edits; see the pre-existing
failure at the top of this document. Put fixtures in `tests/`, or write them to
`std::filesystem::temp_directory_path()` inside the test. Never assert against
`demo/`.

### C7 — keep the diff scoped

Every changed line should trace to a feature or constraint above. Do not refactor
adjacent code, adjust unrelated text rendering, or reformat files you touch.

## Suggested staging

Each stage should build clean and keep the suite green before moving on.

1. **Index plumbing.** Add a `default_value` field to `ValueEntry` and the
   scoped-name maps to `SyntaxIndex`. Populate in both indexers, covering package
   parameters, module ANSI header parameters, and module body parameters
   (`ParameterDeclarationStatementSyntax`).
   *Verify:* a test in `tests/test_syntax_index.cpp` asserting the maps and
   `default_value` are populated for a package and a module.

2. **D1 + F1 + F3 — qualified definition.** Treat the identifier right of `::` as a
   package member, resolve it through the new maps against the current file's index
   first and then project shards, with no fallback to local declarations.
   *Verify:* D1's two-sided test, plus F1 and F3 in all three placements, in
   `tests/test_definition.cpp`.

3. **F2 + F4 + F5 — hover.** Route hover through the index for package members so
   closed files produce detail, keeping the existing AST hover path as the primary
   source for symbols in the current file.
   *Verify:* exact-string tests in `tests/test_lsp_features.cpp`.

4. **F6 — references.** Emit `package_value::<pkg>::<name>` symbol IDs for scoped
   uses so they group with the declaration.
   *Verify:* the F6 test plus a `tests/test_syntax_index.cpp` test asserting the
   symbol ID on the reference entries.

5. **Performance guard.** Add a test that builds ~300 synthetic package files and
   asserts go-to-definition on an *unresolvable* identifier stays fast. Without a
   guard this regression is invisible — it only appears at project scale, and every
   functional test still passes.
   *Verify:* comfortably under the 172 µs figure in C1; the keyed baseline is 62 µs.

6. **Repair the `demo/`-dependent test** described at the top, against a committed
   fixture.

## Build and test

```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build
./build/lazyverilog-tests "[definition]"
./build/lazyverilog-tests "[hover]"
./build/lazyverilog-tests "[index]"
./build/lazyverilog-tests "exact test name here"
```

Tests mirror source files: `tests/test_definition.cpp`, `tests/test_lsp_features.cpp`
(hover lives here), `tests/test_syntax_index.cpp`.

Closed-file tests follow an existing pattern — write the fixture to a temp path,
`analyzer.set_extra_files({path})`, `analyzer.wait_for_background_index_idle()`,
then `analyzer.open()` the consuming file. See the existing
`definition: package function resolves to closed extra file via index` test.

## Known gaps — not in scope

Broken today. Do not let these confuse you into thinking you have regressed
something:

- `fn_pkg::add(1, 2)` inside a `localparam` initializer does not resolve, even
  though `definition: package function resolves to closed extra file via index`
  passes — that test exercises a different call context.
- `child #(.BWIDTH(4))` where `BWIDTH` is a non-ANSI body parameter declared in a
  closed file does not resolve or hover.

Both are reasonable follow-ups.

## Definition of done

- D1 fixed, with tests covering both the qualified and unqualified halves.
- F1–F6 have passing tests; hover assertions match the format string exactly.
- The performance guard passes.
- The `demo/`-dependent test is repaired against a committed fixture.
- Full `ctest` green, no new compiler warnings.
- No changes outside the scope above.
