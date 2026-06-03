# Completion Architecture

**LSP:** `textDocument/completion`, `completionItem/resolve`
**Trigger characters:** `.` `::` `` ` `` `(` `#` `"`

---

## Design Goals

| Priority | Goal |
|----------|------|
| 1 | Semantic correctness — prefer symbol-table-driven results over text search |
| 2 | Context awareness — different contexts yield different provider sets |
| 3 | Low latency — stay responsive under active typing with broken syntax |
| 4 | Extensibility — new providers drop in without touching dispatch logic |
| 5 | Robustness — graceful degradation when syntax is incomplete |

---

## High-Level Architecture

```
textDocument/completion request
        │
        ▼
┌─────────────────────┐
│  CompletionEngine   │  (completion.cpp)
│                     │
│  1. detect context  │◄── token stream + SyntaxTree
│  2. dispatch        │◄── provider registry
│  3. rank + dedup    │
│  4. return list     │
└─────────────────────┘
        │
        ├─► KeywordProvider
        ├─► IdentifierProvider
        ├─► MemberProvider
        ├─► PackageScopeProvider
        ├─► NamedPortProvider
        ├─► ParameterProvider
        ├─► MacroProvider
        ├─► EnumProvider
        ├─► SnippetProvider
        └─► FileProvider
```

The engine is the only entry point. Providers never communicate with each other. The server registers providers at startup; new providers need no changes outside `completion.cpp`.

---

## Internal Data Model

### SyntaxIndex Extension

Current `SyntaxIndex` holds modules, ports, instances, and completion symbols. Scope-sensitive value data is populated during `SyntaxIndex::build()` from the `SyntaxTree` — never from raw source text inspection. Block-local declarations carry a SyntaxTree-derived visibility line range so identifier completion can hide locals outside their enclosing block.

```cpp
// syntax_index.hpp additions

struct FieldEntry {
    std::string name;
    std::string type;       // raw type text (best-effort)
    bool is_rand{false};
    int line{0};
    int col{0};
};

struct MethodEntry {
    std::string name;
    std::string return_type;
    bool is_task{false};
    bool is_virtual{false};
    bool is_static{false};
    std::string visibility; // "public", "protected", "local"
    int line{0};
    int col{0};
};

struct ClassEntry {
    std::string name;
    std::string base_class;  // empty if none
    std::vector<FieldEntry> fields;
    std::vector<MethodEntry> methods;
    int line{0};
    int col{0};
};

struct EnumMemberEntry {
    std::string name;
    std::string value;   // optional, may be empty
};

struct TypedefEntry {
    std::string name;
    std::string resolved; // best-effort one-step resolution
    bool is_enum{false};
    std::vector<EnumMemberEntry> enum_members; // populated if is_enum
    int line{0};
};

struct MacroEntry {
    std::string name;
    bool is_function_like{false};
    std::vector<std::string> params; // empty for object-like
    int line{0};
};

struct PackageEntry {
    std::string name;
    std::vector<std::string> exported_names; // symbol names visible via pkg::
    int line{0};
};

struct InterfaceEntry {
    std::string name;
    std::vector<PortEntry> signals;    // reuse PortEntry
    std::vector<std::string> modports; // modport names
    int line{0};
};

// Extended SyntaxIndex
struct SyntaxIndex {
    // existing
    std::vector<ModuleEntry>   modules;
    std::vector<InstanceEntry> instances;
    std::unordered_map<std::string, size_t> module_by_name;

    // new
    std::vector<ClassEntry>    classes;
    std::vector<TypedefEntry>  typedefs;
    std::vector<MacroEntry>    macros;
    std::vector<PackageEntry>  packages;
    std::vector<InterfaceEntry> interfaces;

    std::unordered_map<std::string, size_t> class_by_name;
    std::unordered_map<std::string, size_t> typedef_by_name;
    std::unordered_map<std::string, size_t> package_by_name;
    std::unordered_map<std::string, size_t> interface_by_name;

    static SyntaxIndex build(const slang::syntax::SyntaxTree& tree,
                             std::string_view source = {});
};
```

### CompletionContext

```cpp
// features/completion.hpp

enum class CompletionContextKind {
    Identifier,       // general: visible symbols + keywords
    MemberAccess,     // foo.
    PackageScope,     // pkg::
    NamedPort,        // module_inst u0 (.
    Parameter,        // module_inst #(.
    Macro,            // `
    EnumAssignment,   // state <= | where state is enum-typed
    Constructor,      // new
    ClassExtends,     // class X extends
    EventControl,     // @(posedge
    IncludeFile,      // `include "
    Unknown,          // fallback
};

