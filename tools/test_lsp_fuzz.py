#!/usr/bin/env python3
"""Tests for tools/lsp_fuzz.py.  Run: python3 -I tools/test_lsp_fuzz.py"""
from __future__ import annotations

import json
import random
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # -I drops the script dir

import lsp_fuzz  # noqa: E402

# --server is split POSIX-style, so the paths use forward slashes.
FAKE = f'"{Path(sys.executable).as_posix()}" -I "{(HERE / "fake_lsp_server.py").as_posix()}"'
CORPUS = HERE.parent / "tests" / "rtl" / "hpc60"

# Every provider the real server advertises (src/server.cpp).
SERVER_CAPABILITIES = {
    name: True for name in (
        "hoverProvider", "documentFormattingProvider", "documentRangeFormattingProvider",
        "definitionProvider", "referencesProvider", "documentHighlightProvider",
        "selectionRangeProvider", "workspaceSymbolProvider", "documentSymbolProvider",
        "signatureHelpProvider", "codeActionProvider", "inlayHintProvider",
        "foldingRangeProvider")
}
SERVER_CAPABILITIES["completionProvider"] = {"resolveProvider": False}
SERVER_CAPABILITIES["renameProvider"] = {"prepareProvider": True}
SERVER_CAPABILITIES["executeCommandProvider"] = {"commands": ["lazyverilog.lint"]}


def generate(seed: int, steps: int) -> list[dict]:
    gen = lsp_fuzz.Generator(random.Random(seed), lsp_fuzz.corpus_files([CORPUS]),
                             SERVER_CAPABILITIES)
    out = []
    for _ in range(steps):
        out.extend(gen.step())
    return out


def run_fuzz(*args: str, timeout: float = 60) -> tuple[int, float]:
    start = time.monotonic()
    proc = subprocess.run([sys.executable, "-I", str(HERE / "lsp_fuzz.py"), *args],
                          capture_output=True, timeout=timeout)
    return proc.returncode, time.monotonic() - start


class ApplyChange(unittest.TestCase):
    def test_apply_change_utf16(self):
        change = {"range": {"start": {"line": 0, "character": 3},
                            "end": {"line": 0, "character": 4}}, "text": "X"}
        self.assertEqual(lsp_fuzz.apply_change("a\U0001F600b\n", change), "a\U0001F600X\n")
        self.assertEqual(lsp_fuzz.apply_change("old", {"text": "new"}), "new")

    def test_apply_change_across_lines(self):
        change = {"range": {"start": {"line": 0, "character": 1},
                            "end": {"line": 1, "character": 1}}, "text": ""}
        self.assertEqual(lsp_fuzz.apply_change("ab\ncd\n", change), "ad\n")


class Generation(unittest.TestCase):
    def test_same_seed_same_session(self):
        self.assertEqual(generate(7, 200), generate(7, 200))

    def test_every_capability_has_generator(self):
        lsp_fuzz.check_coverage(SERVER_CAPABILITIES)
        with self.assertRaises(lsp_fuzz.MissingGenerator) as caught:
            lsp_fuzz.check_coverage({**SERVER_CAPABILITIES, "fooProvider": True})
        self.assertEqual(str(caught.exception), "fooProvider")

    def test_session_mix_includes_stale_cancels(self):
        messages = generate(3, 2000)
        issued = {m["id"] for m in messages if "id" in m}
        cancelled = [m["params"]["id"] for m in messages if m.get("method") == "$/cancelRequest"]
        self.assertTrue(any(i in issued for i in cancelled))
        self.assertTrue(any(i not in issued for i in cancelled))

    def test_session_mix_includes_reopen_checks(self):
        gen = lsp_fuzz.Generator(random.Random(3), lsp_fuzz.corpus_files([CORPUS]),
                                 SERVER_CAPABILITIES)
        compared = []
        for _ in range(2000):
            gen.step()
            compared.extend(gen.take_comparisons())
        self.assertEqual({c[2] for c in compared},
                         {"textDocument/foldingRange", "textDocument/formatting"})

    def test_session_mix_includes_closed_uri(self):
        open_uris: set[str] = set()
        hit = False
        for m in generate(5, 2000):
            method, params = m.get("method"), m.get("params") or {}
            uri = (params.get("textDocument") or {}).get("uri")
            if method == "textDocument/didOpen":
                open_uris.add(uri)
            elif method == "textDocument/didClose":
                open_uris.discard(uri)
            elif "id" in m and uri and uri not in open_uris:
                hit = True
        self.assertTrue(hit)

    def test_session_mix_includes_change_to_closed_uri(self):
        open_uris: set[str] = set()
        hit = False
        for m in generate(5, 4000):
            method, params = m.get("method"), m.get("params") or {}
            uri = (params.get("textDocument") or {}).get("uri")
            if method == "textDocument/didOpen":
                open_uris.add(uri)
            elif method == "textDocument/didClose":
                open_uris.discard(uri)
            elif method == "textDocument/didChange" and uri not in open_uris:
                hit = True
        self.assertTrue(hit)

    def test_reopened_uri_is_not_counted_closed(self):
        gen = lsp_fuzz.Generator(random.Random(5), lsp_fuzz.corpus_files([CORPUS]),
                                 SERVER_CAPABILITIES)
        for _ in range(3000):
            gen.step()
            self.assertFalse(set(gen.closed) & set(gen.docs))


