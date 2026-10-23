# LSP session fuzzing

`tools/lsp_fuzz.py` drives the real server over stdio with a random but
seeded session: opens, incremental and full edits, closes, every request the
server advertises (at valid, past-EOF and closed-document positions), and
cancels for both live and never-issued ids. It needs Python 3 and nothing else.

## What counts as a failure

| Kind | Meaning |
|---|---|
| `crash` | the server exited or closed stdout before `exit`, or exited non-zero after it |
| `hang` | a request stayed unanswered for `--reply-timeout` seconds (default 10) |
| `protocol` | the server sent a message that is not UTF-8 JSON |
| `sanitizer` | an ASan/LSan/UBSan report appeared on stderr |
| `coverage` | the server advertises a provider the fuzzer has no generator for |

Stderr is drained on its own thread, and writes to the server go through a
queue, so a chatty server or one that stops reading while it writes is not
mistaken for a hang. A response the client cannot decode used to look exactly
like a hang; it is now reported as `protocol` with the start of the body.

## Running it

```bash
python3 -I tools/lsp_fuzz.py --server build/lazyverilog-lsp \
    --root tests/rtl/hpc60 --seed 1 --seconds 60 --out build/fuzz
```

`--server` is a command line split POSIX-style: quote a path that has spaces
and use forward slashes on Windows. `--src DIR` (repeatable) draws files from
somewhere other than `--root`. The same seed produces the same session.

CTest runs three of these when a Python 3 interpreter is found:

- `lsp-fuzz-unit` -- the fuzzer's own tests (`tools/test_lsp_fuzz.py`), against
  `tools/fake_lsp_server.py`, a stand-in server with switches that make it
  crash, stall, flood stdout or stderr, or send invalid UTF-8.
- `lsp-fuzz-smoke` -- 60 s with seed 1 on `tests/rtl/hpc60`.
- `lsp-fuzz-replay` -- every session in `tests/fuzz_sessions/`.

## Reading a report

A failure writes two files to `--out`:

- `fail-<seed>.jsonl` -- the session: a header line, then one `{"send": ...}`
  line per message, in order. Replies to server-to-client requests are not
  recorded; the client sends them itself.
- `fail-<seed>.txt` -- the kind and detail on the first line, the number of
  messages sent, then the last 64 KiB of the server's stderr.

## Replaying and minimizing

```bash
python3 -I tools/lsp_fuzz.py --server build/lazyverilog-lsp \
    --replay build/fuzz/fail-1.jsonl --out build/fuzz/replay
```

Replay drops the recorded `shutdown`/`exit`, keeps the fuzzer's pacing (at most
8 requests in flight), and ends with its own shutdown. Pacing keeps it
deterministic for most findings, not all. A request that depends on a finished
parse (`documentSymbol` returns nothing while `DocumentState::tree` is null)
can pass on a short session that the long one failed. Put a request that waits
for the parse, such as `textDocument/formatting`, before it.

To minimize, cut the session after the failing request, then keep only the
messages on that request's document. If the cut-down session no longer fails,
rebuild the document text at the failing request (apply the recorded changes
to the `didOpen` text with `lsp_fuzz.apply_change`) and write a short session
by hand. A unit test in the feature's own test file pins the root cause.

## Adding a regression session

Save the minimized session as `tests/fuzz_sessions/<area>-<short-description>.jsonl`
with a `note` in the header saying what it caught, check that it fails before
the fix and passes after it, and commit it with the fix. Re-run `cmake` so the
replay test picks the new file up.
