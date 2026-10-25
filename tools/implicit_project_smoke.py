#!/usr/bin/env python3
"""End-to-end check of implicit projects: a workspace folder with no
lazyverilog.toml is indexed anyway.

Drives a real `lazyverilog-lsp` over stdio against a throwaway tree:

    ws/rtl/top.sv     `include "defs.svh"; instantiates `sub` twice
    ws/rtl/sub.sv     declares `sub` with a parameter and ports (never opened)
    ws/inc/defs.svh   the header, in a directory no config names

With the folder sent as a workspace folder, the features that read the unopened
sub.sv must answer on the instances in top.sv: go-to-definition, hover,
AutoInst, named-port completion and parameter signature help, and the include
must resolve.  `lazyverilog.rescan`, sent many times, must keep at most one scan
worker per root.  With `implicitProject = "off"` none of it happens, as before
implicit projects existed.  Either way nothing may be written into the tree:
shards go to the user cache, which this script points at a temporary directory.

    tools/implicit_project_smoke.py --binary build/lazyverilog-lsp
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
from lsp_client import Client  # noqa: E402

TOP = ('`include "defs.svh"\n'
       'module top;\n'
       '  sub u_sub (/*AUTOINST*/);\n'
       '  sub #(.W(4)) u_p (.\n'
       '  );\n'
       '  wire [`WIDTH-1:0] w;\n'
       'endmodule\n')
SUB = ('module sub #(parameter int W = 8) (\n'
       '  input logic clk,\n'
       '  output logic dout\n'
       ');\nendmodule\n')
RESCANS = 20


def run(binary: str, ws: Path, mode: str | None, timeout: float) -> dict:
    """Open top.sv and ask each cross-file feature about `sub`."""
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

    def at(line: int, character: int) -> dict:
        return {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}}

    def reaches_sub() -> bool:
        _, msg = client.request("textDocument/definition", at(2, 3))
        result = msg.get("result") or []
        if isinstance(result, dict):
            result = [result]
        return any((r.get("uri") or r.get("targetUri") or "").endswith("/sub.sv") for r in result)

    # The folder is scanned on a background thread; poll until definition
    # answers or the deadline passes (the "off" run always waits it out).
    out = {"definition": False}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if reaches_sub():
            out["definition"] = True
            break
        time.sleep(0.25)
    client.wait_quiet(1.0, cap=10.0)

    _, msg = client.request("textDocument/hover", at(2, 3))
    out["hover"] = str(((msg.get("result") or {}).get("contents") or {}).get("value", ""))
    _, msg = client.request("textDocument/codeAction", {
        "textDocument": {"uri": uri},
        "range": {"start": {"line": 2, "character": 4}, "end": {"line": 2, "character": 4}},
        "context": {"diagnostics": []}})
    out["autoinst"] = "".join(
        edit.get("newText", "")
        for action in msg.get("result") or [] if action.get("title", "").startswith("AutoInst")
        for edits in ((action.get("edit") or {}).get("changes") or {}).values()
        for edit in edits)
    _, msg = client.request("textDocument/completion", at(3, 21))
    result = msg.get("result") or {}
    items = result.get("items", []) if isinstance(result, dict) else result
    out["ports"] = sorted(item.get("label", "") for item in items if item.get("label", "").startswith("."))
    _, msg = client.request("textDocument/signatureHelp", at(3, 8))
    out["signature"] = " ".join(s.get("label", "") for s in (msg.get("result") or {}).get("signatures", []))

    if mode != "off":
        # Back to back, so most arrive while a scan is still running.
        for i in range(RESCANS):
            client.request("workspace/executeCommand",
                           {"command": "lazyverilog.rescan", "arguments": []})
            if i % 5 == 4:
                client.wait_quiet(0.5, cap=10.0)
        client.wait_quiet(1.0, cap=10.0)
        out["after_rescan"] = reaches_sub()

    with client.cv:
        published = [params for _, method, params in client.notifications
                     if method == "textDocument/publishDiagnostics" and params
                     and params.get("uri") == uri]
    # Only the latest publish for the file counts.
    out["messages"] = ([d.get("message", "") for d in published[-1].get("diagnostics", [])]
                       if published else [])

    client.request("shutdown", None, timeout=10)
    client.notify("exit", None)
    try:
        client.proc.wait(timeout=10)
    except Exception:
        client.proc.kill()
    client.stderr_thread.join(timeout=10)
    joined = re.findall(rb"joining (\d+) implicit scan workers", client.stderr_tail)
    out["workers"] = int(joined[-1]) if joined else None
    return out


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
        (ws / "rtl" / "sub.sv").write_text(SUB)
        (ws / "inc" / "defs.svh").write_text("`define WIDTH 8\n")
        cache = tmp / "cache"
        cache.mkdir()
        # Keep the shards out of the real user cache on every platform.
        os.environ["LOCALAPPDATA"] = str(cache)
        os.environ["XDG_CACHE_HOME"] = str(cache)
        if sys.platform == "darwin":
            os.environ["HOME"] = str(cache)  # user cache: $HOME/Library/Caches

        failures = []
        auto = run(args.binary, ws, None, args.timeout)
        if not auto["definition"]:
            failures.append("auto: definition of `sub` did not reach the unopened sub.sv")
        if "dout" not in auto["hover"]:
            failures.append(f"auto: hover on `sub` does not list its ports: {auto['hover']!r}")
        if ".dout(dout)" not in auto["autoinst"]:
            failures.append(f"auto: AutoInst did not expand `sub`'s ports: {auto['autoinst']!r}")
        if auto["ports"] != [".clk", ".dout"]:
            failures.append(f"auto: named-port completion gave {auto['ports']}")
        if "parameter int W" not in auto["signature"]:
            failures.append(f"auto: parameter signature help gave {auto['signature']!r}")
        if any("defs.svh" in m for m in auto["messages"]):
            failures.append(f"auto: the include did not resolve: {auto['messages']}")
        if not auto["after_rescan"]:
            failures.append("auto: definition stopped reaching sub.sv after rescans")
        if auto["workers"] is None or auto["workers"] > 1:
            failures.append(f"auto: {RESCANS} rescans of one root left {auto['workers']} "
                            "scan workers at shutdown (want at most 1)")

        off = run(args.binary, ws, "off", 3.0)
        if off["definition"]:
            failures.append("off: definition reached sub.sv with implicit projects off")
        if off["hover"] or off["autoinst"] or off["ports"]:
            failures.append(f"off: sub.sv answered with implicit projects off: {off}")
        if not any("defs.svh" in m for m in off["messages"]):
            failures.append(f"off: expected an unresolved-include diagnostic, got {off['messages']}")

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
