import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
} from "vscode-languageclient/node";
import { resolveServerPath, autoInstall } from "./installer";
import { createConfigWatcher, clearAllDebounceHandles } from "./watcher";
import { registerCommands } from "./commands";
import { registerRtlTree } from "./rtltree";

let client: LanguageClient | undefined;

export async function activate(
  context: vscode.ExtensionContext,
): Promise<void> {
  const config = vscode.workspace.getConfiguration();

  let serverPath = resolveServerPath(config, context);
  if (!serverPath) {
    try {
      serverPath = await autoInstall(context);
    } catch {
      // Error already shown to the user by autoInstall
      return;
    }
  }

  const serverOptions: ServerOptions = { command: serverPath };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: "file", language: "systemverilog" },
      { scheme: "file", language: "verilog" },
    ],
    synchronize: {
      configurationSection: "lazyverilog",
    },
  };

  client = new LanguageClient(
    "lazyverilog",
    "LazyVerilog",
    serverOptions,
    clientOptions,
  );

  await client.start();

  registerCommands(context, client);
  registerRtlTree(context, client);

  // Set up per-workspace-folder lazyverilog.toml watchers
  for (const folder of vscode.workspace.workspaceFolders ?? []) {
    const watcher = createConfigWatcher(client, folder);
    context.subscriptions.push(watcher);
  }
}

export async function deactivate(): Promise<void> {
  clearAllDebounceHandles();
  if (client) {
    await client.stop();
    client = undefined;
  }
}