class StubClient:
    """Just what Runner.poll() reads from a Client."""
    def __init__(self, responses: dict):
        self.cv = threading.Condition()
        self.responses = {rid: (0.0, msg) for rid, msg in responses.items()}
        self.sanitizer_hit, self.bad_messages, self.eof = False, [], False
        self.proc = type("Proc", (), {"poll": staticmethod(lambda: None)})()


class Comparison(unittest.TestCase):
    def poll(self, first: dict, second: dict):
        runner = lsp_fuzz.Runner(StubClient({1: first, 2: second}), [], reply_timeout=60)
        runner.compare(1, 2, "textDocument/foldingRange", "file:///a.sv")
        runner.pending = {1: (time.monotonic(), "m"), 2: (time.monotonic(), "m")}
        runner.poll()

    def test_different_answers_are_a_mismatch(self):
        with self.assertRaises(lsp_fuzz.Failure) as caught:
            self.poll({"id": 1, "result": [1]}, {"id": 2, "result": [2]})
        self.assertEqual(caught.exception.kind, "incremental-mismatch")

    def test_a_cancelled_answer_is_not_compared(self):
        cancelled = {"code": -32800, "message": "request cancelled"}
        self.poll({"id": 1, "result": [1]}, {"id": 2, "error": cancelled})
        self.poll({"id": 1, "error": cancelled}, {"id": 2, "result": [2]})


class Watchdog(unittest.TestCase):
    def runner(self, pending: dict, responses: dict | None = None):
        runner = lsp_fuzz.Runner(StubClient(responses or {}), [], reply_timeout=1,
                                 command_timeout=60)
        runner.pending = pending
        return runner

    def test_slow_command_is_not_a_hang(self):
        late = time.monotonic() - 5
        # A request queued behind the command waits for it too.
        self.runner({1: (late, "workspace/executeCommand"),
                     2: (late, "textDocument/hover")}).poll()

    def test_slow_request_is_a_hang_whatever_the_command_timeout(self):
        late = time.monotonic() - 5
        with self.assertRaises(lsp_fuzz.Failure) as caught:
            self.runner({2: (late, "textDocument/hover")}).poll()
        self.assertEqual(caught.exception.kind, "hang")

    def test_request_sent_before_a_command_keeps_its_timeout(self):
        late = time.monotonic() - 5
        with self.assertRaises(lsp_fuzz.Failure):
            self.runner({1: (late, "textDocument/hover"),
                         2: (late, "workspace/executeCommand")}).poll()

    def test_response_to_an_id_not_pending_is_a_protocol_failure(self):
        with self.assertRaises(lsp_fuzz.Failure) as caught:
            self.runner({}, {7: {"id": 7, "result": None}}).poll()
        self.assertEqual(caught.exception.kind, "protocol")


