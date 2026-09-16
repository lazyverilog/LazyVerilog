import * as vscode from "vscode";
import { LanguageClient } from "vscode-languageclient/node";

const DEBOUNCE_MS = 150;

// Per-workspace-folder debounce handles. Cleared on deactivate.
const debounceHandles = new Map<string, ReturnType<typeof setTimeout>>();

export function clearAllDebounceHandles(): void {
  for (const handle of debounceHandles.values()) {
    clearTimeout(handle);
  }
  debounceHandles.clear();
}

export function createConfigWatcher(
  client: LanguageClient,
  workspaceFolder: vscode.WorkspaceFolder,
): vscode.Disposable {
  const folderUri = workspaceFolder.uri.toString();
  // Nested configs matter now: the server resolves each file to the nearest
  // lazyverilog.toml above it, so a monorepo can have one per sub-project.
  // Watching only the folder root would miss every one of them, and the edit
  // would appear not to take effect until a restart.
  const pattern = new vscode.RelativePattern(workspaceFolder, "**/lazyverilog.toml");
  const watcher = vscode.workspace.createFileSystemWatcher(pattern);

  function notify(uri: vscode.Uri): void {
    const existing = debounceHandles.get(folderUri);
    if (existing !== undefined) {
      clearTimeout(existing);
    }
    const handle = setTimeout(() => {
      debounceHandles.delete(folderUri);
      void client.sendNotification("workspace/didChangeConfiguration", {
        settings: {
          lazyverilog: {
            configFile: uri.fsPath,
            reason: "lazyverilog.toml changed",
          },
        },
      });
    }, DEBOUNCE_MS);
    debounceHandles.set(folderUri, handle);
  }

  watcher.onDidChange(notify);
  watcher.onDidCreate(notify);
  watcher.onDidDelete((uri) => {
    const existing = debounceHandles.get(folderUri);
    if (existing !== undefined) {
      clearTimeout(existing);
      debounceHandles.delete(folderUri);
    }
    void client.sendNotification("workspace/didChangeConfiguration", {
      settings: {
        lazyverilog: {
          configFile: uri.fsPath,
          reason: "lazyverilog.toml deleted",
        },
      },
    });
  });

  return {
    dispose(): void {
      const handle = debounceHandles.get(folderUri);
      if (handle !== undefined) {
        clearTimeout(handle);
        debounceHandles.delete(folderUri);
      }
      watcher.dispose();
    },
  };
}