struct CompletionContext {
    CompletionContextKind kind{CompletionContextKind::Unknown};
    std::string prefix;           // text already typed (for prefix matching)
    std::string scope_name;       // LHS of . or :: (for MemberAccess / PackageScope)
    std::string lhs_type;         // resolved type name (best-effort, for EnumAssignment)
    Position    cursor;
    bool        trigger_explicit{false}; // user-invoked vs. trigger-character
};
```

### CompletionProvider Interface

```cpp
class CompletionProvider {
public:
    virtual ~CompletionProvider() = default;

    // Return true if this provider can contribute to ctx.
    virtual bool accepts(const CompletionContext& ctx) const = 0;

    // Return completion items. May be called concurrently; must be thread-safe.
    virtual std::vector<lsCompletionItem> provide(
        const CompletionContext& ctx,
        const DocumentState&     state,
        const Analyzer&          analyzer
    ) const = 0;
};
```

---

## Context Detection

Context detection runs synchronously on the request thread before provider dispatch. It is fast — it works on the pre-parsed token buffer, not on a fresh parse.

### Algorithm

```
1. Find the token at/before cursor.
2. Walk backwards up to N tokens (N ≈ 20).
3. Match the first applicable pattern below.
4. If no pattern matches → Identifier context.
```

### Token Pattern Table

| Pattern (right-to-left from cursor) | Context |
|--------------------------------------|---------|
| `IDENT .` | MemberAccess; scope_name = IDENT |
| `IDENT ::` | PackageScope; scope_name = IDENT |
| `` ` `` | Macro |
| `.` inside named-port argument list¹ | NamedPort |
| `.` inside `#(...)` parameter list¹ | Parameter |
| `<=` or `=` where LHS is enum-typed | EnumAssignment; lhs_type = typedef name |
| `new` | Constructor |
| `extends` | ClassExtends |
| `posedge` / `negedge` / `@(` | EventControl |
| `"include "` with open quote | IncludeFile |
| anything else | Identifier |

¹ Requires checking whether the enclosing `(` belongs to a module instantiation — scan back to the `IDENT IDENT (` or `IDENT #(` pattern.

### Implementation Notes

- Use `TokenKind` comparisons only. No string matching or regex.
- Named-port vs. parameter disambiguation: check whether a `#(` or plain `(` opened the argument list.
- The `EnumAssignment` context requires one lookup into `SyntaxIndex::typedef_by_name`. If the typedef is not found or is not an enum, fall back to `Identifier`.

---

## Provider System

### KeywordProvider

Accepts: `Identifier`, `ClassExtends`, `EventControl`, `Constructor`

Maintains a static table of context → keyword list:

```cpp
// module item level
{ "assign", "always_comb", "always_ff", "always_latch", "initial",
  "final", "generate", "endmodule", "parameter", "localparam", ... }

// procedural block
{ "if", "else", "case", "casez", "casex", "for", "foreach", "while",
  "repeat", "return", "break", "continue", "begin", "end", ... }

// class body
{ "function", "task", "constraint", "covergroup", "rand", "randc",
  "static", "virtual", "local", "protected", "extends", "implements", ... }

// covergroup body
{ "coverpoint", "cross", "bins", "illegal_bins", "ignore_bins",
  "option", "type_option", ... }
```

Context (module item vs. procedural vs. class body vs. covergroup body) is determined by walking the parsed `slang::syntax::SyntaxTree` and selecting the smallest enclosing syntax node at the cursor. The implementation uses node types such as `ModuleDeclarationSyntax`, `StatementSyntax`, `ClassDeclarationSyntax`, and `CovergroupDeclarationSyntax`; it does not scan raw SystemVerilog text for block keywords.

Returns `lsCompletionItemKind::Keyword`. No snippets — SnippetProvider handles those.

