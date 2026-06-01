# LSP Features

Standard Language Server Protocol features.

---

## Hover

**LSP:** `textDocument/hover`

Shows symbol information at the cursor — kind, type signature, and documentation — as a markdown popup.

No configuration.

---

## Go to Definition

**LSP:** `textDocument/definition`

Jumps to the declaration of the symbol under the cursor.

No configuration.

---

## Find References

**LSP:** `textDocument/references`

Finds all usages of the symbol under the cursor. Includes the declaration if `includeDeclaration` is set by the client.

No configuration.

---

## Rename

**LSP:** `textDocument/prepareRename`, `textDocument/rename`

Renames a symbol across all references. SystemVerilog keywords cannot be renamed.

No configuration.

---

## Completion

**LSP:** `textDocument/completion`

**Trigger characters:** `.` `$`

Offers very limited completions: fixed SystemVerilog keywords plus module and port names indexed from the current file.

No configuration.

---

## Signature Help

**LSP:** `textDocument/signatureHelp`

**Trigger characters:** `(` `,`

Displays parameter signatures while typing function/task calls and module instantiations. Tracks the active parameter as you move between arguments. Also handles module parameter lists (`#(...)`).

No configuration.

---

## Workspace Symbols

**LSP:** `workspace/symbol`

Searches modules and classes from indexed design files. Matching is case-insensitive substring matching.

Requires the design index to be populated via `design.vcode`. See [design](../design/index.md).

No configuration.

---

## Inlay Hints

**LSP:** `textDocument/inlayHint`

Displays inline hints inside module instantiations:

- **Port hints** — direction and type of each connected port shown inline.
- **Coverage hint** — `N/M` connected port count shown at the opening parenthesis.

```toml
[inlay_hint]
enable = true
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enable` | bool | `true` | Set `false` to disable all inlay hints |
