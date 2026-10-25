#!/usr/bin/env python3
"""Seeded LSP session fuzzer for lazyverilog-lsp.

Drives the real server over stdio with a random mix of opens, incremental and
full edits, closes, cancels and every request type the server advertises, and
fails on a crash, a sanitizer report, or a request left unanswered for longer
than --reply-timeout seconds.  Every message sent is recorded; a failing run
writes the session to <out>/fail-<seed>.jsonl, which --replay sends again.

The request mix comes from the server's capabilities: an advertised provider
with no generator here fails the run, so a new feature cannot go unfuzzed.

Examples
--------
    tools/lsp_fuzz.py --server build/lazyverilog-lsp --root tests/rtl/hpc60 --seconds 60
    tools/lsp_fuzz.py --server build/lazyverilog-lsp --replay fuzz-out/fail-1.jsonl

`--server` is split like a POSIX shell command line, so write paths with
forward slashes.  See docs/dev/fuzzing.md.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shlex
import string
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
from lsp_client import Client  # noqa: E402

MAX_PENDING = 8
MAX_OPEN = 8
SANITIZER_REPORT_WAIT_S = 30
SOURCE_SUFFIXES = (".sv", ".svh", ".v")
VOCAB = ["begin", "end", "module", "endmodule", "`", "(", ")", ";", '"', "/*", "*/", "//",
         "é", "\U0001F600", "\n", " ", "`define ", "`include \"x.svh\"\n", "always_ff"]
NEW_NAMES = ["x", "new_name", "a b", "", "\U0001F600", "module"]
STEP_WEIGHTS = {"open": 5, "change": 30, "full_change": 3, "close": 3, "request": 50,
                "cancel": 9, "reopen_check": 2}
# Answered from the server's copy of the text alone, so a document edited
# incrementally must answer exactly as a fresh open of the same text does.
REOPEN_CHECKED = (("foldingRangeProvider", "textDocument/foldingRange"),
                  ("documentFormattingProvider", "textDocument/formatting"))
UNANSWERED = (-32800, -32801)  # RequestCancelled, ContentModified: nothing to compare


class MissingGenerator(Exception):
    pass


class Failure(Exception):
    def __init__(self, kind: str, detail: str):
        super().__init__(f"{kind}: {detail}")
        self.kind, self.detail = kind, detail


# -- text model (LSP positions are UTF-16 code units) --------------------------

def utf16_len(s: str) -> int:
    return sum(2 if ord(c) > 0xFFFF else 1 for c in s)


def to_index(text: str, line: int, character: int) -> int:
    pos = 0
    for _ in range(line):
        nl = text.find("\n", pos)
        if nl < 0:
            return len(text)
        pos = nl + 1
    units = 0
    while pos < len(text) and text[pos] != "\n" and units < character:
        units += 2 if ord(text[pos]) > 0xFFFF else 1
        pos += 1
    return pos


def apply_change(text: str, change: dict) -> str:
    if "range" not in change:
        return change["text"]
    start, end = change["range"]["start"], change["range"]["end"]
    a = to_index(text, start["line"], start["character"])
    b = to_index(text, end["line"], end["character"])
    return text[:a] + change["text"] + text[b:]


def position_at(text: str, index: int) -> dict:
    line = text.count("\n", 0, index)
    line_start = text.rfind("\n", 0, index) + 1
    return {"line": line, "character": utf16_len(text[line_start:index])}


def corpus_files(dirs: list[Path]) -> list[Path]:
    files = {p.resolve() for d in dirs for p in Path(d).rglob("*")
             if p.suffix in SOURCE_SUFFIXES and p.is_file()}
    return sorted(files)


# -- request generators, keyed by the capability that advertises them ---------

def _doc(uri):
    return {"textDocument": {"uri": uri}}


def _at(gen, uri):
    return {**_doc(uri), "position": gen.position(uri)}


def _range(gen, uri):
    return {**_doc(uri), "range": gen.range(uri)}


FORMAT_OPTIONS = {"tabSize": 4, "insertSpaces": True}

GENERATORS = {
    "hoverProvider": lambda g, u: ("textDocument/hover", _at(g, u)),
    "definitionProvider": lambda g, u: ("textDocument/definition", _at(g, u)),
    "referencesProvider": lambda g, u: (
        "textDocument/references",
        {**_at(g, u), "context": {"includeDeclaration": g.rng.random() < 0.5}}),
    "documentHighlightProvider": lambda g, u: ("textDocument/documentHighlight", _at(g, u)),
    "signatureHelpProvider": lambda g, u: ("textDocument/signatureHelp", _at(g, u)),
    "completionProvider": lambda g, u: ("textDocument/completion", _at(g, u)),
    "renameProvider": lambda g, u: g.rng.choice([
        ("textDocument/prepareRename", _at(g, u)),
        ("textDocument/rename", {**_at(g, u), "newName": g.rng.choice(NEW_NAMES)})]),
    "documentFormattingProvider": lambda g, u: (
        "textDocument/formatting", {**_doc(u), "options": FORMAT_OPTIONS}),
    "documentRangeFormattingProvider": lambda g, u: (
        "textDocument/rangeFormatting", {**_range(g, u), "options": FORMAT_OPTIONS}),
    "workspaceSymbolProvider": lambda g, u: (
        "workspace/symbol", {"query": g.rng.choice(["", "a", "clk", "hpc", "\U0001F600"])}),
    "documentSymbolProvider": lambda g, u: ("textDocument/documentSymbol", _doc(u)),
    "codeActionProvider": lambda g, u: (
        "textDocument/codeAction", {**_range(g, u), "context": {"diagnostics": []}}),
    "inlayHintProvider": lambda g, u: ("textDocument/inlayHint", _range(g, u)),
    "foldingRangeProvider": lambda g, u: ("textDocument/foldingRange", _doc(u)),
    "selectionRangeProvider": lambda g, u: (
        "textDocument/selectionRange",
        {**_doc(u), "positions": [g.position(u) for _ in range(g.rng.randint(1, 3))]}),
    "executeCommandProvider": lambda g, u: ("workspace/executeCommand", g.command(u)),
}


def check_coverage(capabilities: dict):
    for name, value in capabilities.items():
        if name.endswith("Provider") and value and name not in GENERATORS:
            raise MissingGenerator(name)


class Generator:
    """Produces the messages of a session.  Deterministic for a given seed,
    corpus and capability set: it never looks at the server's answers."""

    def __init__(self, rng: random.Random, files: list[Path], capabilities: dict):
        self.rng, self.files = rng, files
        self.methods = [name for name in GENERATORS if capabilities.get(name)]
        commands = capabilities.get("executeCommandProvider")
        self.commands = commands.get("commands", []) if isinstance(commands, dict) else []
        self.docs: dict[str, str] = {}
        self.versions: dict[str, int] = {}
        self.closed: list[str] = []
        self.issued: list[int] = []
        self.next_id = 1
        self._disk: dict[str, str] = {}
        self.reopen_methods = [m for cap, m in REOPEN_CHECKED if capabilities.get(cap)]
        self.incremental: set[str] = set()   # open docs with an incremental edit since open
        self.comparisons: list[tuple[int, int, str, str]] = []

    # -- helpers used by GENERATORS --
    def text(self, uri: str) -> str:
        if uri in self.docs:
            return self.docs[uri]
        if uri not in self._disk:
            path = next(p for p in self.files if p.as_uri() == uri)
            self._disk[uri] = path.read_text(encoding="utf-8", errors="replace")
        return self._disk[uri]

    def position(self, uri: str, in_bounds: bool = False) -> dict:
        text = self.text(uri)
        if not in_bounds and self.rng.random() < 0.1:
            return {"line": text.count("\n") + self.rng.randrange(1, 5),
                    "character": self.rng.randrange(80)}
        return position_at(text, self.rng.randrange(len(text) + 1))

    def range(self, uri: str) -> dict:
        text = self.text(uri)
        a, b = sorted(self.rng.randrange(len(text) + 1) for _ in range(2))
        return {"start": position_at(text, a), "end": position_at(text, b)}

    def command(self, uri: str) -> dict:
        command = self.rng.choice(self.commands) if self.commands else "lazyverilog.lint"
        pos = self.position(uri)
        arguments = self.rng.choice([[], [uri], [uri, pos["line"], pos["character"]],
                                     [{"uri": uri, **pos}]])
        return {"command": command, "arguments": arguments}

    # -- messages --
    def request(self, method: str, params) -> dict:
        rid = self.next_id
        self.next_id += 1
        self.issued.append(rid)
        return {"jsonrpc": "2.0", "id": rid, "method": method, "params": params}

    @staticmethod
    def notification(method: str, params) -> dict:
        return {"jsonrpc": "2.0", "method": method, "params": params}

    def step(self) -> list[dict]:
        kinds = list(STEP_WEIGHTS)
        kind = self.rng.choices(kinds, weights=[STEP_WEIGHTS[k] for k in kinds])[0]
        if not self.docs or (kind == "open" and len(self.docs) < MAX_OPEN):
            return self._open()
        if kind == "open":
            kind = "close"
        return getattr(self, "_" + kind)()

    def _open(self) -> list[dict]:
        closed = [p for p in self.files if p.as_uri() not in self.docs]
        if not closed:
            return self._close()
        uri = self.rng.choice(closed).as_uri()
        text = self.text(uri)
        self.docs[uri] = text
        self.closed = [u for u in self.closed if u != uri]
        self.versions[uri] = 1
        return [self.notification("textDocument/didOpen", {"textDocument": {
            "uri": uri, "languageId": "systemverilog", "version": 1, "text": text}})]

    def _close(self) -> list[dict]:
        uri = self.rng.choice(sorted(self.docs))
        del self.docs[uri]
        self.incremental.discard(uri)
        self.closed.append(uri)
        return [self.notification("textDocument/didClose", _doc(uri))]

    def _mutation(self, text: str) -> tuple[int, int, str]:
        """(start, end, replacement) as indices into `text`."""
        a = self.rng.randrange(len(text) + 1)
        roll = self.rng.random()
        if roll < 0.3:
            b = min(len(text), a + self.rng.randrange(1, 40))
            return a, b, ""
        if roll < 0.6:
            return a, a, self.rng.choice(VOCAB)
        if roll < 0.8:
            start = text.rfind("\n", 0, a) + 1
            end = text.find("\n", a)
            line = text[start:] if end < 0 else text[start:end + 1]
            return start, start, line
        noise = string.printable + "é\U0001F600"
        return a, a, "".join(self.rng.choice(noise) for _ in range(self.rng.randint(1, 8)))

    def _edit(self, full: bool) -> list[dict]:
        if self.closed and self.rng.random() < 1 / 50:
            # A change to a document that is not open: the server must ignore it.
            return [self.notification("textDocument/didChange", {
                "textDocument": {"uri": self.rng.choice(self.closed), "version": 1},
                "contentChanges": [{"text": self.rng.choice(VOCAB)}]})]
        uri = self.rng.choice(sorted(self.docs))
        text = self.docs[uri]
        a, b, new = self._mutation(text)
        if full:
            change = {"text": text[:a] + new + text[b:]}
        else:
            change = {"range": {"start": position_at(text, a), "end": position_at(text, b)},
                      "text": new}
        self.docs[uri] = apply_change(text, change)
        self.versions[uri] += 1
        if not full:
            self.incremental.add(uri)
        return [self.notification("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": self.versions[uri]},
            "contentChanges": [change]})]

    def _change(self) -> list[dict]:
        return self._edit(full=False)

    def _full_change(self) -> list[dict]:
        return self._edit(full=True)

    def _request(self) -> list[dict]:
        if self.rng.random() < 1 / 50:
            unopened = self.closed or [p.as_uri() for p in self.files if p.as_uri() not in self.docs]
            uri = self.rng.choice(unopened) if unopened else self.rng.choice(sorted(self.docs))
        else:
            uri = self.rng.choice(sorted(self.docs))
        method, params = GENERATORS[self.rng.choice(self.methods)](self, uri)
        return [self.request(method, params)]

    def _reopen_check(self) -> list[dict]:
        """Ask, reopen with the model's text, ask again: the answers must match."""
        candidates = sorted(self.incremental)
        if not candidates or not self.reopen_methods:
            return self._change()
        uri = self.rng.choice(candidates)
        self.incremental.discard(uri)
        params = {"textDocument/foldingRange": _doc(uri),
                  "textDocument/formatting": {**_doc(uri), "options": FORMAT_OPTIONS}}
        before = [self.request(m, params[m]) for m in self.reopen_methods]
        self.versions[uri] += 1
        reopen = [self.notification("textDocument/didClose", _doc(uri)),
                  self.notification("textDocument/didOpen", {"textDocument": {
                      "uri": uri, "languageId": "systemverilog", "version": self.versions[uri],
                      "text": self.docs[uri]}})]
        after = [self.request(m, params[m]) for m in self.reopen_methods]
        self.comparisons += [(b["id"], a["id"], b["method"], uri) for b, a in zip(before, after)]
        return before + reopen + after

    def take_comparisons(self) -> list[tuple[int, int, str, str]]:
        """(before id, after id, method, uri) pairs from the last steps, once."""
        taken, self.comparisons = self.comparisons, []
        return taken

    def _cancel(self) -> list[dict]:
        if self.issued and self.rng.random() < 0.9:
            rid = self.rng.choice(self.issued)
        else:
            rid = -self.rng.randrange(1, 1000)  # never issued
        return [self.notification("$/cancelRequest", {"id": rid})]


