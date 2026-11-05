#!/usr/bin/env bash
#
# run_bench_scale_5rounds.sh —— 三个基准的规模整改验收（每基准 5 轮）。
#
# 覆盖：
#   1. go2cpp_bufio_test 场景 5（2 GiB/轮，内部 6 轮、首轮丢弃 → 5 轮）；
#   2. bench_tunnel（单连接 512 MiB/轮、并发 512 MiB/轮，rounds=5）；
#   3. go2cpp_tests 内 Parser chunked 基准（900 MiB/轮，进程级 5 轮）。
#
# 前置：每轮测前都过 scripts/wait_low_load.sh 门禁（load1 < 1.0 且目标核
# idle > 90%），之后所有测量都绑同一个核（默认 6）。chunked 单独每轮过门禁，
# 因为它对共享内存带宽最敏感。
#
# 用法：scripts/run_bench_scale_5rounds.sh
#       OUT=/tmp/x.txt BUILD=.build-bench CORE=6 scripts/run_bench_scale_5rounds.sh
#
# 输出：原始轮值追加写入 $OUT（默认 /tmp/bench-scale-5rounds.txt），便于
# 直接抄进 docs/reports/bench-scale-fix-2026-11-05.md；CV 由报告脚本另算。
#
# @author go2cpp contributors
# @date 2026-11-05

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="${CORE:-6}"
BUILD="${BUILD:-$ROOT/.build-bench}"
OUT="${OUT:-/tmp/bench-scale-5rounds.txt}"

cd "$ROOT" || exit 1
if [ ! -x "$BUILD/go2cpp_bufio_test" ] || [ ! -x "$BUILD/bench_tunnel" ] \
    || [ ! -x "$BUILD/go2cpp_tests" ]; then
    echo "[bench-scale] 缺少构建产物：先 ninja -C $BUILD go2cpp_bufio_test bench_tunnel go2cpp_tests" >&2
    exit 2
fi

"$ROOT/scripts/wait_low_load.sh" "$CORE" || exit 1
: > "$OUT"

echo "[bench-scale] bufio 场景5（2 GiB/轮，6 轮取后 5 轮）" | tee -a "$OUT"
taskset -c "$CORE" "$BUILD/go2cpp_bufio_test" 2>&1 |
    grep -A 10 '场景5' | tee -a "$OUT"

echo "[bench-scale] bench_tunnel rounds=5 concurrency=4" | tee -a "$OUT"
taskset -c "$CORE" "$BUILD/bench_tunnel" 5 4 2>&1 |
    grep -E '_tput|_conc|summary' | tee -a "$OUT"

echo "[bench-scale] parser chunked（900 MiB/轮，进程级 5 轮，每轮门禁）" | tee -a "$OUT"
for round in 1 2 3 4 5; do
    "$ROOT/scripts/wait_low_load.sh" "$CORE" >/dev/null 2>&1 || {
        echo "[bench-scale] run $round 门禁失败，终止" >&2
        exit 1
    }
    GO2CPP_TEST_FILTER=network taskset -c "$CORE" "$BUILD/go2cpp_tests" 2>&1 |
        grep 'chunked ' | sed "s/^/[run $round] /" | tee -a "$OUT"
done

echo "[bench-scale] done -> $OUT"
