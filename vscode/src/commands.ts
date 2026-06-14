import * as vscode from "vscode";
import {
  LanguageClient,
  WorkspaceEdit as LspWorkspaceEdit,
} from "vscode-languageclient/node";

// ---------------------------------------------------------------------------
// Types matching server response shapes
// ---------------------------------------------------------------------------

interface AutoffPair {
  dst: string;
  src: string;
  missing_if?: boolean;
  missing_else?: boolean;
}

interface AutoffPreviewResult {
  pairs?: AutoffPair[];
  error?: string;
  warn?: boolean;
}

interface LintViolation {
  // Server Lint/LintAll currently returns LSP-like severity strings
  // ("Error", "Warning", "Information", "Hint") and a `message` field.
  // Keep `text` / numeric severity optional for compatibility with any older
  // client-side assumptions or future server variants.
  severity?: number | string;
  file?: string;
  line?: number;
  col?: number;
  message?: string;
  text?: string;
  class?: string;
}

// ---------------------------------------------------------------------------
// Shared output channel
// ---------------------------------------------------------------------------

function commandLog(message: string, data?: unknown): void {
  // Intentionally disabled for release builds.
  //
  // This hook is kept as a no-op so diagnostic call sites can be re-enabled
  // locally while debugging tricky UI / native Electron issues, but published
  // VSIX files must not write command telemetry or workspace-derived data to a
  // fixed path such as /tmp.  Keeping the function also avoids touching the
  // feature logic below; all existing calls remain harmless and side-effect
  // free.
  void message;
  void data;
}

function summarizeLintViolations(violations: unknown): unknown {
  // This is diagnostics-only logging, so it must be more permissive than the
  // actual UI rendering path.  Some server/client failure modes can return a
  // partial or differently-shaped item; logging should capture that shape, not
  // throw `Cannot read properties of undefined`.
  if (!Array.isArray(violations)) {
    return { count: 0, invalid: "lint response is not an array", type: typeof violations };
  }
  return {
    count: violations.length,
    first: violations.slice(0, 20).map((raw) => {
      const v = raw as Partial<LintViolation>;
      return {
        file: typeof v.file === "string" ? v.file : "",
        line: typeof v.line === "number" ? v.line : 0,
        col: typeof v.col === "number" ? v.col : 0,
        severity:
          typeof v.severity === "number" || typeof v.severity === "string"
            ? v.severity
            : "Warning",
        class: typeof v.class === "string" ? v.class : "",
        message: lintMessage(v).slice(0, 200),
        rawKeys: raw && typeof raw === "object" ? Object.keys(raw) : [],
      };
    }),
  };
}

async function confirmApplyQuickPick(
  title: string,
  detail: string,
  logPrefix: string,
): Promise<boolean> {
  // Avoid modal VS Code message dialogs throughout the extension.  On the
  // reporter's Code OSS/Electron/Wayland setup, native modal confirmations can
  // SIGSEGV the whole application.  QuickPick remains in the regular workbench
  // UI while still requiring an explicit user choice.
  commandLog(`${logPrefix}: showing non-modal Apply confirmation`);
  type ConfirmItem = vscode.QuickPickItem & { action: "apply" | "cancel" };
  const picked = await vscode.window.showQuickPick<ConfirmItem>(
    [
      {
        label: "$(check) Apply",
        description: title,
        detail,
        action: "apply",
      },
      {
        label: "$(close) Cancel",
        description: "Do not modify files",
        action: "cancel",
      },
    ],
    {
      placeHolder: title,
      ignoreFocusOut: true,
      matchOnDescription: true,
      matchOnDetail: true,
    },
  );
  commandLog(`${logPrefix}: Apply confirmation returned`, { picked: picked?.label });
  return picked?.action === "apply";
}

let outputChannel: vscode.OutputChannel | undefined;

function getOutputChannel(): vscode.OutputChannel {
  if (!outputChannel) {
    outputChannel = vscode.window.createOutputChannel("LazyVerilog");
  }
  return outputChannel;
}