# -- running a session against the server -------------------------------------

class Runner:
    def __init__(self, client: Client, record: list[dict], reply_timeout: float,
                 command_timeout: float | None = None):
        self.client, self.record, self.reply_timeout = client, record, reply_timeout
        # workspace/executeCommand (lintAll compiles the project) and everything
        # queued behind it get this instead of reply_timeout.
        self.command_timeout = command_timeout or reply_timeout
        self.pending: dict = {}           # id -> (sent_at, method)
        self.answers: dict = {}           # id -> response, for ids in `keep`
        self.keep: set = set()
        self.comparisons: list = []       # (before id, after id, method, uri)
        self.exiting = False

    def compare(self, before: int, after: int, method: str, uri: str):
        """Require the answers to `before` and `after` to be identical."""
        self.record.append({"compare": [before, after, method, uri]})
        self.keep.update((before, after))
        self.comparisons.append((before, after, method, uri))

    def send(self, msg: dict):
        # A compared answer must describe the text at its own place in the
        # stream.  The server may answer from a later snapshot while more
        # messages are queued (foldingRange runs off the dispatch thread), so
        # nothing goes out until every compared request has been answered.
        self.wait(lambda: not self.keep.intersection(self.pending))
        self.record.append({"send": msg})
        if "id" in msg and "method" in msg:
            self.pending[msg["id"]] = (time.monotonic(), msg["method"])
        self.client.send(msg)  # queued; a dead server shows up as EOF in poll()

    def poll(self):
        c = self.client
        with c.cv:
            for rid in [rid for rid in self.pending if rid in c.responses]:
                _, msg = c.responses.pop(rid)
                del self.pending[rid]
                if rid in self.keep:
                    self.answers[rid] = msg
            if c.responses:
                # A duplicate answer, or one to a cancelled-and-answered or
                # never-issued id: JSON-RPC allows exactly one per request.
                raise Failure("protocol", f"response to an id that is not pending: "
                                          f"{sorted(map(str, c.responses))[:5]}")
            if c.sanitizer_hit:
                raise Failure("sanitizer", "sanitizer report on stderr")
            if c.bad_messages:
                raise Failure("protocol", f"server sent a message that is not UTF-8 JSON: "
                                          f"{c.bad_messages[0][:300]!r}")
            if not self.exiting and (c.eof or c.proc.poll() is not None):
                raise Failure("crash", f"server exited (code {c.proc.poll()})")
        for before, after, method, uri in [c for c in self.comparisons
                                           if c[0] in self.answers and c[1] in self.answers]:
            self.comparisons.remove((before, after, method, uri))
            self.keep.difference_update((before, after))
            first, second = self.answers.pop(before), self.answers.pop(after)
            if any((a.get("error") or {}).get("code") in UNANSWERED for a in (first, second)):
                continue  # a cancel in the mix got there first
            if first.get("result") != second.get("result") or first.get("error") != second.get("error"):
                raise Failure("incremental-mismatch",
                              f"{method} on {uri}: answer after incremental edits (id {before}) "
                              f"differs from a fresh open of the same text (id {after})\n"
                              f"  incremental: {json.dumps(first)[:2000]}\n"
                              f"  fresh open:  {json.dumps(second)[:2000]}")
        now = time.monotonic()
        behind_command = False  # pending is in send order
        for rid, (sent, method) in self.pending.items():
            behind_command |= method == "workspace/executeCommand"
            limit = self.command_timeout if behind_command else self.reply_timeout
            if now - sent > limit:
                raise Failure("hang", f"{method} id={rid} unanswered after {limit:g} s")

    def wait(self, until):
        while not until():
            with self.client.cv:
                self.client.cv.wait(0.05)
            self.poll()

    def wait_capacity(self):
        self.wait(lambda: len(self.pending) < MAX_PENDING)

    def drain(self):
        self.wait(lambda: not self.pending)

    def initialize(self, root: Path) -> dict:
        msg = {"jsonrpc": "2.0", "id": 0, "method": "initialize",
               "params": {"processId": os.getpid(), "rootUri": root.as_uri(), "capabilities": {}}}
        self.keep.add(0)
        self.send(msg)
        self.drain()
        self.send(Generator.notification("initialized", {}))
        return (self.answers.get(0, {}).get("result") or {}).get("capabilities") or {}

    def shutdown(self, rid: int):
        self.drain()
        self.send({"jsonrpc": "2.0", "id": rid, "method": "shutdown", "params": None})
        self.drain()
        self.exiting = True
        self.send(Generator.notification("exit", None))
        proc = self.client.proc
        try:
            code = proc.wait(self.reply_timeout)
        except Exception:
            proc.kill()
            raise Failure("hang", f"server still running {self.reply_timeout:g} s after exit")
        if self.client.stderr_thread:
            self.client.stderr_thread.join(5)
        self.poll()
        if code != 0:
            raise Failure("crash", f"server exit code {code} after exit")


