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
    tools/edit_latency_bench.py ~/work/tb tb/my_test.svh --complete-after "phase.raise" --word get_n

With --complete-after, keystrokes type --word (again and again) on a new line
after the first line containing that text, and each one also sends the
`textDocument/completion` nvim-cmp sends on every identifier keystroke; its
latency and response size are reported.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
from lsp_client import Client  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = REPO_ROOT / "build" / "lazyverilog-lsp"


def summarize(name: str, samples: list[float], unit: str = "ms"):
    samples = sorted(v for v in samples if v == v)   # drop NaN (timed out)
    if not samples:
        print(f"  {name:18s} n/a")
        return
    p90 = samples[int(0.9 * (len(samples) - 1))]
    p95 = samples[int(0.95 * (len(samples) - 1))]
    print(f"  {name:18s} median {statistics.median(samples):7.2f}   "
          f"p90 {p90:7.2f}   p95 {p95:7.2f}   max {samples[-1]:7.2f}   {unit}")


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
    ap.add_argument("--complete-after", default=None,
                    help="type on a new line after the first line containing this "
                         "text, and time a completion per keystroke")
    ap.add_argument("--word", default="get_n",
                    help="identifier typed with --complete-after (default get_n)")
    args = ap.parse_args()

    project = Path(args.project).resolve()
    path = Path(args.file).resolve()
    uri = path.as_uri()
    text = path.read_text()
    lines = text.split("\n")

    cmd = [str(Path(args.binary).resolve())]
    if args.cpus and os.name == "nt":
        # `start /affinity` takes a hex mask; /b keeps the stdio pipes.
        mask = sum(1 << int(c) for c in args.cpus.split(","))
        cmd = ["cmd", "/c", "start", "", "/b", "/wait", "/affinity", f"{mask:x}"] + cmd
    elif args.cpus:
        cmd = ["taskset", "-c", args.cpus] + cmd

    client = Client(cmd, str(project))
    init_ms, _ = client.request("initialize", {
        "processId": os.getpid(),
        "rootUri": project.as_uri(),
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
    version = 1
    if args.complete_after:
        anchor = next(i for i, l in enumerate(lines) if args.complete_after in l)
        version += 1
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{
                "range": {"start": {"line": anchor, "character": len(lines[anchor])},
                          "end": {"line": anchor, "character": len(lines[anchor])}},
                "text": "\n    "}]})
        client.wait_quiet(1.0)
        line, col = anchor + 1, 4
    folding, hints, diagnostics, completion, sizes = [], [], [], [], []
    typed = 0
    for k in range(args.keystrokes):
        edit_start = time.monotonic()
        version += 1
        if args.complete_after and typed == len(args.word):
            # Erase the word and start over: one change, then the next keystroke.
            client.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{
                    "range": {"start": {"line": line, "character": col},
                              "end": {"line": line, "character": col + typed}},
                    "text": ""}]})
            version += 1
            typed = 0
        ch = args.word[typed] if args.complete_after else " "
        at = col + (typed if args.complete_after else k)
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{
                "range": {"start": {"line": line, "character": at},
                          "end": {"line": line, "character": at}},
                "rangeLength": 0, "text": ch}]})
        typed += 1
        if args.complete_after:
            ms, reply = client.request("textDocument/completion", {
                "textDocument": {"uri": uri},
                "position": {"line": line, "character": col + typed},
                "context": {"triggerKind": 1}})
            completion.append(ms)
            sizes.append(len(json.dumps(reply.get("result"))) / 1024.0)
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
    if completion:
        summarize("completion", completion)
        summarize("completion size", sizes, "KiB")

    client.notify("exit", {})
    try:
        client.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        client.proc.kill()


if __name__ == "__main__":
    main()
