#!/usr/bin/env bash
# Collect everything needed to explain lazyverilog's cold project-index startup
# on a shared/batch node, where the dev workstation cannot reproduce the timing.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build --target index-bench -j4
#   tools/hpc_probe.sh <project-root-containing-lazyverilog.toml> [outfile]
#
# Writes a single text file.  Nothing here needs root and nothing writes into
# the project.
set -u

PROJ=${1:?usage: hpc_probe.sh <project-root> [outfile]}
OUT=${2:-lazyverilog-hpc-probe.txt}
BIN=${BIN:-$(dirname "$0")/../build/index-bench}

[ -f "$PROJ/lazyverilog.toml" ] || { echo "no lazyverilog.toml in $PROJ"; exit 1; }
[ -x "$BIN" ] || { echo "build index-bench first: cmake --build build --target index-bench"; exit 1; }

exec > >(tee "$OUT") 2>&1

section() { printf '\n===== %s =====\n' "$1"; }

section "1. node"
date; hostname; uname -a
echo "nproc: $(nproc)"
lscpu 2>/dev/null | grep -E 'Model name|^CPU\(s\)|Thread\(s\)|Core\(s\)|Socket\(s\)|NUMA node\(s\)|L3'
echo "load: $(uptime)"

section "2. cpu budget inputs (these size the worker pool)"
grep -H Cpus_allowed_list /proc/self/status
echo "-- cgroup membership"; cat /proc/self/cgroup
echo "-- cgroup v2 cpu.max (walk to root)"
cg=$(awk -F: '$1==0{print $3}' /proc/self/cgroup)
d=/sys/fs/cgroup${cg}
while :; do [ -r "$d/cpu.max" ] && echo "$d/cpu.max: $(cat "$d/cpu.max")"
  [ "$d" = /sys/fs/cgroup ] && break; d=$(dirname "$d"); done
echo "-- cgroup v1 cpu quota (walk to root)"
cg1=$(awk -F: '$2 ~ /(^|,)cpu(,|$)/{print $3}' /proc/self/cgroup | head -1)
d=/sys/fs/cgroup/cpu${cg1:-}
while :; do [ -r "$d/cpu.cfs_quota_us" ] && \
    echo "$d: quota=$(cat "$d/cpu.cfs_quota_us") period=$(cat "$d/cpu.cfs_period_us" 2>/dev/null)"
  [ "$d" = /sys/fs/cgroup/cpu ] && break; p=$(dirname "$d"); [ "$p" = "$d" ] && break; d=$p; done
echo "-- scheduler environment"
env | grep -E '^(SLURM|LSB_|NCPUS|PBS_|OMP_NUM_THREADS)' | sort || echo "(none set)"

section "3. memory limits"
free -g 2>/dev/null | head -3
cat /sys/fs/cgroup/memory.max 2>/dev/null || cat /sys/fs/cgroup/memory/memory.limit_in_bytes 2>/dev/null
ulimit -a | grep -Ei 'memory|virtual|address'

section "4. project shape"
FL=$(grep -E '^\s*vcode' "$PROJ/lazyverilog.toml" | head -1 | sed 's/.*=\s*"\(.*\)".*/\1/')
echo "filelist: $FL"
"$BIN" "$PROJ" 0 2>&1 | head -3
echo "-- header (.svh/.vh) sizes reachable from incdirs, biggest 15"
grep -h '^+incdir+' "$PROJ/$FL" 2>/dev/null | sed 's/^+incdir+//' | \
  while read -r d; do find "$d" -maxdepth 3 \( -name '*.svh' -o -name '*.vh' \) -printf '%s %p\n' 2>/dev/null; done | \
  sort -rn | head -15
echo "-- filesystem"
df -PT "$PROJ" 2>/dev/null | tail -1
stat -f -c 'fstype=%T' "$PROJ" 2>/dev/null

section "5. RUN A: full CPU budget, traced + sampled for pool size and peak RSS"
# Peak thread count is the direct read of pool size: peak - 1 = indexer workers.
# The sampling loop costs a little CPU, so take timing from RUN C, not from here.
LAZYVERILOG_TRACE_PERF=1 "$BIN" "$PROJ" 1 >runA.out 2>runA.trace &
pid=$!; peak=0; hwm=0
while kill -0 $pid 2>/dev/null; do
  n=$(ls /proc/$pid/task 2>/dev/null | wc -l); [ "$n" -gt "$peak" ] && peak=$n
  m=$(awk '/VmHWM/{print $2}' /proc/$pid/status 2>/dev/null); [ "${m:-0}" -gt "$hwm" ] && hwm=$m
  sleep 0.05 2>/dev/null || sleep 1
done
wait $pid
echo "peak threads: $peak   (indexer workers = peak - 1)"
echo "peak RSS:     $((hwm / 1024)) MB"
cat runA.out

section "6. RUN B: forced to one CPU (the scaling comparison that matters)"
LAZYVERILOG_TRACE_PERF=1 taskset -c 0 "$BIN" "$PROJ" 1 >runB.out 2>runB.trace
cat runB.out

section "7. RUN C: full budget, untraced and unsampled (the clean wall time)"
"$BIN" "$PROJ" 2

section "8. per-file cost"
# sum_in_function / wall_of_that_run = how many workers were effectively running.
for r in A B; do
  printf 'run %s: ' "$r"
  grep -o 'make_file_state_with_options .*: [0-9]*us' "run$r.trace" \
    | sed 's/.*: \([0-9]*\)us/\1/' | sort -n \
    | awk '{v[++c]=$1/1000; s+=$1/1000}
           END{ if(!c){print "no trace lines"; exit}
                printf "files=%d  sum_in_function=%.2f s  median=%.1f ms  p90=%.1f ms  max=%.1f ms\n",
                       c, s/1000, v[int(c/2)+1], v[int(c*0.9)+1], v[c] }'
done
echo
echo "slowest 10 files (run B, serial, so attribution is clean):"
grep -o 'make_file_state_with_options .*: [0-9]*us' runB.trace \
  | sed 's/.*\/\([^/]*\): \([0-9]*\)us/\2 \1/' | sort -rn | head -10

section "9. load during the runs"
uptime
echo "done -> $OUT (also keep runA.trace runB.trace)"
