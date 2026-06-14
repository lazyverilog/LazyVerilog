import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
} from "vscode-languageclient/node";
import { resolveServerPath, autoInstall } from "./installer";
import { createConfigWatcher, clearAllDebounceHandles } from "./watcher";
import { ExecuteCommandRequest } from "vscode-languageserver-protocol";
import { registerCommands } from "./commands";
import { registerRtlTree } from "./rtltree";
import { registerInterface } from "./interface";
import { registerConnect } from "./connect";

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

  // LazyVerilog has a few commands that are intentionally handled by the VS Code
  // extension instead of being forwarded directly to the language server.  For
  // example, AutoFF needs a client-side confirmation preview before the server's
  // apply command is requested, and the RTL hierarchy commands update a VS Code
  // tree view.
  //
  // vscode-languageclient automatically registers every command advertised by
  // the server's executeCommandProvider.  The server advertises command names
  // such as `lazyverilog.autoffPreview`, `lazyverilog.autowire`, and
  // `lazyverilog.rtlTree` for non-VS-Code clients too.  If the language client
  // auto-registers those names first, our client-side registerCommand calls fail
  // activation with errors like:
  //
  //   command 'lazyverilog.autoffPreview' already exists
  //
  // We still use workspace/executeCommand explicitly via client.sendRequest(...)
  // throughout the extension, so disabling only the automatic command-registry
  // feature keeps server functionality available while preserving the richer
  // VS Code UX command handlers.
  //
  // LanguageClient stores built-in features in an internal list at construction
  // time.  There is no public unregister API for a single built-in feature, so
  // remove only the executeCommand feature before installing a no-op replacement.
  // Keep this narrowly scoped: all other language features remain untouched.
  const clientWithFeatures = client as unknown as {
    _features?: Array<{ registrationType?: { method: string } }>;
  };
  if (clientWithFeatures._features) {
    clientWithFeatures._features = clientWithFeatures._features.filter(
      (feature) =>
        feature.registrationType?.method !== ExecuteCommandRequest.type.method,
    );
  }

  client.registerFeature({
    getState: () => ({
      kind: "workspace",
      id: ExecuteCommandRequest.type.method,
      registrations: false,
    }),
    get registrationType() {
      return ExecuteCommandRequest.type;
    },
    fillClientCapabilities: () => {
      // Do not advertise dynamic executeCommand registration support. The
      // extension sends executeCommand requests itself when its UI commands need
      // server work.
    },
    initialize: () => {
      // Intentionally ignore the server's executeCommandProvider command list so
      // VS Code command names remain owned by this extension's UI handlers.
    },
    register: () => {
      // Dynamic registrations are ignored for the same reason as initialize().
    },
    unregister: () => {
      // Nothing was registered.
    },
    clear: () => {
      // Nothing was registered.
    },
  });

  await client.start();

  registerCommands(context, client);
  registerRtlTree(context, client);
  registerInterface(context, client);
  registerConnect(context, client);

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