def write_failure(out: Path, header: dict, record: list[dict], failure: Failure, client: Client):
    out.mkdir(parents=True, exist_ok=True)
    name = f"fail-{header.get('seed', 'replay')}"
    with open(out / f"{name}.jsonl", "w", encoding="utf-8") as f:
        f.write(json.dumps(header) + "\n")
        for line in record:
            f.write(json.dumps(line) + "\n")
    if failure.kind == "sanitizer":
        # The marker is the report's first line; ASan writes the stack after it, then aborts.
        try:
            client.proc.wait(SANITIZER_REPORT_WAIT_S)
        except subprocess.TimeoutExpired:
            pass
        if client.stderr_thread:
            client.stderr_thread.join(5)
    tail = client.stderr_tail.decode("utf-8", errors="replace")
    (out / f"{name}.txt").write_text(
        f"{failure.kind}: {failure.detail}\nmessages sent: {len(record)}\n"
        f"--- server stderr (tail) ---\n{tail}", encoding="utf-8")
    print(f"FAIL {failure.kind}: {failure.detail}\n  session: {out / (name + '.jsonl')}",
          file=sys.stderr)


def start(server: str, root: Path) -> Client:
    argv = shlex.split(server)
    # The server runs in `root`, so a relative binary path is resolved here first.
    if Path(argv[0]).exists():
        argv[0] = str(Path(argv[0]).resolve())
    return Client(argv, str(root), capture_stderr=True)


