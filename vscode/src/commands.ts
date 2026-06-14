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
  severity: number; // 1=error, 2=warning, 3=info, 4=hint
  file: string;
  line: number;
  col: number;
  text: string;
  class: string;
}

// ---------------------------------------------------------------------------
// Shared output channel
// ---------------------------------------------------------------------------

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
  const answer = await vscode.window.showInformationMessage(
    `AutoFF: Insert flip-flop assignments?\n\n${preview}`,
    { modal: true },
    "Apply",
  );
  if (answer !== "Apply") return;

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

async function runLint(
  client: LanguageClient,
  uri: string | undefined,
  label: string,
  progress = false,
): Promise<void> {
  const run = async () => {
    let violations: LintViolation[];
    try {
      violations = (await client.sendRequest("workspace/executeCommand", {
        command: uri ? "lazyverilog.lint" : "lazyverilog.lintAll",
        arguments: uri ? [uri] : [],
      })) as LintViolation[];
    } catch (e) {
      void vscode.window.showErrorMessage(
        `[LazyVerilog] ${label}: ${(e as Error).message}`,
      );
      return;
    }

    if (!violations || violations.length === 0) {
      void vscode.window.showInformationMessage(
        `[LazyVerilog] ${label}: no violations found`,
      );
      return;
    }

    const ch = getOutputChannel();
    ch.clear();
    ch.appendLine(`[LazyVerilog] ${label}: ${violations.length} violation(s)\n`);
    for (const v of violations) {
      const sev = SEVERITY_LABEL[v.severity] ?? "warning";
      ch.appendLine(`${v.file}:${v.line}:${v.col}: ${sev}: ${v.text} [${v.class}]`);
    }
    ch.show(true);
    void vscode.window.showInformationMessage(
      `[LazyVerilog] ${label}: ${violations.length} violation(s) — see Output panel`,
    );
  };

  if (progress) {
    await vscode.window.withProgress(
      {
        location: vscode.ProgressLocation.Notification,
        title: `[LazyVerilog] ${label}: running…`,
        cancellable: false,
      },
      run,
    );
  } else {
    await run();
  }
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

      const answer = await vscode.window.showInformationMessage(
        preview.join("\n"),
        { modal: true },
        "Apply",
      );
      if (answer !== "Apply") return;

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
      const answer = await vscode.window.showWarningMessage(
        "LintAll will synchronously parse and lint all .f files. This may take a while.",
        { modal: true },
        "Continue",
      );
      if (answer !== "Continue") return;
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
