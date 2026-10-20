#!/usr/bin/env bash
# 一键跑 replay 增强的全部验收场景（不含需要真实并发的 stuck 注入）。
#
#   ./tools/replay/scripts/run_all.sh [build_dir]
set -euo pipefail

build_dir="${1:-/tmp/go2cpp-replay-build}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== [1/6] fanin 96 rendezvous（10 轮）"
"${here}/stress_record_replay.sh" "${build_dir}" 10

echo "=== [2/6] select 多 channel 竞争（5 轮）"
"${here}/select_record_replay.sh" "${build_dir}" 5

echo "=== [3/6] context/超时虚拟时钟（3 轮）"
"${here}/context_timeout_replay.sh" "${build_dir}" 3

echo "=== [4/6] string/结构体值（3 轮）"
"${here}/values_record_replay.sh" "${build_dir}" 3

echo "=== [5/6] 检查点分片与 until"
"${here}/chunked_replay.sh" "${build_dir}"

echo "=== [6/6] 丢唤醒注入（stuck 复现）"
"${here}/fault_injection.sh" "${build_dir}"

echo "=== all replay enhancement scenarios passed"
