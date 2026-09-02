#!/usr/bin/env python3
"""Measure what an editor actually waits on per keystroke.

`startup_bench.py` covers cold indexing; this covers the steady-state edit loop.
It speaks LSP to a real `lazyverilog-lsp` over stdio, opens one file, waits for
the project index, then replays keystrokes as `didChange` notifications.  After
each one it issues the requests Neovim sends with this plugin's default
`on_attach` -- `foldingRange` (from `vim.lsp.foldexpr`) and `inlayHint` -- and
times the round trip, plus how long the following `publishDiagnostics` takes.

Every number is server round-trip time as the client sees it, so a slow result
here means the server; a session that feels laggy while these stay small means
the editor side (redraw, fold recompute, diagnostic rendering) or the transport.

Examples
--------
    tools/edit_latency_bench.py <project-root> <file-in-it>
    tools/edit_latency_bench.py ~/work/chip rtl/core/alu.sv --cpus 0
    tools/edit_latency_bench.py ~/work/chip rtl/inc/defs.svh --gap 0.6
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = REPO_ROOT / "build" / "lazyverilog-lsp"


def frame(obj: dict) -> bytes:
    body = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


class Client:
    """Minimal LSP client: framed JSON-RPC over the server's stdio."""

    def __init__(self, cmd: list[str], cwd: str):
        self.proc = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)
        self.next_id = 0
        self.write_lock = threading.Lock()
        self.responses = {}
        self.notifications = []          # (arrival_time, method, params)
        self.last_message = time.monotonic()
        self.cv = threading.Condition()
        threading.Thread(target=self._read_loop, daemon=True).start()

    def _read_loop(self):
        out = self.proc.stdout
        while True:
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                ch = out.read(1)
                if not ch:
                    return
                header += ch
            fields = dict(line.split(b": ", 1)
                          for line in header.strip().split(b"\r\n"))
            body = out.read(int(fields[b"Content-Length"]))
            arrived = time.monotonic()
            try:
                msg = json.loads(body)
            except ValueError:
                continue
            with self.cv:
                self.last_message = arrived
                if "id" in msg and "method" not in msg:
                    self.responses[msg["id"]] = (arrived, msg)
                elif "method" in msg:
                    self.notifications.append((arrived, msg["method"], msg.get("params")))
                    if "id" in msg:      # server->client request, e.g. inlayHint/refresh
                        self.send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
                self.cv.notify_all()

    def send(self, obj: dict):
        with self.write_lock:
            self.proc.stdin.write(frame(obj))
            self.proc.stdin.flush()

    def notify(self, method: str, params: dict):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method: str, params: dict, timeout: float = 60.0):
        self.next_id += 1
        rid = self.next_id
        start = time.monotonic()
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        with self.cv:
            while rid not in self.responses:
                if not self.cv.wait(timeout):
                    raise TimeoutError(method)
            arrived, msg = self.responses.pop(rid)
        return (arrived - start) * 1000.0, msg

    def wait_quiet(self, quiet: float, cap: float = 600.0) -> float:
        """Wait until the server has said nothing for `quiet` seconds."""
        start = time.monotonic()
        while True:
            with self.cv:
                idle = time.monotonic() - self.last_message
                if idle >= quiet or time.monotonic() - start > cap:
                    return time.monotonic() - start
                self.cv.wait(quiet - idle)

    def wait_diagnostics(self, uri: str, after: float, timeout: float = 30.0) -> float:
        with self.cv:
            while True:
                for arrived, method, params in self.notifications:
                    if (arrived > after and method == "textDocument/publishDiagnostics"
                            and params and params.get("uri") == uri):
                        return (arrived - after) * 1000.0
                if not self.cv.wait(timeout):
                    return float("nan")


def summarize(name: str, samples: list[float]):
    samples = sorted(v for v in samples if v == v)   # drop NaN (timed out)
    if not samples:
        print(f"  {name:18s} n/a")
        return
    p90 = samples[int(0.9 * (len(samples) - 1))]
    print(f"  {name:18s} median {statistics.median(samples):7.2f}   "
          f"p90 {p90:7.2f}   max {samples[-1]:7.2f}   ms")


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter, description=__doc__)
    ap.add_argument("project", help="directory holding lazyverilog.toml")
    ap.add_argument("file", help="file to edit, inside the project")
    ap.add_argument("-n", "--keystrokes", type=int, default=20)
    ap.add_argument("--gap", type=float, default=0.05,
                    help="seconds between keystrokes (default 0.05, ~20 cps; use "
                         "0.6 to let the idle-triggered header fanout run each time)")
    ap.add_argument("--binary", default=str(DEFAULT_BINARY))
    ap.add_argument("--cpus", default=None,
                    help="restrict the server to this CPU list via taskset, e.g. "
                         "'0' for a one-core slice")
    ap.add_argument("--quiet", type=float, default=5.0,
                    help="seconds of server silence that count as 'index settled'")
    args = ap.parse_args()

    project = Path(args.project).resolve()
    path = Path(args.file).resolve()
    uri = "file://" + str(path)
    text = path.read_text()
    lines = text.split("\n")

    cmd = [str(Path(args.binary).resolve())]
    if args.cpus:
        cmd = ["taskset", "-c", args.cpus] + cmd

    client = Client(cmd, str(project))
    init_ms, _ = client.request("initialize", {
        "processId": os.getpid(),
        "rootUri": "file://" + str(project),
        "capabilities": {
            "textDocument": {
                "synchronization": {"didSave": True},
                "foldingRange": {"lineFoldingOnly": True},
                "inlayHint": {"dynamicRegistration": False},
                "publishDiagnostics": {},
            },
            "workspace": {"didChangeWatchedFiles": {"dynamicRegistration": True}},
        },
    })
    client.notify("initialized", {})
    client.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "systemverilog", "version": 1, "text": text}})
    settle_s = client.wait_quiet(args.quiet)

    # Proof the project index is actually live: an empty result here means the
    # edit numbers below were measured against a server that indexed nothing.
    sym_ms, sym = client.request("workspace/symbol", {"query": path.stem[:4] or "a"})
    sym_hits = len(sym.get("result") or [])

    # Type at the end of the file, where no construct is left half-finished.
    line = max(0, len(lines) - 2)
    col = len(lines[line])
    folding, hints, diagnostics = [], [], []
    for k in range(args.keystrokes):
        edit_start = time.monotonic()
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2 + k},
            "contentChanges": [{
                "range": {"start": {"line": line, "character": col + k},
                          "end": {"line": line, "character": col + k}},
                "rangeLength": 0, "text": " "}]})
        ms, _ = client.request("textDocument/foldingRange", {"textDocument": {"uri": uri}})
        folding.append(ms)
        ms, _ = client.request("textDocument/inlayHint", {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": min(len(lines) - 1, 200), "character": 0}}})
        hints.append(ms)
        diagnostics.append(client.wait_diagnostics(uri, edit_start))
        time.sleep(args.gap)

    print(f"{path.name}: {len(lines)} lines, {args.keystrokes} keystrokes, "
          f"gap {args.gap}s, cpus {args.cpus or 'all'}")
    print(f"  initialize {init_ms:.1f} ms; index settled after {settle_s:.1f} s; "
          f"workspace/symbol -> {sym_hits} hits in {sym_ms:.1f} ms")
    summarize("foldingRange", folding)
    summarize("inlayHint", hints)
    summarize("didChange->diag", diagnostics)

    client.notify("exit", {})
    try:
        client.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        client.proc.kill()


if __name__ == "__main__":
    main()
