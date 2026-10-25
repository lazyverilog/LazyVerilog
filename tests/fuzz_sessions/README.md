# Fuzz regression sessions

Each session here is replayed against the server by the `lsp-fuzz-replay`
CTest test (`tools/lsp_fuzz.py --replay`), and must pass. When a fuzz failure
is fixed, its session (minimized by hand if it is large) lands here as
`<area>-<short-description>.jsonl`, in the same commit as the fix or the one
after it. The format is one JSON object per line: a header (`seed`, `root`, or
a free-form `note`), then `{"send": <message>}` lines. See
`docs/dev/fuzzing.md`.