def fuzz(args) -> int:
    root = Path(args.root).resolve()
    srcs = [Path(s).resolve() for s in (args.src or [root])]
    header = {"seed": args.seed, "root": str(root), "src": [str(s) for s in srcs]}
    client = start(args.server, root)
    record: list[dict] = []
    runner = Runner(client, record, args.reply_timeout, args.command_timeout)
    try:
        caps = runner.initialize(root)
        check_coverage(caps)
        gen = Generator(random.Random(args.seed), corpus_files(srcs), caps)
        if not gen.files:
            raise Failure("setup", f"no {'/'.join(SOURCE_SUFFIXES)} files under {srcs}")
        deadline = time.monotonic() + args.seconds
        steps = 0
        while time.monotonic() < deadline:
            messages = gen.step()
            for comparison in gen.take_comparisons():
                runner.compare(*comparison)
            for msg in messages:
                runner.wait_capacity()
                runner.send(msg)
            steps += 1
        runner.shutdown(gen.next_id)
    except (Failure, MissingGenerator) as e:
        failure = e if isinstance(e, Failure) else Failure("coverage", f"no generator for {e}")
        write_failure(Path(args.out), header, record, failure, client)
        client.proc.kill()
        return 1
    print(f"ok: seed {args.seed}, {steps} steps, {len(record)} messages")
    return 0


