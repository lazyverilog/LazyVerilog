#!/usr/bin/env bash
# Format one snippet from stdin under an inline [format] body, twice.
# Prints pass-1 output (`|` marks line ends) and IDEMP-FAIL + diff if pass 2 differs.
# usage: snippet.sh '[format.statement]\nalign = true' <<'EOF' ... EOF
set -u
fmt=${FMT:-$(git rev-parse --show-toplevel)/build/lazyverilog-fmt}
d=$(mktemp -d)
printf '[format]\n%b\n' "${1:-}" > "$d/lazyverilog.toml"
cat > "$d/t.sv"
"$fmt" -i "$d/t.sv" || echo "RC=$?"
cp "$d/t.sv" "$d/t2.sv"; "$fmt" -i "$d/t2.sv"
sed 's/$/|/' "$d/t.sv"
cmp -s "$d/t.sv" "$d/t2.sv" || { echo "IDEMP-FAIL"; diff "$d/t.sv" "$d/t2.sv"; }
rm -rf "$d"
