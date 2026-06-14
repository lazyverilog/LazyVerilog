import * as vscode from "vscode";
import {
  LanguageClient,
  WorkspaceEdit as LspWorkspaceEdit,
} from "vscode-languageclient/node";

// ---------------------------------------------------------------------------
// Server response types
// ---------------------------------------------------------------------------

interface PortInfo {
  name: string;
  direction?: string;
  type?: string;
}

interface ModuleInfo {
  ports?: PortInfo[];
  instances?: { hierarchical_path?: string; inst_name?: string; module_name?: string }[];
}

interface HierarchyNode {
  hierarchical_path?: string;
  inst_name?: string;
  module_name?: string;
  root?: boolean;
}

interface ConnectInfoResult {
  modules?: Record<string, ModuleInfo>;
  roots?: HierarchyNode[];
  error?: string;
}

interface ConnectPreview {
  wire_name?: string;
  wire_type?: string;
  lca_module?: string;
  edits?: { file?: string; line?: number; description?: string; is_warning?: boolean }[];
  warnings?: string[];
  error?: string;
}

function connectLog(message: string, data?: unknown): void {
  // Intentionally disabled for release builds.
  //
  // The connect flow may handle workspace paths, hierarchy names, generated
  // edits, and signal names.  Those details are useful during local crash
  // debugging, but a Marketplace VSIX should not write them to a fixed file in
  // /tmp without an explicit user opt-in.  Preserve the hook as a no-op so the
  // surrounding control-flow logging can be temporarily restored when needed.
  void message;
  void data;
}

function summarizeWorkspaceEditChanges(changes: unknown): unknown {
  // Log enough to diagnose invalid/huge/overlapping edits without dumping full
  // source files.  Include a short escaped text prefix so line-ending and
  // comma/port-list issues are visible in the crash log.
  if (!changes || typeof changes !== "object") return changes;
  const summary: Record<string, unknown[]> = {};
  for (const [uriText, rawTextEdits] of Object.entries(changes as Record<string, unknown>)) {
    if (!Array.isArray(rawTextEdits)) {
      summary[uriText] = [{ invalid: "edits is not an array", type: typeof rawTextEdits }];
      continue;
    }
    summary[uriText] = rawTextEdits.map((raw, index) => {
      const edit = raw as LspTextEditLike;
      const newText = typeof edit.newText === "string" ? edit.newText : "";
      return {
        index,
        range: edit.range,
        newTextLength: newText.length,
        newTextPreview: newText.slice(0, 160),
      };
    });
  }
  return summary;
}


interface LspTextEditLike {
  range?: {
    start?: { line?: number; character?: number };
    end?: { line?: number; character?: number };
  };
  newText?: string;
}

function asNonNegativeInteger(value: unknown, field: string): number {
  // LSP positions are zero-based UTF-16 line/character pairs.  A malformed
  // server response should be rejected before it reaches VS Code's native edit
  // application path; otherwise an invalid Range can make the editor unstable
  // and, on some Electron builds, can crash the renderer/main process instead
  // of surfacing a normal JavaScript exception.
  if (!Number.isInteger(value) || (value as number) < 0) {
    throw new Error(`invalid ${field}: expected a non-negative integer`);
  }
  return value as number;
}

function positionFromLspStrict(
  pos: { line?: number; character?: number } | undefined,
  context: string,
): vscode.Position {
  if (!pos) {
    throw new Error(`invalid ${context}: missing position`);
  }
  return new vscode.Position(
    asNonNegativeInteger(pos.line, `${context}.line`),
    asNonNegativeInteger(pos.character, `${context}.character`),
  );
}

function rangeFromLspStrict(edit: LspTextEditLike, context: string): vscode.Range {
  if (!edit.range) {
    throw new Error(`invalid ${context}: missing range`);
  }
  const range = new vscode.Range(
    positionFromLspStrict(edit.range.start, `${context}.range.start`),
    positionFromLspStrict(edit.range.end, `${context}.range.end`),
  );
  if (range.start.isAfter(range.end)) {
    throw new Error(`invalid ${context}: range start is after range end`);
  }
  return range;
}

