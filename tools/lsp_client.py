"""Minimal LSP client shared by the bench and fuzz tools: framed JSON-RPC over
a server's stdio."""
from __future__ import annotations

import json
import queue
import subprocess
import threading
import time

SANITIZER_MARKERS = (b"ERROR: AddressSanitizer", b"ERROR: LeakSanitizer", b"runtime error:")
STDERR_TAIL_BYTES = 64 * 1024


def frame(obj: dict) -> bytes:
    body = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


class Client:
    """Talks to one server process.

    With `capture_stderr`, the server's stderr is drained on its own thread --
    a server blocked on a full stderr pipe would otherwise look like a hang --
    keeping the last `STDERR_TAIL_BYTES` and flagging sanitizer reports.

    Writes go through a queue to their own thread, so neither the caller nor the
    stdout reader ever blocks on a full stdin pipe: a reader stuck writing stops
    draining stdout, and a server blocked writing stdout may stop reading stdin.
    """

    def __init__(self, cmd: list[str], cwd: str, capture_stderr: bool = False):
        self.proc = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE if capture_stderr else None)
        self.next_id = 0
        self.outbox = queue.Queue()
        self.write_error = None
        self.responses = {}
        self.notifications = []          # (arrival_time, method, params)
        self.bad_messages = []           # bodies that are not UTF-8 JSON
        self.last_message = time.monotonic()
        self.eof = False
        self.stderr_tail = b""
        self.sanitizer_hit = False
        self.cv = threading.Condition()
        threading.Thread(target=self._read_loop, daemon=True).start()
        threading.Thread(target=self._write_loop, daemon=True).start()
        self.stderr_thread = None
        if capture_stderr:
            self.stderr_thread = threading.Thread(target=self._stderr_loop, daemon=True)
            self.stderr_thread.start()

    def _read_loop(self):
        out = self.proc.stdout
        while True:
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                ch = out.read(1)
                if not ch:
                    with self.cv:
                        self.eof = True
                        self.cv.notify_all()
                    return
                header += ch
            fields = dict(line.split(b": ", 1)
                          for line in header.strip().split(b"\r\n"))
            body = out.read(int(fields[b"Content-Length"]))
            arrived = time.monotonic()
            # perf_counter: monotonic() ticks every ~16 ms on Windows.
            arrived_precise = time.perf_counter()
            try:
                msg = json.loads(body)
            except ValueError:
                with self.cv:
                    self.bad_messages.append(body)
                    self.cv.notify_all()
                continue
            with self.cv:
                self.last_message = arrived
                if "id" in msg and "method" not in msg:
                    self.responses[msg["id"]] = (arrived_precise, msg)
                elif "method" in msg:
                    self.notifications.append((arrived, msg["method"], msg.get("params")))
                    if "id" in msg:      # server->client request, e.g. inlayHint/refresh
                        self.send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
                self.cv.notify_all()

    def _stderr_loop(self):
        tail = b""
        for line in self.proc.stderr:
            hit = any(marker in line for marker in SANITIZER_MARKERS)
            tail = (tail + line)[-STDERR_TAIL_BYTES:]
            with self.cv:
                self.stderr_tail = tail
                self.sanitizer_hit = self.sanitizer_hit or hit

    def _write_loop(self):
        while True:
            data = self.outbox.get()
            try:
                self.proc.stdin.write(data)
                self.proc.stdin.flush()
            except OSError as e:
                self.write_error = e
                return

    def send(self, obj: dict):
        self.outbox.put(frame(obj))

    def notify(self, method: str, params: dict):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method: str, params: dict, timeout: float = 60.0):
        self.next_id += 1
        rid = self.next_id
        start = time.perf_counter()
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
