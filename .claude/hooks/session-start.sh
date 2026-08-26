#!/bin/bash
# SessionStart hook: makes a modern Neovim + the built lazyverilog-lsp binary
# available in Claude Code on the web sessions, so the nvim plugin (which
# needs Neovim >= 0.10 for vim.lsp.get_clients) can be exercised against a
# real, freshly-built LSP server.
#
# Local/CLI sessions are left untouched — a developer's own machine already
# has whatever nvim/build setup they use.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

REPO_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
NVIM_MIN_MAJOR=0
NVIM_MIN_MINOR=10
NVIM_INSTALL_DIR="/opt/nvim-linux-x86_64"
NVIM_BIN="$NVIM_INSTALL_DIR/bin/nvim"

nvim_version_ok() {
  command -v nvim >/dev/null 2>&1 || return 1
  local ver major minor
  ver=$(nvim --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
  major=$(echo "$ver" | cut -d. -f1)
  minor=$(echo "$ver" | cut -d. -f2)
  [ "$major" -gt "$NVIM_MIN_MAJOR" ] && return 0
  [ "$major" -eq "$NVIM_MIN_MAJOR" ] && [ "$minor" -ge "$NVIM_MIN_MINOR" ]
}

if ! nvim_version_ok; then
  echo "[session-start] installing Neovim (>= 0.${NVIM_MIN_MINOR}) for the lazyverilog nvim plugin"
  tmp_tarball="$(mktemp -d)/nvim-linux-x86_64.tar.gz"
  curl -fsSL -o "$tmp_tarball" \
    https://github.com/neovim/neovim/releases/latest/download/nvim-linux-x86_64.tar.gz
  rm -rf "$NVIM_INSTALL_DIR"
  mkdir -p "$NVIM_INSTALL_DIR"
  tar xzf "$tmp_tarball" -C "$(dirname "$NVIM_INSTALL_DIR")"
  ln -sf "$NVIM_BIN" /usr/local/bin/nvim
  rm -rf "$(dirname "$tmp_tarball")"
fi

echo "[session-start] $(nvim --version | head -1)"

echo "[session-start] configuring + building lazyverilog-lsp"
cmake -B "$REPO_DIR/build" -DCMAKE_BUILD_TYPE=Release -S "$REPO_DIR"
cmake --build "$REPO_DIR/build" -j"$(nproc)" --target lazyverilog-lsp

echo "[session-start] done: $REPO_DIR/build/lazyverilog-lsp"

# nvim-mcp gives Claude an MCP server that can drive a running Neovim
# instance (buffers, diagnostics, RPC) instead of only shelling out to nvim
# headlessly. The server is registered project-wide via .mcp.json (trusted
# automatically by .claude/settings.json's enableAllProjectMcpServers), but
# the `nvim-mcp` binary itself isn't part of the repo, so each fresh
# container needs it built.
if command -v nvim-mcp >/dev/null 2>&1; then
  echo "[session-start] nvim-mcp already installed: $(command -v nvim-mcp)"
elif command -v cargo >/dev/null 2>&1; then
  echo "[session-start] installing nvim-mcp (cargo install nvim-mcp)"
  cargo install nvim-mcp
else
  echo "[session-start] WARNING: cargo not found, skipping nvim-mcp install" >&2
fi
