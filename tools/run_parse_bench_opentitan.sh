#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

corpus_root="${OPENTITAN_ROOT:-/tmp/lazyverilog-bench/opentitan}"
filelist="${OPENTITAN_FILELIST:-/tmp/lazyverilog-bench/opentitan_hw.f}"
verible_root="${VERIBLE_ROOT:-$HOME/dev/verible}"
cpu_core="${CPU_CORE:-2}"
cpu_quota="${CPU_QUOTA:-}"
tool_bin="/tmp/lazyverilog-bench/bin"

mkdir -p "$(dirname "$corpus_root")" "$(dirname "$filelist")" "$tool_bin"

if [[ ! -x "$verible_root/bazel-bin/verible/verilog/tools/syntax/verible-verilog-syntax" ]] &&
    ! command -v bazel >/dev/null 2>&1; then
    if command -v bazelisk >/dev/null 2>&1; then
        ln -sf "$(command -v bazelisk)" "$tool_bin/bazel"
    elif command -v go >/dev/null 2>&1; then
        GOBIN="$tool_bin" go install github.com/bazelbuild/bazelisk@latest
        ln -sf "$tool_bin/bazelisk" "$tool_bin/bazel"
    fi
fi

export PATH="$tool_bin:$PATH"

if [[ ! -d "$corpus_root/.git" ]]; then
    git clone --depth 1 https://github.com/lowRISC/opentitan.git "$corpus_root"
elif [[ "${UPDATE_OPENTITAN:-0}" == "1" ]]; then
    git -C "$corpus_root" pull --ff-only
fi

find "$corpus_root/hw" -type f \
    \( -name '*.sv' -o -name '*.v' -o -name '*.svh' -o -name '*.vh' \) \
    | sort > "$filelist"

cmake --build "$repo_root/build" --target parse-bench

bench_cmd=(
    taskset -c "$cpu_core"
    nice -n 19
    "$repo_root/build/parse-bench"
    "$filelist"
    --ignore-missing
    --verible-root "$verible_root"
    --build-verible
    "$@"
)

echo "corpus:   $corpus_root"
echo "filelist: $filelist"
echo "files:    $(wc -l < "$filelist")"
echo "cpu:      taskset core $cpu_core, nice 19"
if [[ -n "$cpu_quota" ]]; then
    echo "quota:    systemd CPUQuota=$cpu_quota"
    exec systemd-run --user --scope -p "CPUQuota=$cpu_quota" "${bench_cmd[@]}"
fi

exec "${bench_cmd[@]}"
