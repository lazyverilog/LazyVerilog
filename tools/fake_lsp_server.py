#!/usr/bin/env python3
"""Test double for tools/test_lsp_fuzz.py: a tiny LSP server over stdio that
answers every request with `null`, plus switches that make it misbehave."""
from __future__ import annotations

import argparse
import json
import sys

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
    args = ap.parse_args()

    if args.stderr_bytes:
        sys.stderr.buffer.write(b"x" * args.stderr_bytes)
        sys.stderr.flush()

    stdin, stdout = sys.stdin.buffer, sys.stdout.buffer
    seen = 0
    while (msg := read_message(stdin)) is not None:
        seen += 1
        if args.exit_after and seen >= args.exit_after:
            return 3
        method = msg.get("method")
        if method == "exit":
            return 0
        if "id" not in msg or method == args.stall_method:
            continue
        if method == args.bad_utf8:
            body = b'{"jsonrpc":"2.0","id":%d,"result":"\xc3"}' % msg["id"]
            stdout.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
            stdout.flush()
            continue
        result = {"capabilities": CAPABILITIES} if method == "initialize" else None
        write_message(stdout, {"jsonrpc": "2.0", "id": msg["id"], "result": result})
        if method == "initialize" and args.stderr_text:
            sys.stderr.write(args.stderr_text + "\n")
            sys.stderr.flush()
        if method == "initialize":
            for i in range(args.flood_stdout):
                write_message(stdout, {"jsonrpc": "2.0", "id": f"s{i}",
                                       "method": "workspace/inlayHint/refresh"})
                write_message(stdout, {"jsonrpc": "2.0", "method": "window/logMessage",
                                       "params": {"type": 4, "message": "x" * 65536}})
    return 0


if __name__ == "__main__":
    sys.exit(main())