### IdentifierProvider

Accepts: `Identifier`, `EnumAssignment`, `Constructor`, `ClassExtends`, `EventControl`

Collects visible symbols from `SyntaxIndex`:
- All module names → `Module`
- All class names → `Class`
- All interface names → `Interface`
- All typedef names → `TypeParameter`
- All package names → `Module`
- All macro names → `Constant`
- All port names (current module) → `Variable`

For `EnumAssignment`: filters to enum members of the known LHS type first, then appends other identifiers at lower priority.

For `Constructor`: filters to class names only.

For `ClassExtends`: filters to class and interface names only.

For `EventControl`: prioritizes signals of type `event` or ports of direction `input`.

### MemberProvider

Accepts: `MemberAccess`

Resolves `scope_name` to a type:

1. Check `SyntaxIndex::module_by_name` — if hit, return port names.
2. Check `SyntaxIndex::interface_by_name` — return signals + modport names.
3. Check `SyntaxIndex::class_by_name` — return fields + methods.
4. Check if `scope_name` is a local variable: scan port declarations of the enclosing module for `scope_name`, extract its declared type, then repeat from step 1–3 with that type.
5. If the type is a typedef, resolve one step via `SyntaxIndex::typedef_by_name`.
6. If still unresolved: return empty (do not pollute results with unrelated items).