def replay(args) -> int:
    worst = 0
    for session in args.replay:
        lines = Path(session).read_text(encoding="utf-8").splitlines()
        header = json.loads(lines[0])
        entries = [json.loads(line) for line in lines[1:] if line.strip()]
        entries = [e for e in entries
                   if "compare" in e or e["send"].get("method") not in ("shutdown", "exit")]
        messages = [e["send"] for e in entries if "send" in e]
        root = Path(header.get("root", ".")) if Path(header.get("root", ".")).is_dir() else Path.cwd()
        client = start(args.server, root)
        record: list[dict] = []
        runner = Runner(client, record, args.reply_timeout, args.command_timeout)
        try:
            for entry in entries:
                if "compare" in entry:
                    runner.compare(*entry["compare"])
                    continue
                msg = entry["send"]
                runner.wait_capacity()
                runner.send(msg)
                if msg.get("method") == "initialize":
                    runner.drain()
            ids = [m["id"] for m in messages if isinstance(m.get("id"), int)]
            runner.shutdown(max(ids, default=0) + 1)
            print(f"ok: {session}")
        except Failure as e:
            write_failure(Path(args.out), {**header, "replay_of": str(session)}, record, e, client)
            client.proc.kill()
            worst = 1
    return worst


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--server", required=True, help="server command line")
    ap.add_argument("--root", help="workspace root (rootUri and server cwd)")
    ap.add_argument("--src", action="append", help="source directory to draw files from "
                                                   "(repeatable; default: --root)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--out", default="fuzz-out", help="where failing sessions are written")
    ap.add_argument("--reply-timeout", type=float, default=10.0,
                    help="seconds a request may stay unanswered (default 10)")
    ap.add_argument("--command-timeout", type=float,
                    help="the same for workspace/executeCommand and requests queued "
                         "behind it (default: --reply-timeout)")
    ap.add_argument("--replay", nargs="+", metavar="SESSION", help="replay recorded sessions")
    args = ap.parse_args()
    if args.replay:
        return replay(args)
    if not args.root:
        ap.error("--root is required unless --replay is given")
    return fuzz(args)


if __name__ == "__main__":
    sys.exit(main())
