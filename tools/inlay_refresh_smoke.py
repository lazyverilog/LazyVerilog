#!/usr/bin/env python3
"""End-to-end check of when the server sends workspace/inlayHint/refresh.

  refreshSupport offered: inlayHint answers from the last parsed tree while a
      typing burst is being parsed, and the server sends one refresh once the
      parse queue goes idle -- not one per parse.  The client re-requests on
      it, as Neovim does, and that answer matches the settled hints.
  refreshSupport not offered: the server never sends the refresh (the client
      may not handle it), and inlayHint waits for the parse instead.

    tools/inlay_refresh_smoke.py --binary build/lazyverilog-lsp
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
from lsp_client import Client  # noqa: E402

KEYSTROKES = 12
# Enough instances that each reparse is still running when its inlayHint
# arrives, which is the window the refresh exists for.
TOP = ("module child (input logic a, output logic b);\n"
       "    assign b = a;\n"
       "endmodule\n"
       "module top;\n"
       "    logic x, y;\n"
       "    child u_child (.a(x), .b(y));\n"
       "    // typing here\n"
       + "".join(f"    child u_{i} (.a(x), .b());\n" for i in range(4000)) +
       "endmodule\n")


def refreshes(client: Client) -> int:
    with client.cv:
        return sum(1 for _, method, _ in client.notifications
                   if method == "workspace/inlayHint/refresh")


def run(binary: str, root: Path, refresh_support: bool) -> list[str]:
    path = root / "top.sv"
    uri = path.as_uri()
    caps = {"textDocument": {"inlayHint": {}}}
    if refresh_support:
        caps["workspace"] = {"inlayHint": {"refreshSupport": True}}
    client = Client([str(Path(binary).resolve())], str(root), capture_stderr=True)
    errors = []
    try:
        client.request("initialize", {"processId": os.getpid(), "rootUri": root.as_uri(),
                                      "capabilities": caps})
        client.notify("initialized", {})
        client.notify("textDocument/didOpen", {"textDocument": {
            "uri": uri, "languageId": "systemverilog", "version": 1, "text": TOP}})
        client.wait_quiet(1.5)
        hint_params = {"textDocument": {"uri": uri},
                       "range": {"start": {"line": 0, "character": 0},
                                 "end": {"line": 9, "character": 0}}}
        if refresh_support:
            client.refresh_params = hint_params
        before = refreshes(client)

        text = TOP
        for k in range(KEYSTROKES):
            text = text.replace("// typing here", "// typing here ", 1)
            client.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": 2 + k},
                "contentChanges": [{"text": text}]})
            client.request("textDocument/inlayHint", hint_params)
            time.sleep(0.02)
        client.wait_quiet(1.5)
        sent = refreshes(client) - before
        _, settled = client.request("textDocument/inlayHint", hint_params)
        settled = settled.get("result") or []

        mode = "refreshSupport" if refresh_support else "no refreshSupport"
        print(f"{mode}: {sent} refresh(es) for a {KEYSTROKES}-keystroke burst, "
              f"{client.refresh_rerequests} re-request(s), {len(settled)} settled hints")
        if not settled:
            errors.append(f"{mode}: no hints at all; the check proves nothing")
        if refresh_support:
            # One for the burst; a project-index publish may add one more.
            if not 1 <= sent <= 2:
                errors.append(f"{mode}: expected one refresh per burst, got {sent}")
            last = f"refresh-{client.refresh_rerequests}"
            with client.cv:
                reply = client.responses.get(last, (0, {}))[1]
            if (reply.get("result") or []) != settled:
                errors.append(f"{mode}: the re-requested hints differ from the settled ones")
        elif sent:
            errors.append(f"{mode}: refresh sent to a client that did not offer it")
    finally:
        try:
            client.request("shutdown", None, timeout=10)
            client.notify("exit", None)
            client.proc.wait(timeout=10)
        except Exception:
            client.proc.kill()
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    args = ap.parse_args()
    root = Path(tempfile.mkdtemp(prefix="lazyverilog-inlay-refresh-"))
    try:
        (root / "top.sv").write_text(TOP)
        (root / "files.f").write_text("top.sv\n")
        (root / "lazyverilog.toml").write_text('[design]\nvcode = "files.f"\n')
        errors = run(args.binary, root, True) + run(args.binary, root, False)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    for error in errors:
        print("FAIL:", error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