class Oracle(unittest.TestCase):
    def setUp(self):
        self.out = Path(tempfile.mkdtemp(prefix="lsp_fuzz_test_"))

    def fuzz(self, server_args: str, seconds: str = "5") -> tuple[int, float]:
        return run_fuzz("--server", f"{FAKE} {server_args}", "--root", str(CORPUS),
                        "--seed", "1", "--seconds", seconds, "--out", str(self.out),
                        "--reply-timeout", "2")

    def test_clean_server_passes(self):
        code, _ = self.fuzz("")
        self.assertEqual(code, 0)

    def test_crash_is_reported(self):
        code, _ = self.fuzz("--exit-after 5")
        self.assertEqual(code, 1)
        self.assertTrue(list(self.out.glob("fail-*.jsonl")))
        self.assertIn("crash", next(self.out.glob("fail-*.txt")).read_text())

    def test_hang_is_reported(self):
        code, elapsed = self.fuzz("--stall-method textDocument/hover", seconds="30")
        self.assertEqual(code, 1)
        self.assertLess(elapsed, 15)
        self.assertIn("textDocument/hover", next(self.out.glob("fail-*.txt")).read_text())

    def test_stderr_flood_is_not_a_hang(self):
        code, _ = self.fuzz(f"--stderr-bytes {10 * 1024 * 1024}", seconds="3")
        self.assertEqual(code, 0)

    def test_server_flooding_stdout_is_not_a_deadlock(self):
        code, _ = self.fuzz("--flood-stdout 200", seconds="3")
        self.assertEqual(code, 0)

    def test_incremental_mismatch_is_reported(self):
        code, _ = self.fuzz("--corrupt-incremental", seconds="10")
        self.assertEqual(code, 1)
        report = next(self.out.glob("fail-*.txt")).read_text()
        self.assertIn("incremental-mismatch", report)
        self.assertIn("file:///", report)
        session = next(self.out.glob("fail-*.jsonl")).read_text()
        code, _ = run_fuzz("--server", f"{FAKE} --corrupt-incremental", "--replay",
                           str(next(self.out.glob("fail-*.jsonl"))), "--reply-timeout", "2",
                           "--out", str(self.out / "replay"))
        self.assertIn('"compare"', session)
        self.assertEqual(code, 1)

    def test_incremental_match_passes(self):
        code, _ = self.fuzz("", seconds="10")
        self.assertEqual(code, 0)

    def test_answer_from_a_later_snapshot_is_not_a_mismatch(self):
        # The real server answers foldingRange off the dispatch thread, from
        # whatever snapshot is current when the work runs.
        code, _ = self.fuzz("--late-folding", seconds="10")
        self.assertEqual(code, 0)

    def test_undecodable_response_is_reported(self):
        code, elapsed = self.fuzz("--bad-utf8 textDocument/hover", seconds="30")
        self.assertEqual(code, 1)
        self.assertLess(elapsed, 15)
        self.assertIn("protocol", next(self.out.glob("fail-*.txt")).read_text())

    def test_sanitizer_report_is_reported(self):
        code, _ = self.fuzz("--stderr-text 'ERROR: AddressSanitizer: heap-use-after-free'")
        self.assertEqual(code, 1)
        self.assertIn("sanitizer", next(self.out.glob("fail-*.txt")).read_text())

    def test_sanitizer_report_keeps_the_stack(self):
        # ASan writes its stack after the first line; the report must wait for it.
        code, _ = self.fuzz("--stderr-text 'ERROR: AddressSanitizer: heap-buffer-overflow' "
                            "--stderr-stack")
        self.assertEqual(code, 1)
        self.assertIn("#0 0x1 in frame_zero", next(self.out.glob("fail-*.txt")).read_text())

    def test_replay_reproduces_crash(self):
        self.fuzz("--exit-after 5")
        session = next(self.out.glob("fail-*.jsonl"))
        header = json.loads(session.read_text().splitlines()[0])
        self.assertEqual(header["seed"], 1)
        code, _ = run_fuzz("--server", f"{FAKE} --exit-after 5", "--replay", str(session),
                           "--reply-timeout", "2", "--out", str(self.out / "replay"))
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