function offsetAtStrict(
  document: vscode.TextDocument,
  position: vscode.Position,
  context: string,
): number {
  // VS Code accepts an EOF position at (lineCount, 0), but lineAt(lineCount) is
  // invalid.  Handle that single EOF spelling explicitly, then validate regular
  // line/character bounds without clamping.  Clamping would hide a server bug and
  // could apply a Connect edit to the wrong place.
  if (position.line === document.lineCount && position.character === 0) {
    return document.getText().length;
  }
  if (position.line >= document.lineCount) {
    throw new Error(
      `invalid ${context}: line ${position.line} is outside document with ${document.lineCount} line(s)`,
    );
  }

  const line = document.lineAt(position.line);
  if (position.character > line.text.length) {
    throw new Error(
      `invalid ${context}: character ${position.character} is outside line ${position.line} with ${line.text.length} UTF-16 code unit(s)`,
    );
  }
  return document.offsetAt(position);
}

async function connectWorkspaceEditFromChanges(
  result: LspWorkspaceEdit & { changes?: unknown },
): Promise<vscode.WorkspaceEdit | undefined> {
  const changes = result.changes as Record<string, unknown> | undefined;
  if (!changes) return undefined;

  const workspaceEdit = new vscode.WorkspaceEdit();

  for (const [uriText, rawTextEdits] of Object.entries(changes)) {
    if (!Array.isArray(rawTextEdits)) {
      throw new Error(`invalid changes for ${uriText}: expected an array of TextEdit objects`);
    }

    const uri = vscode.Uri.parse(uriText);

    // Open the document before building the WorkspaceEdit so range validation is
    // performed against the exact file contents VS Code will edit.  This is
    // especially important for Connect because the server can edit closed files
    // from the filelist in addition to the active buffer.
    const document = await vscode.workspace.openTextDocument(uri);

    const edits = rawTextEdits.map((raw, index) => {
      const textEdit = raw as LspTextEditLike;
      const context = `${uriText} edit #${index + 1}`;
      const range = rangeFromLspStrict(textEdit, context);
      const startOffset = offsetAtStrict(document, range.start, `${context}.range.start`);
      const endOffset = offsetAtStrict(document, range.end, `${context}.range.end`);
      if (textEdit.newText !== undefined && typeof textEdit.newText !== "string") {
        throw new Error(`invalid ${context}: newText must be a string`);
      }
      return {
        range,
        newText: textEdit.newText ?? "",
        startOffset,
        endOffset,
        context,
        originalIndex: index,
      };
    });

    // LSP requires same-document TextEdits to be non-overlapping.  Detecting
    // overlaps here gives a controlled LazyVerilog error instead of handing an
    // ambiguous edit set to VS Code.  Adjacent edits and multiple zero-length
    // insertions at the same offset are allowed; same-position insert order is
    // inherently ambiguous in LSP, so keep the server order for those ties.
    const byStart = [...edits].sort((a, b) =>
      a.startOffset - b.startOffset ||
      a.endOffset - b.endOffset ||
      a.originalIndex - b.originalIndex,
    );
    for (let i = 1; i < byStart.length; ++i) {
      if (byStart[i].startOffset < byStart[i - 1].endOffset) {
        throw new Error(
          `overlapping Connect edits in ${uriText}: ${byStart[i - 1].context} overlaps ${byStart[i].context}`,
        );
      }
    }

    workspaceEdit.set(
      uri,
      // Feed VS Code a stable, ascending edit list.  The server currently emits
      // Connect edits in descending order to be robust for sequential clients,
      // but the VS Code WorkspaceEdit API is happier when all edits for a file
      // are normalized before they cross into the editor's native bulk-edit path.
      byStart.map((e) => vscode.TextEdit.replace(e.range, e.newText)),
    );
  }

  return workspaceEdit;
}

// ---------------------------------------------------------------------------
// Path helpers (mirrors Lua _connect_boundary_paths)
// ---------------------------------------------------------------------------

function splitPath(path: string): string[] {
  return path.split(".");
}

function joinPath(parts: string[], count: number): string {
  return parts.slice(0, count).join(".");
}

