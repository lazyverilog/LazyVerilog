# Install for VS Code

Install LazyVerilog from the
[Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=lazyverilog.lazyverilog-vscode),
then open a `.sv`, `.svh`, `.v`, or `.vh` file. The extension downloads the server on first use.

## Use your own server binary

```json
{
  "lazyverilog.serverPath": "/path/to/lazyverilog-lsp"
}
```

## Install from a VSIX

If the Marketplace version is not available yet, download the `.vsix` from the latest
[GitHub Release](https://github.com/lazyverilog/LazyVerilog/releases/latest), then:

1. Press `Ctrl+Shift+P`.
2. Run `Extensions: Install from VSIX...`.
3. Select the downloaded file.

Next: [Usage in VS Code](../usage/vscode.md).