**Limitation:** Multi-step typedef chains and parameterized types may not resolve. This is an accepted tradeoff of the SyntaxTree + heuristics approach. See [Slang Integration](#slang-integration-ideas) for the upgrade path.

### PackageScopeProvider

Accepts: `PackageScope`

Looks up `scope_name` in `SyntaxIndex::package_by_name`, returns `exported_names` as completion items.

Includes wildcard-imported packages: scan the current file's `import pkg::*` statements and merge all matching packages.

### NamedPortProvider

Accepts: `NamedPort`

This is the highest-value provider. Steps:

1. Walk backwards from cursor to find the module instance: `ModuleName InstanceName (`.
2. Look up `ModuleName` in `SyntaxIndex::module_by_name`.
3. Collect all ports from `ModuleEntry::ports`.
4. Scan the current instantiation's existing connections (already in `InstanceEntry::connections` for indexed instances; otherwise scan token stream).
5. Remove already-connected ports.
6. For each remaining port, rank higher if a local signal with the same name exists in scope.
7. Generate snippet: `.port_name(${1:port_name})` for ports without a matching local signal.
8. Generate plain: `.port_name(port_name)` for ports with a matching local signal.

Returns `lsCompletionItemKind::Field`.

### ParameterProvider

Accepts: `Parameter`

1. Find enclosing `ModuleName #(`.
2. Look up module in index.
3. Filter module's parameter declarations (port entries with kind `parameter` / `localparam`).
4. Remove already-assigned parameters.
5. Include default value in `detail` field.

### MacroProvider

Accepts: `Macro`

Source: `SyntaxIndex::macros` (from current file + all extra_files).

- Object-like macros → plain completion, kind `Constant`.
- Function-like macros → snippet completion with `${N:param}` placeholders, kind `Function`.

UVM macros (`` `uvm_component_utils ``, `` `uvm_config_db ``, etc.) appear naturally if UVM source is in `extra_files_`. No special UVM hardcoding needed.

### EnumProvider

Accepts: `EnumAssignment` (supplements IdentifierProvider)

Returns enum members of the known LHS type with maximum boost score. Listed before any other items.

### SnippetProvider

Accepts: `Identifier` (only at statement or module-item level, not inside expressions)

Returns structural snippets. Activate only when prefix is empty or matches snippet name:

| Trigger | Snippet |
|---------|---------|
| `module` | `module ${1:name} (...);\n  $0\nendmodule` |
| `interface` | `interface ${1:name} (...);\n  $0\nendinterface` |
| `class` | `class ${1:name};\n  $0\nendclass` |
| `always_ff` | `always_ff @(posedge ${1:clk} or negedge ${2:rst_n}) begin\n  $0\nend` |
| `always_comb` | `always_comb begin\n  $0\nend` |
| `function` | `function ${1:void} ${2:name}(...);\n  $0\nendfunction` |
| `task` | `task ${1:name}(...);\n  $0\nendtask` |
| `case` | `case (${1:expr})\n  default: $0\nendcase` |
| `covergroup` | `covergroup ${1:name};\n  $0\nendgroup` |
| `generate` | `generate\n  $0\nendgenerate` |
| `package` | `package ${1:name};\n  $0\nendpackage` |

Returns `lsCompletionItemKind::Snippet`.

### FileProvider

Accepts: `IncludeFile`

Scans only explicit header files (`.svh`, `.vh`) that appear in the configured
`.f` filelist.

LazyVerilog intentionally does not interpret `+incdir+` for completion. If a
header should be suggested, list that header file itself in the `.f` filelist.

---

## Ranking

All providers contribute items into a single flat list. Ranking runs after collection.

### Score Formula

```
score = prefix_score + scope_score + type_score + recency_score
```

| Component | Value | Condition |
|-----------|-------|-----------|
| `prefix_score` | 100 | exact prefix match |
| `prefix_score` | 60–99 | fuzzy match (higher = better) |
| `prefix_score` | 0 | no match (item excluded) |
| `scope_score` | 30 | declared in same module/block |
| `scope_score` | 20 | declared in same file |
| `scope_score` | 10 | declared in extra_files |
| `scope_score` | 0 | global / external |
| `type_score` | 25 | type-compatible with expected type (EnumAssignment, EventControl) |
| `type_score` | 0 | unknown or incompatible |
| `recency_score` | 0–10 | recently accepted completions (LRU, max 50 items, per-document) |

Items with `prefix_score == 0` are dropped entirely.

### Fuzzy Match

Use a simple scoring function: consecutive match > scattered match. Score proportional to match density. Do not use regex. Walk both strings character by character.

```cpp
// Returns 0 if no fuzzy match, 1–99 otherwise.
int fuzzy_score(std::string_view candidate, std::string_view prefix);
```

### Deduplication

After ranking, deduplicate by `(label, kind)`. Keep the higher-scored item. Snippets are never deduplicated against non-snippets — they have different `insertText`.

### Sort Keys

LSP `sortText` field: zero-pad the negative score so lexicographic sort equals numeric sort descending.

```cpp
item.sortText = fmt::format("{:05d}", 99999 - clamped_score);
```

---

## Broken Syntax Strategy

The engine must never return an empty completion list unless the context is genuinely unresolvable (e.g., inside a string literal).

### Degradation Ladder

```
1. Full context detection from SyntaxTree (normal path)
        │ fails (parse error at cursor)
        ▼
2. Token-stream-only context detection
   (walk raw token buffer, no AST traversal)
        │ fails (no tokens near cursor)
        ▼
3. Identifier fallback
   Return all module names + all typedef names + keyword set
        │ always succeeds
        ▼
4. Keyword-only fallback (last resort)
   Return the flat SystemVerilog keyword list
```

### Token-Stream Fallback

When the SyntaxTree is invalid or stale, run context detection against the raw token buffer that the lexer produced before the failed parse. The lexer is tolerant of incomplete input. This covers cases like:

```systemverilog
always_ff @(posedge   // incomplete — no closing paren
foo.bar(              // incomplete call
class A exten         // mid-keyword
```

Pattern matching against token kinds still works because the lexer tokenizes independently of the parser.

### Stale Tree Heuristic

If `make_state()` has not yet returned for the current document version (background re-parse in progress), use the previous document's `SyntaxIndex` for provider dispatch. Mark the response `isIncomplete: true` so the client re-requests on the next keystroke.

---

## Async Model and Cancellation

### Thread Model

```
Server thread (JSON-RPC recv)
    │
    └─► completion_worker thread pool (1–2 threads)
            │
            ├─► detect context (fast, ~µs)
            ├─► dispatch providers (fast–medium, ~ms)
            ├─► rank (fast, ~µs)
            └─► respond
```

Each completion request receives a cancellation token. When `$/cancelRequest` arrives for a pending request ID, the token is signalled. Providers check the token at natural yield points (between providers, not inside a tight loop).

```cpp
struct CancellationToken {
    std::atomic<bool> cancelled{false};
};

// Provider dispatch loop
for (auto& provider : providers_) {
    if (token.cancelled) throw CompletionCancelled{};
    if (provider->accepts(ctx))
        append(provider->provide(ctx, state, analyzer));
}
```

### Request Coalescing

If two completion requests arrive for the same document URI within a short window (< 50 ms), cancel the first and process only the second. Avoids redundant work during fast typing.

---

## LSP Integration

### Capability Registration

```cpp
// server.cpp — td_initialize handler
capabilities.completionProvider = {
    .triggerCharacters    = {".", "::", "`", "(", "#", "\""},
    .resolveProvider      = true,   // enable completionItem/resolve
    .completionItemKinds  = { /* all used kinds */ },
};
```

### Request Handler

```cpp
ep.registerHandler([&](const td_completion::request& req) {
    auto token = make_cancellation_token(req.id);
    return completion_worker_.submit([=, &analyzer_] {
        auto state = analyzer_.get_state(req.params.textDocument.uri);
        if (!state) return make_empty_list();
        return completion_engine_.complete(req.params, *state, analyzer_, token);
    });
});
```

### completionItem/resolve Handler

Lazy-load documentation and expensive detail for a single item. The item's `data` field carries a stable identifier (e.g., `{ "kind": "member", "type": "my_class", "name": "field_name" }`).

```cpp
ep.registerHandler([&](const completionItem_resolve::request& req) {
    auto item = req.params;
    // fill item.documentation from SyntaxIndex or source comment extraction
    return item;
});
```

### Completion Item Kind Mapping

| Symbol | `lsCompletionItemKind` |
|--------|------------------------|
| module | `Module` |
| interface | `Interface` |
| class | `Class` |
| function | `Function` |
| task | `Function` |
| variable / net / port | `Variable` |
| parameter / localparam | `Constant` |
| typedef | `TypeParameter` |
| enum member | `EnumMember` |
| macro (object-like) | `Constant` |
| macro (function-like) | `Function` |
| keyword | `Keyword` |
| snippet | `Snippet` |
| modport | `Field` |
| struct field / class property | `Field` |
| package | `Module` |
| file path | `File` |

---

## Slang Integration Ideas

The current design uses `SyntaxTree + SyntaxIndex` (no `Compilation`). This is intentional for latency, but certain features degrade:

| Feature | With SyntaxIndex | With Compilation |
|---------|-----------------|-----------------|
| Module port names | ✓ | ✓ |
| Class fields (direct) | ✓ | ✓ |
| Typedef chain resolution | one step | full chain |
| Parameterized class members | ✗ | ✓ |
| Virtual interface members | ✗ | ✓ |
| RHS type inference | ✗ | ✓ |
| Hierarchical references | ✗ | ✓ |

### Upgrade Path

If a lazy compilation approach is added to `Analyzer` in the future:

1. Add `analyzer_.get_compilation(uri)` returning `std::optional<slang::Compilation*>`.
2. `MemberProvider` checks for compilation first; falls back to SyntaxIndex.
3. `EnumProvider` uses `compilation->lookupName()` for full typedef chain resolution.
4. Compilation runs on a low-priority background thread; results land in a cache keyed by (uri, version).

No changes to the provider interface are required.

---

## Recommended Implementation Order

Build in this order to get value at each step:

| Phase | Deliverable | Value |
|-------|-------------|-------|
| 1 | `CompletionContext` detection + engine scaffolding | Foundation; no regressions |
| 2 | `KeywordProvider` (context-aware keyword sets) | Replaces current hardcoded list |
| 3 | `IdentifierProvider` (modules, interfaces, classes from SyntaxIndex) | Most-used general completion |
| 4 | `SyntaxIndex` extension (classes, typedefs, macros, packages) | Enables phases 5–8 |
| 5 | `NamedPortProvider` | Highest editor-UX value for SV |
| 6 | `MemberProvider` (best-effort type resolution) | `obj.` completion |
| 7 | `MacroProvider` | `` `macro `` completion |
| 8 | `SnippetProvider` | Structural snippets |
| 9 | `PackageScopeProvider` | `pkg::` completion filtered to symbols indexed under that package |
| 10 | `ParameterProvider` | `#(.param` completion |
| 11 | `EnumProvider` + type-aware ranking | `state <= IDLE` |
| 12 | `FileProvider` | `` `include "`` |
| 13 | `completionItem/resolve` | Lazy documentation |
| 14 | Async + cancellation | Responsiveness at scale |
| 15 | Recency ranking (LRU cache) | Polish |

Do not skip Phase 1–3. They are the scaffolding everything else plugs into.

---

## Common Pitfalls and Failure Modes

### Pitfalls

**Dumping all symbols everywhere.** Every provider must implement `accepts()` narrowly. A provider that returns `true` for `Unknown` context will pollute every completion list with unrelated items.

**String-based type resolution.** Tempting to extract type names via regex from port/variable declarations. This breaks on `typedef`, `import`, and parameterized types. Always use `TokenKind`-based slang syntax facts.

**Stale instance connections.** `NamedPortProvider` must detect which ports are already connected by scanning the *current document text*, not the indexed `InstanceEntry::connections`. The index lags behind the current edit.

**Trigger character overreach.** `.` triggers both `MemberAccess` and `NamedPort`. Context detection must be correct before dispatching — never dispatch both providers for the same `.`.

**Empty list on parse failure.** Always fall through the degradation ladder. An empty list is worse than a noisy list — an empty list breaks the editor UX entirely.

**Thread safety on `SyntaxIndex`.** The index is built on the `make_state()` thread and read on the completion worker thread. Ensure the `DocumentState` passed to providers is an immutable snapshot, not a reference to live state.

**`resolveProvider = true` without implementing resolve.** If `resolveProvider` is advertised but the handler is missing, some editors will hang waiting for resolved items. Implement a no-op passthrough resolve handler before advertising `resolveProvider = true`.

### Failure Modes

| Scenario | Expected Behavior |
|----------|------------------|
| File has 500 syntax errors | Fall through to token-stream context, return identifiers + keywords |
| Cursor inside block comment | Return empty list (detect by token kind `BlockComment`) |
| Cursor inside string literal | Return empty list or FileProvider only (for `include "`) |
| Module not found in index | `NamedPortProvider` returns empty; `IdentifierProvider` covers it |
| Unknown `scope_name` in `obj.` | `MemberProvider` returns empty; no fallback to all-symbols |
| Cancellation during provider loop | Throw `CompletionCancelled`, return empty response with `$/cancelRequest` acknowledgement |
| `extra_files` contain UVM source | MacroProvider + IdentifierProvider naturally cover UVM symbols |
| Parameterized class `my_class #(T)` member access | `MemberProvider` cannot resolve; returns empty for that member — acceptable limitation |
| Forward declaration only (no body) | Providers return the name but no members; degradation is correct |
| Wildcard import `import pkg::*` | `IdentifierProvider` merges all symbols from all wildcard-imported packages |
| Nested generate block | Context detection still works (token-based); scope filtering is best-effort |

---

## Edge Cases

### Hierarchical References

`top.dut.signal` chains require multi-step instance resolution. This is out of scope for the SyntaxIndex approach. `MemberProvider` resolves exactly one `.` hop. For deeper chains, return empty after the first unresolved step.

### Virtual Interfaces

`virtual interface` variables hold an interface type name that is not recoverable from the SyntaxTree without full type propagation. Best-effort: scan variable declarations for `virtual interface IfaceName vif` patterns using `TokenKind` and treat `IfaceName` as the resolved type.

### Anonymous Structs

```systemverilog
struct packed { logic [3:0] a; logic b; } my_struct;
```

Fields are not stored by name in the SyntaxIndex. `MemberProvider` will return empty for anonymous struct members. Named typedefs (`typedef struct { ... } my_t;`) are covered via `TypedefEntry`.

### Macro-Generated Code

Do not expand macros during completion. Symbols generated by macro expansion are invisible to the SyntaxIndex. This is a known gap. If UVM source is indexed, UVM class members are available through normal class indexing, not macro expansion.

### Package Wildcard Imports Across Files

If `import uvm_pkg::*` appears in file A and completion is requested in file B which includes file A, the wildcard import is only visible if both files are in the SyntaxIndex (i.e., both are in `extra_files_`). Single-file indexing does not cross file boundaries.
