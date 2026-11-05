#!/bin/bash
# 5 轮串行跑「无性能报告模块」微基准（taskset -c 1），结果落 results/。
# 用法：bash tests/bench/perf_uncovered/run_5rounds.sh
# 前置：按 README.md 先编译到 /tmp/perf_uncovered/bench_<module>。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT=/tmp/perf_uncovered
RES=$OUT/results
rm -rf "$RES"; mkdir -p "$RES" || exit 1

MODULES="json error resilience trace metrics config log arena orm relay io"

echo "loadavg: $(cat /proc/loadavg)"
for round in 1 2 3 4 5; do
  echo "===== round $round ====="
  for m in $MODULES; do
    if [ ! -x "$OUT/bench_$m" ]; then
      echo "skip $m (not built)"
      continue
    fi
    timeout 120 taskset -c 1 "$OUT/bench_$m" >> "$RES/$m.txt" 2>&1
    echo "  $m rc=$? $(tail -1 "$RES/$m.txt")"
  done
  timeout 120 taskset -c 1 "$OUT/bench_hook" >> "$RES/hook_baseline.txt" 2>&1
  echo "  hook_baseline rc=$? $(tail -1 "$RES/hook_baseline.txt")"
  LD_PRELOAD=$ROOT/.build-bento-wsl/libgo2cpp_hook.so \
    timeout 120 taskset -c 1 "$OUT/bench_hook" >> "$RES/hook_preload.txt" 2>&1
  echo "  hook_preload rc=$? $(tail -1 "$RES/hook_preload.txt")"
done
echo "=== done: $RES ==="
