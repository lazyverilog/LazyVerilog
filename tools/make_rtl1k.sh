#!/usr/bin/env bash
# Build the rtl1k corpus (cva6 + ibex, ~950 .sv files, no verification code) as one
# lazyverilog project in <out>.  Needs the tests/rtl/cva6 and tests/rtl/ibex submodules.
# Same recipe as "Reproducing" in docs/dev/review-2026-09.md.
#
#   tools/make_rtl1k.sh <repo> <out>
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <repo> <out>" >&2; exit 2; }
repo=$(cd "$1" && pwd)
out=$2
mkdir -p "$out"
cd "$out"
dirs=("$repo/tests/rtl/cva6" "$repo/tests/rtl/ibex")
find "${dirs[@]}" -name '*.sv' | grep -v /verif/ | sort > lazyverilog.f
# incdirs come from the .f, not the toml
find "${dirs[@]}" -name '*.svh' -exec dirname {} \; | sort -u | sed 's/^/+incdir+/' >> lazyverilog.f
printf '[design]\nvcode = "lazyverilog.f"\n[compilation]\nbackground_compilation = false\n' > lazyverilog.toml
echo "$(grep -vc '^+incdir+' lazyverilog.f) sources in $out/lazyverilog.f"
