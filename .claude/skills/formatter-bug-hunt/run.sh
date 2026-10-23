#!/usr/bin/env bash
# Format every *.sv in <rtl-dir> under every config in <cfg-dir>/*.toml.
# Reports: safety failures (exit != 0), non-idempotent outputs, and writes
# <out-dir>/<cfg>/<file> (pass 1) and <file>.2 (pass 2) for diffing.
# usage: run.sh <rtl-dir> <cfg-dir> <out-dir> [fmt-binary]
set -u
rtl=$1 cfgs=$2 out=$3
fmt=${4:-$(git rev-parse --show-toplevel)/build/lazyverilog-fmt}
for cfg in "$cfgs"/*.toml; do
    name=$(basename "$cfg" .toml)
    work="$out/$name"
    rm -rf "$work"; mkdir -p "$work"
    cp "$cfg" "$work/lazyverilog.toml"   # fmt reads the nearest toml above the file
    for src in "$rtl"/*.sv; do
        f="$work/$(basename "$src")"
        cp "$src" "$f.in"
        cp "$src" "$f"
        "$fmt" -i "$f" 2>"$f.err"; rc=$?
        [ $rc -ne 0 ] && { echo "SAFETY rc=$rc $name/$(basename "$src"): $(head -1 "$f.err")"; continue; }
        cp "$f" "$f.2"
        "$fmt" -i "$f.2" 2>>"$f.err"
        cmp -s "$f" "$f.2" || echo "IDEMPOTENCY $name/$(basename "$src")"
    done
done
echo "done: $out"
