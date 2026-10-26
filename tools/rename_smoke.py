#!/usr/bin/env python3
"""End-to-end check of rename through the server, where the asking file's
project is resolved -- none of it is reachable from the in-process tests.

  sim/lazyverilog.toml + files.f listing ../rtl/leaf.sv: renaming `leaf` from
      sim/tb.sv is offered and edits both files.  A check that the declaration
      sat under the toml's directory used to refuse it.
  an implicit project declaring module `m` twice: rename is refused with a
      RequestFailed error that carries the request's id.  The handler used to
      return a response with no result, which the transport could not
      serialize, so the client never got an answer.

    tools/rename_smoke.py --binary build/lazyverilog-lsp
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


def session(binary: str, root: Path, opened: list[Path]) -> Client:
    client = Client([str(Path(binary).resolve())], str(root), capture_stderr=True)
    client.request("initialize", {
        "processId": os.getpid(),
        "rootUri": root.as_uri(),
        "workspaceFolders": [{"uri": root.as_uri(), "name": root.name}],
        "capabilities": {"textDocument": {"rename": {"prepareSupport": True}},
                         "workspace": {"workspaceFolders": True}},
    })
    client.notify("initialized", {})
    for path in opened:
        client.notify("textDocument/didOpen", {"textDocument": {
            "uri": path.as_uri(), "languageId": "systemverilog", "version": 1,
            "text": path.read_text()}})
    return client


def close(client: Client) -> None:
    try:
        client.request("shutdown", None, timeout=10)
        client.notify("exit", None)
        client.proc.wait(timeout=10)
    except Exception:
        client.proc.kill()


def at(path: Path, line: int, character: int, **extra) -> dict:
    return {"textDocument": {"uri": path.as_uri()},
            "position": {"line": line, "character": character}, **extra}


def poll(deadline: float, ask):
    """ask() until it returns something truthy or the deadline passes."""
    while True:
        answer = ask()
        if answer or time.monotonic() > deadline:
            return answer
        time.sleep(0.25)


def leaf_outside_toml_dir(binary: str, base: Path, timeout: float) -> list[str]:
    sim, rtl = base / "sim", base / "rtl"
    sim.mkdir(parents=True)
    rtl.mkdir()
    (sim / "lazyverilog.toml").write_text('[design]\nvcode = "files.f"\n')
    (sim / "files.f").write_text("tb.sv\n../rtl/leaf.sv\n")
    (sim / "tb.sv").write_text("module tb;\n  leaf u();\nendmodule\n")
    (rtl / "leaf.sv").write_text("module leaf;\nendmodule\n")
    tb = sim / "tb.sv"

    failures = []
    client = session(binary, sim, [tb])
    try:
        # Until the filelist is indexed there is no declaration to find.
        def prepared():
            _, msg = client.request("textDocument/prepareRename", at(tb, 1, 2), timeout=10)
            error = (msg.get("error") or {}).get("message", "")
            return msg if msg.get("result") or (error and "cannot find" not in error) else None
        msg = poll(time.monotonic() + timeout, prepared)
        if not msg or (msg.get("result") or {}).get("placeholder") != "leaf":
            failures.append(f"leaf: prepareRename from sim/tb.sv refused: {msg}")
        try:
            _, msg = client.request("textDocument/rename", at(tb, 1, 2, newName="leaf2"),
                                    timeout=10)
        except TimeoutError:
            msg = {"error": "no reply"}
        changed = sorted(Path(u.replace("file:///", "/")).name
                         for u in ((msg.get("result") or {}).get("changes") or {}))
        if changed != ["leaf.sv", "tb.sv"]:
            failures.append(f"leaf: rename edited {changed}, want leaf.sv and tb.sv: {msg}")
    finally:
        close(client)
    return failures


def duplicate_module_refused(binary: str, ws: Path, timeout: float) -> list[str]:
    ws.mkdir(parents=True)
    (ws / "a.sv").write_text("module m;\nendmodule\n")
    (ws / "b.sv").write_text("module m;\nendmodule\nmodule t;\n  m u();\nendmodule\n")
    b = ws / "b.sv"

    failures = []
    client = session(binary, ws, [b])
    try:
        # Renamed outright until the implicit scan lands; refused after.
        state = {}
        def refused():
            try:
                _, msg = client.request("textDocument/rename", at(b, 3, 2, newName="zz"),
                                        timeout=10)
            except TimeoutError:
                state["timeout"] = True
                return True
            state["msg"] = msg
            return msg.get("error")
        poll(time.monotonic() + timeout, refused)
        msg = state.get("msg") or {}
        error = msg.get("error") or {}
        if state.get("timeout"):
            failures.append("duplicate: a refused rename got no reply")
        elif error.get("code") != -32803 or "declared 2 times" not in error.get("message", ""):
            failures.append(f"duplicate: rename not refused with RequestFailed: {msg}")
        if client.proc.poll() is not None:
            failures.append("duplicate: the server exited")
    finally:
        close(client)
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", required=True)
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="lazyverilog-rename-smoke-")).resolve()
    try:
        cache = tmp / "cache"
        cache.mkdir()
        # Keep implicit-project shards out of the real user cache.
        os.environ["LOCALAPPDATA"] = str(cache)
        os.environ["XDG_CACHE_HOME"] = str(cache)
        if sys.platform == "darwin":
            os.environ["HOME"] = str(cache)

        failures = leaf_outside_toml_dir(args.binary, tmp / "layout", args.timeout)
        failures += duplicate_module_refused(args.binary, tmp / "dup", args.timeout)
        for failure in failures:
            print("FAIL:", failure)
        if not failures:
            print("rename smoke: ok")
        return 1 if failures else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
