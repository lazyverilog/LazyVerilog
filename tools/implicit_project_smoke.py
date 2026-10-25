#!/usr/bin/env python3
"""End-to-end check of implicit projects: a workspace folder with no
lazyverilog.toml is indexed anyway.

Drives a real `lazyverilog-lsp` over stdio against a throwaway tree:

    ws/rtl/top.sv     `include "defs.svh"; instantiates `sub`
    ws/rtl/sub.sv     declares `sub` (never opened)
    ws/inc/defs.svh   the header, in a directory no config names

With the folder sent as a workspace folder, go-to-definition on `sub` must reach
the unopened sub.sv and the include must resolve.  With
`implicitProject = "off"` neither happens, as before implicit projects existed.
Either way nothing may be written into the tree: shards go to the user cache,
which this script points at a temporary directory.

    tools/implicit_project_smoke.py --binary build/lazyverilog-lsp
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

TOP = '`include "defs.svh"\nmodule top;\n  sub u_sub ();\n  wire [`WIDTH-1:0] w;\nendmodule\n'


def run(binary: str, ws: Path, mode: str | None, timeout: float) -> tuple[bool, list[str]]:
    """Open top.sv; return (definition reached sub.sv, diagnostic messages)."""
    top = ws / "rtl" / "top.sv"
    uri = top.as_uri()
    client = Client([str(Path(binary).resolve())], str(ws), capture_stderr=True)
    options = {"lazyverilog": {"implicitProject": mode}} if mode else None
    client.request("initialize", {
        "processId": os.getpid(),
        "rootUri": ws.as_uri(),
        "workspaceFolders": [{"uri": ws.as_uri(), "name": ws.name}],
        "initializationOptions": options,
        "capabilities": {"textDocument": {"publishDiagnostics": {}},
                         "workspace": {"workspaceFolders": True}},
    })
    client.notify("initialized", {})
    client.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "systemverilog", "version": 1, "text": TOP}})

    # The folder is scanned on a background thread; poll until definition
    # answers or the deadline passes (the "off" run always waits it out).
    reached = False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        _, msg = client.request("textDocument/definition", {
            "textDocument": {"uri": uri}, "position": {"line": 2, "character": 3}})
        result = msg.get("result") or []
        if isinstance(result, dict):
            result = [result]
        if any((r.get("uri") or r.get("targetUri") or "").endswith("/sub.sv") for r in result):
            reached = True
            break
        time.sleep(0.25)
    client.wait_quiet(1.0, cap=10.0)

    with client.cv:
        published = [params for _, method, params in client.notifications
                     if method == "textDocument/publishDiagnostics" and params
                     and params.get("uri") == uri]
    # Only the latest publish for the file counts.
    messages = [d.get("message", "") for d in published[-1].get("diagnostics", [])] if published else []

    client.request("shutdown", None, timeout=10)
    client.notify("exit", None)
    try:
        client.proc.wait(timeout=10)
    except Exception:
        client.proc.kill()
    return reached, messages


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", required=True)
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="lazyverilog-implicit-smoke-")).resolve()
    try:
        ws = tmp / "ws"
        (ws / "rtl").mkdir(parents=True)
        (ws / "inc").mkdir()
        (ws / "rtl" / "top.sv").write_text(TOP)
        (ws / "rtl" / "sub.sv").write_text("module sub;\nendmodule\n")
        (ws / "inc" / "defs.svh").write_text("`define WIDTH 8\n")
        cache = tmp / "cache"
        cache.mkdir()
        # Keep the shards out of the real user cache on every platform.
        os.environ["LOCALAPPDATA"] = str(cache)
        os.environ["XDG_CACHE_HOME"] = str(cache)
        if sys.platform == "darwin":
            os.environ["HOME"] = str(cache)  # user cache: $HOME/Library/Caches

        failures = []
        reached, messages = run(args.binary, ws, None, args.timeout)
        if not reached:
            failures.append("auto: definition of `sub` did not reach the unopened sub.sv")
        if any("defs.svh" in m for m in messages):
            failures.append(f"auto: the include did not resolve: {messages}")

        reached, messages = run(args.binary, ws, "off", 3.0)
        if reached:
            failures.append("off: definition reached sub.sv with implicit projects off")
        if not any("defs.svh" in m for m in messages):
            failures.append(f"off: expected an unresolved-include diagnostic, got {messages}")

        written = [str(p) for p in ws.rglob(".cache")]
        if written:
            failures.append(f"the server wrote into the tree: {written}")
        if not any(cache.rglob("root.txt")):
            failures.append("auto: no implicit shard directory in the user cache")

        for failure in failures:
            print("FAIL:", failure)
        if not failures:
            print("implicit project smoke: ok")
        return 1 if failures else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