function lcaLen(pathA: string, pathB: string): [number, string[], string[]] {
  const a = splitPath(pathA);
  const b = splitPath(pathB);
  const n = Math.min(a.length, b.length);
  let i = 0;
  while (i < n && a[i] === b[i]) i++;
  return [i, a, b];
}

// Returns [sourceUp, destDown] boundary paths between two leaf paths.
// Matches Lua _connect_boundary_paths exactly (1-indexed → 0-indexed translated).
function boundaryPaths(sourcePath: string, destPath: string): [string[], string[]] {
  const [lca, src, dst] = lcaLen(sourcePath, destPath);
  const sourceUp: string[] = [];
  for (let i = src.length - 1; i >= lca + 1; i--) {
    sourceUp.push(joinPath(src, i));
  }
  const destDown: string[] = [];
  for (let i = lca + 1; i <= dst.length - 1; i++) {
    destDown.push(joinPath(dst, i));
  }
  return [sourceUp, destDown];
}

// ---------------------------------------------------------------------------
// Instance selection — BFS hierarchy traversal
// ---------------------------------------------------------------------------

async function selectInstance(
  client: LanguageClient,
  uri: string,
  roots: HierarchyNode[],
  targetModule: string,
  pathToModule: Record<string, string>,
): Promise<HierarchyNode | undefined> {
  const queue: HierarchyNode[] = [...roots];
  const seen = new Set<string>();
  const matches: HierarchyNode[] = [];
  let visited = 0;
  const MAX = 10000;

  while (queue.length > 0) {
    if (visited >= MAX) {
      void vscode.window.showWarningMessage(
        `[LazyVerilog] Connect: stopped after ${MAX} nodes — narrow the project/filelist`,
      );
      break;
    }
    const node = queue.shift()!;
    const path = node.hierarchical_path ?? "";
    if (!path || seen.has(path)) continue;
    seen.add(path);
    visited++;
    if (node.module_name) pathToModule[path] = node.module_name;

    if (!node.root && node.module_name === targetModule) {
      matches.push(node);
      continue; // don't expand past target
    }

    let result: { children?: HierarchyNode[]; error?: string } | null;
    try {
      result = (await client.sendRequest("workspace/executeCommand", {
        command: "lazyverilog.connectHierarchyChildren",
        arguments: [uri, path],
      })) as { children?: HierarchyNode[] } | null;
    } catch {
      break;
    }
    for (const child of result?.children ?? []) {
      if (child.hierarchical_path && !seen.has(child.hierarchical_path)) {
        if (child.module_name) pathToModule[child.hierarchical_path] = child.module_name;
        queue.push(child);
      }
    }
  }

  if (matches.length === 0) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect: no instances of '${targetModule}' found`,
    );
    return undefined;
  }

  matches.sort((a, b) =>
    (a.hierarchical_path ?? "").localeCompare(b.hierarchical_path ?? ""),
  );
  if (matches.length === 1) return matches[0];

  const items = matches.map((m) => ({
    label: m.inst_name ?? m.hierarchical_path ?? "",
    description: m.hierarchical_path ?? "",
    node: m,
  }));
  const picked = await vscode.window.showQuickPick(items, {
    placeHolder: `Select ${targetModule} instance`,
  });
  return picked?.node;
}

// ---------------------------------------------------------------------------
// Pick from candidates or type a custom name
// ---------------------------------------------------------------------------

const CUSTOM_LABEL = "$(edit) Type a new name…";

async function pickOrType(
  prompt: string,
  candidates: string[],
  defaultVal = "",
): Promise<string | undefined> {
  if (candidates.length > 0) {
    const items = [...candidates, CUSTOM_LABEL];
    const picked = await vscode.window.showQuickPick(items, { placeHolder: prompt });
    if (!picked) return undefined;
    if (picked === CUSTOM_LABEL) {
      return vscode.window.showInputBox({ prompt, value: defaultVal });
    }
    return picked;
  }
  return vscode.window.showInputBox({ prompt, value: defaultVal });
}

// ---------------------------------------------------------------------------
// Port candidates for a boundary path
// ---------------------------------------------------------------------------

function portCandidates(
  modules: Record<string, ModuleInfo>,
  pathToModule: Record<string, string>,
  boundaryPath: string,
  direction: string,
): [string[], string | undefined] {
  const moduleName = pathToModule[boundaryPath];
  const mod = moduleName ? modules[moduleName] : undefined;
  if (!mod) return [[], moduleName];
  const names = (mod.ports ?? [])
    .filter((p) => !direction || p.direction === direction)
    .map((p) => p.name)
    .sort();
  return [names, moduleName];
}

// ---------------------------------------------------------------------------
// Main connect wizard
// ---------------------------------------------------------------------------

async function connectFlow(
  client: LanguageClient,
  uri: string,
  module1: string,
  module2: string,
): Promise<void> {
  connectLog("connectFlow start", { uri, module1, module2 });

  // Step 1: fetch connect info
  let info: ConnectInfoResult | null;
  try {
    info = (await client.sendRequest("workspace/executeCommand", {
      command: "lazyverilog.connectInfo",
      arguments: [uri, "lazy"],
    })) as ConnectInfoResult | null;
  } catch (e) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect: ${(e as Error).message}`,
    );
    return;
  }
  if (!info || info.error) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect: ${info?.error ?? "no data"}`,
    );
    return;
  }

  const modules = info.modules ?? {};
  const roots = info.roots ?? [];

  if (!modules[module1]) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect: module '${module1}' not found`,
    );
    return;
  }
  if (!modules[module2]) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect: module '${module2}' not found`,
    );
    return;
  }

  if (roots.length === 0) {
    void vscode.window.showErrorMessage(
      "[LazyVerilog] Connect: no hierarchy roots found",
    );
    return;
  }

  const pathToModule: Record<string, string> = {};
  for (const [name, mod] of Object.entries(modules)) {
    for (const inst of mod.instances ?? []) {
      if (inst.hierarchical_path) pathToModule[inst.hierarchical_path] = name;
    }
  }

  // Step 2: pick inst1
  const inst1 = await selectInstance(client, uri, roots, module1, pathToModule);
  if (!inst1) {
    connectLog("connectFlow cancelled at source instance");
    return;
  }
  connectLog("source instance selected", inst1);

  // Step 3: pick output port of module1
  const outPorts1 = (modules[module1].ports ?? [])
    .filter((p) => p.direction === "output")
    .map((p) => p.name)
    .sort();
  const port1Name = await pickOrType(
    `Source output port of ${module1}:`,
    outPorts1,
  );
  if (!port1Name) {
    connectLog("connectFlow cancelled at source port");
    return;
  }
  connectLog("source port selected", { port1Name });

  // Step 4: pick inst2
  const inst2 = await selectInstance(client, uri, roots, module2, pathToModule);
  if (!inst2) {
    connectLog("connectFlow cancelled at destination instance");
    return;
  }
  connectLog("destination instance selected", inst2);

  // Step 5: pick input port of module2
  const inPorts2 = (modules[module2].ports ?? [])
    .filter((p) => p.direction === "input")
    .map((p) => p.name)
    .sort();
  const port2Name = await pickOrType(
    `Destination input port of ${module2}:`,
    inPorts2,
  );
  if (!port2Name) {
    connectLog("connectFlow cancelled at destination port");
    return;
  }
  connectLog("destination port selected", { port2Name });

  // Step 6: wire name
  const connectRoute = `${inst1.hierarchical_path}.${port1Name} -> ${inst2.hierarchical_path}.${port2Name}`;
  const wireName = await vscode.window.showInputBox({
    prompt: `Wire name at common root  [${connectRoute}]`,
  });
  if (!wireName) {
    connectLog("connectFlow cancelled at wire name");
    return;
  }
  connectLog("wire name entered", { wireName });

  // Step 7: boundary port prompts
  const [sourceUp, destDown] = boundaryPaths(
    inst1.hierarchical_path ?? "",
    inst2.hierarchical_path ?? "",
  );

  const sourcePorts: string[] = [];
  for (const bPath of sourceUp) {
    const [cands, bMod] = portCandidates(modules, pathToModule, bPath, "output");
    const label = bMod ? `${bPath} [${bMod}]` : bPath;
    const name = await pickOrType(
      `Output port on ${label} to export ${port1Name}:`,
      cands,
      port1Name,
    );
    if (!name) return;
    sourcePorts.push(name);
  }

  // dest boundary prompts are asked source→dest (down), but applied leaf-up
  const destPortsDown: string[] = [];
  for (const bPath of destDown) {
    const [cands, bMod] = portCandidates(modules, pathToModule, bPath, "input");
    const label = bMod ? `${bPath} [${bMod}]` : bPath;
    const name = await pickOrType(
      `Input port on ${label} to import ${port2Name}:`,
      cands,
      port2Name,
    );
    if (!name) return;
    destPortsDown.push(name);
  }
  const destPortsLeafUp = [...destPortsDown].reverse();

  // Step 8: preview
  const applyArgs = [
    uri,
    inst1.hierarchical_path ?? "",
    port1Name,
    inst2.hierarchical_path ?? "",
    port2Name,
    wireName,
    sourcePorts.join("\n"),
    destPortsLeafUp.join("\n"),
  ];

  connectLog("connect apply args prepared", {
    sourcePath: applyArgs[1],
    sourcePort: applyArgs[2],
    destPath: applyArgs[3],
    destPort: applyArgs[4],
    wireName: applyArgs[5],
    sourceBoundaryPorts: sourcePorts,
    destBoundaryPortsLeafUp: destPortsLeafUp,
  });

  let preview: ConnectPreview | null;
  try {
    preview = (await client.sendRequest("workspace/executeCommand", {
      command: "lazyverilog.connectApplyPreview",
      arguments: applyArgs,
    })) as ConnectPreview | null;
  } catch (e) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect: ${(e as Error).message}`,
    );
    return;
  }
  if (!preview || preview.error) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect: ${preview?.error ?? "no preview"}`,
    );
    return;
  }

  const editLines = (preview.edits ?? []).map(
    (e) =>
      `${e.is_warning ? "⚠ " : "✓ "}${e.file ?? ""}:${e.line ?? 0}  ${e.description ?? ""}`,
  );
  const warningLines = (preview.warnings ?? []).map((w) => `⚠ ${w}`);
  const detailLines = [...editLines, ...warningLines];
  const MAX_PREVIEW_LINES = 40;
  const shownDetailLines = detailLines.slice(0, MAX_PREVIEW_LINES);
  if (detailLines.length > shownDetailLines.length) {
    shownDetailLines.push(
      `… ${detailLines.length - shownDetailLines.length} more change(s) omitted from this dialog`,
    );
  }

  const previewLines: string[] = [
    `Connect: ${preview.wire_type ?? ""} ${preview.wire_name ?? ""}  (wire at ${preview.lca_module ?? ""})`,
    "",
    ...shownDetailLines,
  ];

  connectLog("connect preview received", {
    wireName: preview.wire_name,
    wireType: preview.wire_type,
    lcaModule: preview.lca_module,
    editCount: preview.edits?.length ?? 0,
    warningCount: preview.warnings?.length ?? 0,
    edits: preview.edits,
    warnings: preview.warnings,
  });

  let vsEdit: vscode.WorkspaceEdit | undefined;

  // Step 9a: ask the server for the concrete edit before showing the final
  // confirmation.  The previous implementation used a modal
  // showInformationMessage here; the crash log stopped immediately after
  // "connect preview received", which means Code OSS likely crashed while
  // closing that native modal dialog, before the extension asked for/applied the
  // edit.  Prefetching and validating the edit first gives us useful diagnostics
  // in /tmp/lazyverilog-vscode-connect.log even if the UI crashes later.
  try {
    connectLog("requesting connectApply before confirmation");
    const result = (await client.sendRequest("workspace/executeCommand", {
      command: "lazyverilog.connectApply",
      arguments: applyArgs,
    })) as (LspWorkspaceEdit & { error?: string; changes?: unknown }) | null;
    if (result?.error) {
      connectLog("connectApply returned error before confirmation", { error: result.error });
      void vscode.window.showErrorMessage(`[LazyVerilog] Connect: ${result.error}`);
      return;
    }
    if (!result) {
      connectLog("connectApply returned null before confirmation");
      void vscode.window.showErrorMessage("[LazyVerilog] Connect apply: server returned no edit");
      return;
    }

    connectLog("connectApply result received before confirmation", {
      hasChanges: Boolean(result.changes),
      changes: summarizeWorkspaceEditChanges(result.changes),
    });

    // Connect's server response is a simple WorkspaceEdit.changes map. Apply it
    // directly instead of routing through vscode-languageclient's generic
    // protocol converter; this keeps the Connect path small and lets us validate
    // ranges before they reach VS Code's native bulk-edit code.
    vsEdit = await connectWorkspaceEditFromChanges(result);
    connectLog("connectApply result converted to VS Code WorkspaceEdit before confirmation");
    if (!vsEdit) {
      void vscode.window.showErrorMessage(
        "[LazyVerilog] Connect apply: server returned unsupported edit shape",
      );
      return;
    }
  } catch (e) {
    connectLog("connect prepare-apply exception", {
      message: (e as Error).message,
      stack: (e as Error).stack,
    });
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect apply: ${(e as Error).message}`,
    );
    return;
  }

  // Step 9b: use QuickPick instead of a modal information dialog.  This avoids
  // the native modal path that appears to crash Code OSS/Electron on the user's
  // Wayland setup, while still requiring an explicit Apply confirmation.
  type ConfirmItem = vscode.QuickPickItem & { action: "apply" | "cancel" };
  const confirmItems: ConfirmItem[] = [
    {
      label: "$(check) Apply",
      description: `${preview.edits?.length ?? 0} edit(s), ${preview.warnings?.length ?? 0} warning(s)`,
      detail: previewLines.join("\n"),
      action: "apply",
    },
    {
      label: "$(close) Cancel",
      description: "Do not modify files",
      action: "cancel",
    },
  ];

  connectLog("showing non-modal connect confirmation QuickPick");
  const answer = await vscode.window.showQuickPick(confirmItems, {
    placeHolder: `Connect ${inst1.hierarchical_path}.${port1Name} -> ${inst2.hierarchical_path}.${port2Name}`,
    matchOnDescription: true,
    matchOnDetail: true,
    ignoreFocusOut: true,
  });
  if (answer?.action !== "apply") {
    connectLog("connectFlow cancelled at non-modal confirmation", {
      answer: answer?.label,
    });
    return;
  }
  connectLog("connect confirmation accepted");

  // Step 9c: apply the already validated edit.
  try {
    connectLog("calling vscode.workspace.applyEdit");
    const applied = await vscode.workspace.applyEdit(vsEdit);
    connectLog("vscode.workspace.applyEdit returned", { applied });
    if (!applied) {
      void vscode.window.showErrorMessage(
        "[LazyVerilog] Connect apply: VS Code rejected the workspace edit",
      );
    }
  } catch (e) {
    connectLog("connect apply exception", {
      message: (e as Error).message,
      stack: (e as Error).stack,
    });
    void vscode.window.showErrorMessage(
      `[LazyVerilog] Connect apply: ${(e as Error).message}`,
    );
  }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

export function registerConnect(
  context: vscode.ExtensionContext,
  client: LanguageClient,
): void {
  context.subscriptions.push(
    vscode.commands.registerCommand("lazyverilog.connect", async () => {
      connectLog("lazyverilog.connect command invoked");
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        void vscode.window.showErrorMessage("[LazyVerilog] Connect: no active file");
        return;
      }
      const module1 = await vscode.window.showInputBox({
        prompt: "Source module name",
        placeHolder: "e.g. cpu",
      });
      if (!module1) return;
      const module2 = await vscode.window.showInputBox({
        prompt: "Destination module name",
        placeHolder: "e.g. bus",
      });
      if (!module2) return;
      const uri = editor.document.uri.toString();
      await connectFlow(client, uri, module1, module2);
    }),
  );
}