// ---------------------------------------------------------------------------
// AutoFF flow (shared between autoff / autoffAll variants)
// ---------------------------------------------------------------------------

function formatAutoffPreview(result: AutoffPreviewResult): string {
  const pairs = result.pairs ?? [];
  const lines: string[] = [];
  const resetPairs = pairs.filter((p) => p.missing_if);
  const capturePairs = pairs.filter((p) => p.missing_else);
  if (resetPairs.length > 0) {
    lines.push("Reset (if) block:");
    for (const p of resetPairs) {
      lines.push(`  ${p.dst} <= '0;`);
    }
  }
  if (capturePairs.length > 0) {
    if (lines.length > 0) lines.push("");
    lines.push("Capture (else) block:");
    for (const p of capturePairs) {
      lines.push(`  ${p.dst} <= ${p.src};`);
    }
  }
  return lines.join("\n");
}

async function autoffFlow(
  client: LanguageClient,
  previewCmd: string,
  applyCmd: string,
  args: unknown[],
): Promise<void> {
  let result: AutoffPreviewResult;
  try {
    result = (await client.sendRequest("workspace/executeCommand", {
      command: previewCmd,
      arguments: args,
    })) as AutoffPreviewResult;
  } catch (e) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] AutoFF: ${(e as Error).message}`,
    );
    return;
  }

  if (result.error) {
    const show = result.warn
      ? vscode.window.showWarningMessage
      : vscode.window.showErrorMessage;
    void show(`[LazyVerilog] AutoFF: ${result.error}`);
    return;
  }

  if (!result.pairs || result.pairs.length === 0) {
    void vscode.window.showInformationMessage(
      "[LazyVerilog] AutoFF: nothing to insert",
    );
    return;
  }

  const preview = formatAutoffPreview(result);
  const apply = await confirmApplyQuickPick(
    "AutoFF: Insert flip-flop assignments?",
    preview,
    "AutoFF",
  );
  if (!apply) return;

  try {
    const edit = (await client.sendRequest("workspace/executeCommand", {
      command: applyCmd,
      arguments: args,
    })) as LspWorkspaceEdit | null;
    if (edit) {
      const vsEdit =
        await client.protocol2CodeConverter.asWorkspaceEdit(edit);
      await vscode.workspace.applyEdit(vsEdit);
    }
  } catch (e) {
    void vscode.window.showErrorMessage(
      `[LazyVerilog] AutoFF apply: ${(e as Error).message}`,
    );
  }
}

// ---------------------------------------------------------------------------
// Lint helper
// ---------------------------------------------------------------------------

const SEVERITY_LABEL: Record<number, string> = {
  1: "error",
  2: "warning",
  3: "info",
  4: "hint",
};

function lintSeverityLabel(severity: number | string | undefined): string {
  if (typeof severity === "number") {
    return SEVERITY_LABEL[severity] ?? "warning";
  }
  if (typeof severity === "string" && severity.length > 0) {
    const lower = severity.toLowerCase();
    return lower === "information" ? "info" : lower;
  }
  return "warning";
}

function lintMessage(v: Partial<LintViolation>): string {
  return v.message ?? v.text ?? "";
}

async function runLint(
  client: LanguageClient,
  uri: string | undefined,
  label: string,
  progress = false,
): Promise<void> {
  commandLog(`${label}: runLint start`, { uri, progress });

  const run = async () => {
    let violations: LintViolation[];
    try {
      commandLog(`${label}: sending lint request`, {
        command: uri ? "lazyverilog.lint" : "lazyverilog.lintAll",
        arguments: uri ? [uri] : [],
      });
      violations = (await client.sendRequest("workspace/executeCommand", {
        command: uri ? "lazyverilog.lint" : "lazyverilog.lintAll",
        arguments: uri ? [uri] : [],
      })) as LintViolation[];
      commandLog(`${label}: lint response received`, summarizeLintViolations(violations));
    } catch (e) {
      commandLog(`${label}: lint request exception`, {
        message: (e as Error).message,
        stack: (e as Error).stack,
      });
      void vscode.window.showErrorMessage(
        `[LazyVerilog] ${label}: ${(e as Error).message}`,
      );
      return;
    }

    if (!violations || violations.length === 0) {
      commandLog(`${label}: no violations, showing information message`);
      void vscode.window.showInformationMessage(
        `[LazyVerilog] ${label}: no violations found`,
      );
      return;
    }

    commandLog(`${label}: writing violations to output channel`, { count: violations.length });
    const ch = getOutputChannel();
    ch.clear();
    ch.appendLine(`[LazyVerilog] ${label}: ${violations.length} violation(s)\n`);
    for (const raw of violations) {
      // Be tolerant at display time too.  The LSP server should return the
      // LintViolation shape, but a partial item should not crash the command.
      const v = raw as Partial<LintViolation>;
      const sev = lintSeverityLabel(v.severity);
      const file = v.file ?? "";
      const line = v.line ?? 0;
      const col = v.col ?? 0;
      const text = lintMessage(v);
      const klass = v.class ?? "";
      const suffix = klass ? ` [${klass}]` : "";
      ch.appendLine(`${file}:${line}:${col}: ${sev}: ${text}${suffix}`);
    }
    commandLog(`${label}: showing output channel`);
    ch.show(true);
    commandLog(`${label}: showing completion information message`);
    void vscode.window.showInformationMessage(
      `[LazyVerilog] ${label}: ${violations.length} violation(s) — see Output panel`,
    );
  };

  if (progress) {
    commandLog(`${label}: starting progress notification`);
    await vscode.window.withProgress(
      {
        location: vscode.ProgressLocation.Notification,
        title: `[LazyVerilog] ${label}: running…`,
        cancellable: false,
      },
      run,
    );
    commandLog(`${label}: progress notification completed`);
  } else {
    await run();
  }
}

async function confirmLintAll(): Promise<boolean> {
  // Avoid modal showWarningMessage here.  On the same Code OSS/Electron/Wayland
  // setup, Connect crashed when closing a native modal confirmation.  QuickPick
  // stays in the normal workbench UI and gives us a safer explicit confirmation.
  commandLog("LintAll: showing non-modal confirmation QuickPick");
  type ConfirmItem = vscode.QuickPickItem & { action: "continue" | "cancel" };
  const picked = await vscode.window.showQuickPick<ConfirmItem>(
    [
      {
        label: "$(check) Continue",
        description: "Parse and lint all files from the configured filelist",
        detail: "This may take a while.",
        action: "continue",
      },
      {
        label: "$(close) Cancel",
        description: "Do not run Lint All Files",
        action: "cancel",
      },
    ],
    {
      placeHolder: "Lint All Files may take a while",
      ignoreFocusOut: true,
      matchOnDescription: true,
      matchOnDetail: true,
    },
  );
  commandLog("LintAll: confirmation QuickPick returned", { picked: picked?.label });
  return picked?.action === "continue";
}

// ---------------------------------------------------------------------------
// Public registration
// ---------------------------------------------------------------------------

export function registerCommands(
  context: vscode.ExtensionContext,
  client: LanguageClient,
): void {
  // --- AutoFF code-action command handlers ---
  // These are intercepted client-side: show preview, let user confirm, then apply.
  context.subscriptions.push(
    vscode.commands.registerCommand(
      "lazyverilog.autoff",
      async (uri: string, line: number) => {
        await autoffFlow(
          client,
          "lazyverilog.autoffPreview",
          "lazyverilog.autoffApply",
          [uri, line],
        );
      },
    ),
    vscode.commands.registerCommand(
      "lazyverilog.autoffPreview",
      async (uri: string, line: number) => {
        await autoffFlow(
          client,
          "lazyverilog.autoffPreview",
          "lazyverilog.autoffApply",
          [uri, line],
        );
      },
    ),
    vscode.commands.registerCommand(
      "lazyverilog.autoffAll",
      async (uri: string) => {
        await autoffFlow(
          client,
          "lazyverilog.autoffAllPreview",
          "lazyverilog.autoffAllApply",
          [uri],
        );
      },
    ),
    vscode.commands.registerCommand(
      "lazyverilog.autoffAllPreview",
      async (uri: string) => {
        await autoffFlow(
          client,
          "lazyverilog.autoffAllPreview",
          "lazyverilog.autoffAllApply",
          [uri],
        );
      },
    ),

    // --- AutoWire code-action command handler ---
    // Neovim ignores cmd args and reads cursor position directly.
    // We do the same: get active editor position, preview, confirm, apply.
    vscode.commands.registerCommand("lazyverilog.autowire", async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      const uri = editor.document.uri.toString();
      const line = editor.selection.active.line;

      let preview: string[];
      try {
        preview = (await client.sendRequest("workspace/executeCommand", {
          command: "lazyverilog.autowirepreview",
          arguments: [uri, line],
        })) as string[];
      } catch (e) {
        void vscode.window.showErrorMessage(
          `[LazyVerilog] AutoWire: ${(e as Error).message}`,
        );
        return;
      }

      if (!preview || preview.length === 0) {
        void vscode.window.showInformationMessage(
          "[LazyVerilog] AutoWire: nothing to add or update",
        );
        return;
      }

      const apply = await confirmApplyQuickPick(
        "AutoWire: Apply wire edits?",
        preview.join("\n"),
        "AutoWire",
      );
      if (!apply) return;

      try {
        const edit = (await client.sendRequest("workspace/executeCommand", {
          command: "lazyverilog.autowire",
          arguments: [uri, line],
        })) as LspWorkspaceEdit | null;
        if (edit) {
          const vsEdit =
            await client.protocol2CodeConverter.asWorkspaceEdit(edit);
          await vscode.workspace.applyEdit(vsEdit);
        }
      } catch (e) {
        void vscode.window.showErrorMessage(
          `[LazyVerilog] AutoWire apply: ${(e as Error).message}`,
        );
      }
    }),

    // --- Lint command (palette) ---
    vscode.commands.registerCommand("lazyverilog.lintCommand", async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        void vscode.window.showErrorMessage(
          "[LazyVerilog] Lint: no active file",
        );
        return;
      }
      await runLint(client, editor.document.uri.toString(), "Lint");
    }),

    // --- LintAll command (palette) ---
    vscode.commands.registerCommand("lazyverilog.lintAllCommand", async () => {
      commandLog("lazyverilog.lintAllCommand invoked");
      const confirmed = await confirmLintAll();
      if (!confirmed) {
        commandLog("LintAll: cancelled before run");
        return;
      }
      commandLog("LintAll: confirmed, starting runLint");
      await runLint(client, undefined, "LintAll", true);
    }),

    // --- Format command (palette, range-aware) ---
    vscode.commands.registerCommand("lazyverilog.formatCommand", async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      const uri = editor.document.uri.toString();
      const sel = editor.selection;
      const args = sel.isEmpty
        ? [uri, "full"]
        : [uri, "range", sel.start.line, sel.end.line + 1];

      try {
        const edit = (await client.sendRequest("workspace/executeCommand", {
          command: "lazyverilog.format",
          arguments: args,
        })) as LspWorkspaceEdit | null;
        if (edit) {
          const vsEdit =
            await client.protocol2CodeConverter.asWorkspaceEdit(edit);
          await vscode.workspace.applyEdit(vsEdit);
        }
      } catch (e) {
        void vscode.window.showErrorMessage(
          `[LazyVerilog] Format: ${(e as Error).message}`,
        );
      }
    }),
  );

  // --- renameUnresolved server→client notification ---
  client.onNotification(
    "lazyverilog/renameUnresolved",
    (params: { locations: string[] }) => {
      if (!params?.locations?.length) return;
      const ch = getOutputChannel();
      ch.clear();
      ch.appendLine(
        "[LazyVerilog] Rename applied. Unresolved locations (manual update needed):\n",
      );
      for (const loc of params.locations) {
        ch.appendLine(`  ${loc}`);
      }
      ch.show(true);
      void vscode.window.showWarningMessage(
        `[LazyVerilog] Rename: ${params.locations.length} unresolved location(s) — see Output panel`,
      );
    },
  );
}
