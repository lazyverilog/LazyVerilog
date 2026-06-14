import * as vscode from "vscode";
import { LanguageClient } from "vscode-languageclient/node";

// ---------------------------------------------------------------------------
// Server response types
// ---------------------------------------------------------------------------

interface RtlNode {
  name: string;
  inst?: string;
  file?: string;
  line?: number;
  col?: number;
  recursive?: boolean;
  unknown?: boolean;
  children?: RtlNode[];
}

// ---------------------------------------------------------------------------
// TreeItem
// ---------------------------------------------------------------------------

class RtlTreeItem extends vscode.TreeItem {
  constructor(
    readonly node: RtlNode,
    readonly collapsibleState: vscode.TreeItemCollapsibleState,
  ) {
    super(node.name, collapsibleState);

    // Label: show instance name in parens when it differs from module name
    if (node.inst && node.inst !== node.name) {
      this.label = `${node.name} (${node.inst})`;
    }

    if (node.recursive) {
      this.description = "<recursive>";
    } else if (node.unknown) {
      this.description = "<unknown>";
    }

    if (node.file) {
      const filePath = node.file.replace(/^file:\/\//, "");
      this.tooltip = filePath;
      if (node.line && node.line > 0) {
        this.command = {
          command: "lazyverilog.rtlTreeJump",
          title: "Jump to definition",
          arguments: [filePath, node.line - 1, node.col ?? 0],
        };
      }
    }

    this.contextValue = "rtlNode";
  }
}

// ---------------------------------------------------------------------------
// TreeDataProvider
// ---------------------------------------------------------------------------

class RtlTreeDataProvider
  implements vscode.TreeDataProvider<RtlTreeItem>
{
  private _onDidChangeTreeData = new vscode.EventEmitter<
    RtlTreeItem | undefined | void
  >();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

  private root: RtlNode | undefined;
  private activeFilePath: string | undefined;

  constructor(private readonly client: LanguageClient) {}

  get lastCommand(): string {
    return this._lastCommand;
  }
  private _lastCommand = "lazyverilog.rtlTree";
  private _lastUri: string | undefined;

  async load(uri: string, command: string): Promise<void> {
    this._lastUri = uri;
    this._lastCommand = command;
    let result: RtlNode | null;
    try {
      result = (await this.client.sendRequest("workspace/executeCommand", {
        command,
        arguments: [uri],
      })) as RtlNode | null;
    } catch (e) {
      void vscode.window.showErrorMessage(
        `[LazyVerilog] RtlTree: ${(e as Error).message}`,
      );
      return;
    }
    if (!result) {
      void vscode.window.showWarningMessage(
        "[LazyVerilog] RtlTree: no hierarchy found",
      );
      return;
    }
    this.root = result;
    this.fire();
  }

  async refresh(): Promise<void> {
    if (this._lastUri) await this.load(this._lastUri, this._lastCommand);
  }

  setActiveFile(filePath: string | undefined): void {
    this.activeFilePath = filePath;
    this.fire();
  }

  private fire(): void {
    this._onDidChangeTreeData.fire();
  }

  // ---- TreeDataProvider ---------------------------------------------------

  getTreeItem(element: RtlTreeItem): vscode.TreeItem {
    if (
      this.activeFilePath &&
      element.node.file &&
      element.node.file.replace(/^file:\/\//, "") === this.activeFilePath
    ) {
      element.iconPath = new vscode.ThemeIcon(
        "circle-filled",
        new vscode.ThemeColor("charts.blue"),
      );
    } else {
      element.iconPath = undefined;
    }
    return element;
  }

  getChildren(element?: RtlTreeItem): vscode.ProviderResult<RtlTreeItem[]> {
    if (!this.root) return [];
    const node = element ? element.node : this.root;

    if (!element) {
      // Return the root as the single top-level item
      return [
        new RtlTreeItem(
          this.root,
          (this.root.children?.length ?? 0) > 0
            ? vscode.TreeItemCollapsibleState.Expanded
            : vscode.TreeItemCollapsibleState.None,
        ),
      ];
    }

    return (node.children ?? []).map(
      (child) =>
        new RtlTreeItem(
          child,
          (child.children?.length ?? 0) > 0
            ? vscode.TreeItemCollapsibleState.Expanded
            : vscode.TreeItemCollapsibleState.None,
        ),
    );
  }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

export function registerRtlTree(
  context: vscode.ExtensionContext,
  client: LanguageClient,
): void {
  const provider = new RtlTreeDataProvider(client);

  const treeView = vscode.window.createTreeView("lazyverilog.rtlTree", {
    treeDataProvider: provider,
    showCollapseAll: true,
  });

  // Sync highlight when active editor changes
  const onEditorChange = vscode.window.onDidChangeActiveTextEditor((editor) => {
    provider.setActiveFile(editor?.document.uri.fsPath);
  });

  // ---- lazyverilog.rtlTree command ----------------------------------------
  const rtlTreeCmd = vscode.commands.registerCommand(
    "lazyverilog.rtlTree",
    async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        void vscode.window.showErrorMessage(
          "[LazyVerilog] RtlTree: no active file",
        );
        return;
      }
      const uri = editor.document.uri.toString();
      await vscode.commands.executeCommand(
        "lazyverilog.rtlTree.focus",
      );
      await provider.load(uri, "lazyverilog.rtlTree");
    },
  );

  // ---- lazyverilog.rtlTreeReverse command ---------------------------------
  const rtlTreeRevCmd = vscode.commands.registerCommand(
    "lazyverilog.rtlTreeReverse",
    async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        void vscode.window.showErrorMessage(
          "[LazyVerilog] RtlTree: no active file",
        );
        return;
      }
      const uri = editor.document.uri.toString();
      await vscode.commands.executeCommand(
        "lazyverilog.rtlTree.focus",
      );
      await provider.load(uri, "lazyverilog.rtlTreeReverse");
    },
  );

  // ---- refresh button (shown in tree view title bar) ----------------------
  const refreshCmd = vscode.commands.registerCommand(
    "lazyverilog.rtlTreeRefresh",
    async () => {
      await provider.refresh();
    },
  );

  // ---- jump to definition on item click -----------------------------------
  const jumpCmd = vscode.commands.registerCommand(
    "lazyverilog.rtlTreeJump",
    async (filePath: string, line: number, col: number) => {
      const uri = vscode.Uri.file(filePath);
      const doc = await vscode.workspace.openTextDocument(uri);
      const editor = await vscode.window.showTextDocument(doc);
      const position = new vscode.Position(line, col);
      editor.selection = new vscode.Selection(position, position);
      editor.revealRange(
        new vscode.Range(position, position),
        vscode.TextEditorRevealType.InCenter,
      );
    },
  );

  context.subscriptions.push(
    treeView,
    onEditorChange,
    rtlTreeCmd,
    rtlTreeRevCmd,
    refreshCmd,
    jumpCmd,
  );
}
