#!/usr/bin/env python3
"""Test double for tools/test_lsp_fuzz.py: a tiny LSP server over stdio that
answers every request with `null`, plus switches that make it misbehave."""
from __future__ import annotations

import argparse
import json
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir

from lsp_fuzz import apply_change  # noqa: E402

CAPABILITIES = {
    "textDocumentSync": {"openClose": True, "change": 2},
    "hoverProvider": True,
    "foldingRangeProvider": True,
    "documentFormattingProvider": True,
}


def read_message(stream) -> dict | None:
    length = None
    while True:
        line = stream.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        name, _, value = line.decode().partition(":")
        if name.lower() == "content-length":
            length = int(value)
    return json.loads(stream.read(length))


def write_message(stream, obj: dict):
    body = json.dumps(obj).encode()
    stream.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    stream.flush()


def track_text(texts: dict[str, str], msg: dict, corrupt: bool):
    """Keep each open document's text, the way a real server's sync would."""
    method, params = msg.get("method"), msg.get("params") or {}
    uri = (params.get("textDocument") or {}).get("uri") if isinstance(params, dict) else None
    if method == "textDocument/didOpen":
        texts[uri] = params["textDocument"]["text"]
    elif method == "textDocument/didClose":
        texts.pop(uri, None)
    elif method == "textDocument/didChange" and uri in texts:
        for change in params["contentChanges"]:
            if corrupt and "range" in change:
                change = json.loads(json.dumps(change))
                for end in ("start", "end"):
                    change["range"][end]["character"] += 1
            texts[uri] = apply_change(texts[uri], change)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exit-after", type=int, help="exit abruptly after N messages")
    ap.add_argument("--stall-method", help="never answer this request method")
    ap.add_argument("--stderr-bytes", type=int, default=0, help="write N bytes to stderr first")
    ap.add_argument("--stderr-text", help="write this line to stderr after initialize")
    ap.add_argument("--bad-utf8", help="answer this request method with invalid UTF-8")
    ap.add_argument("--flood-stdout", type=int, default=0,
                    help="after initialize, send N server requests and 64 KiB log "
                         "messages before reading stdin again")
    ap.add_argument("--corrupt-incremental", action="store_true",
                    help="apply incremental changes one character to the right")
    ap.add_argument("--late-folding", action="store_true",
                    help="answer foldingRange 0.2 s late, from the text held by then")
    args = ap.parse_args()

    if args.stderr_bytes:
        sys.stderr.buffer.write(b"x" * args.stderr_bytes)
        sys.stderr.flush()

    stdin, stdout = sys.stdin.buffer, sys.stdout.buffer
    texts: dict[str, str] = {}
    seen = 0
    lock = threading.Lock()

    def reply(msg: dict, result):
        with lock:
            write_message(stdout, {"jsonrpc": "2.0", "id": msg["id"], "result": result})

    def answer_folding(msg: dict):
        text = texts.get(msg["params"]["textDocument"]["uri"])
        reply(msg, [{"startLine": 0, "endLine": text.count("\n")}] if text is not None else None)

    while (msg := read_message(stdin)) is not None:
        seen += 1
        if args.exit_after and seen >= args.exit_after:
            return 3
        method = msg.get("method")
        if method == "exit":
            return 0
        track_text(texts, msg, args.corrupt_incremental)
        if "id" not in msg or method == args.stall_method:
            continue
        if method == args.bad_utf8:
            body = b'{"jsonrpc":"2.0","id":%d,"result":"\xc3"}' % msg["id"]
            stdout.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
            stdout.flush()
            continue
        if method == "textDocument/foldingRange":
            if args.late_folding:
                threading.Timer(0.2, answer_folding, (msg,)).start()
            else:
                answer_folding(msg)
            continue
        result = {"capabilities": CAPABILITIES} if method == "initialize" else None
        text = texts.get(((msg.get("params") or {}).get("textDocument") or {}).get("uri"))
        if text is not None and method == "textDocument/formatting":
            result = [{"range": {"start": {"line": 0, "character": 0},
                                 "end": {"line": text.count("\n") + 1, "character": 0}},
                       "newText": text}]
        reply(msg, result)
        if method == "initialize" and args.stderr_text:
            sys.stderr.write(args.stderr_text + "\n")
            sys.stderr.flush()
        if method == "initialize":
            with lock:
                for i in range(args.flood_stdout):
                    write_message(stdout, {"jsonrpc": "2.0", "id": f"s{i}",
                                           "method": "workspace/inlayHint/refresh"})
                    write_message(stdout, {"jsonrpc": "2.0", "method": "window/logMessage",
                                           "params": {"type": 4, "message": "x" * 65536}})
    return 0


if __name__ == "__main__":
    sys.exit(main())
