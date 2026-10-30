#!/usr/bin/env bash
# 文件: tests/concurrency/run_future_opt_bench.sh
# 用途: Future 优化前后基准复现（基线 = git HEAD 的 future.hpp）。
#
# 用法:
#   tests/concurrency/run_future_opt_bench.sh [轮数]
#
# 说明:
#   - 基线头文件从 git HEAD 导出到临时目录，用同一份 bench 源码编译；
#   - 基线与优化版交替运行，避免系统负载漂移只影响其中一方；
#   - 每项基准内部已做 5 轮取中位数（见 bench_future_opt.cpp）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/.build-bento-wsl}"
CXX="${CXX:-g++}"
ROUNDS="${1:-3}"
OUT="${OUT_DIR:-/tmp/future_opt_bench}"
mkdir -p "$OUT"

BASE_INC="$OUT/baseline_include"
rm -rf "$BASE_INC"
mkdir -p "$BASE_INC/go2cpp/concurrency"
git -C "$ROOT" show HEAD:include/go2cpp/concurrency/future.hpp \
    > "$BASE_INC/go2cpp/concurrency/future.hpp"

COMMON=(-std=c++17 -O3 -DNDEBUG -pthread)
LIBS=(-L"$BUILD" -Wl,-rpath,"$BUILD"
      -lgo2cpp_future -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_sync
      -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_control_flow -lgo2cpp_log
      -lgo2cpp_debug_event -lgo2cpp_debug)

echo "[build] baseline"
"$CXX" "${COMMON[@]}" -I"$BASE_INC" -I"$ROOT/include" \
    "$ROOT/tests/concurrency/bench_future_opt.cpp" \
    -o "$OUT/bench_future_base" "${LIBS[@]}"
echo "[build] optimized"
"$CXX" "${COMMON[@]}" -I"$ROOT/include" \
    "$ROOT/tests/concurrency/bench_future_opt.cpp" \
    -o "$OUT/bench_future_opt" "${LIBS[@]}"

if command -v taskset >/dev/null 2>&1; then
    RUN=(taskset -c 0-3)
else
    RUN=()
fi

for ((round = 1; round <= ROUNDS; ++round)); do
    echo "===== round $round baseline ====="
    timeout 600 "${RUN[@]}" "$OUT/bench_future_base"
    echo "===== round $round optimized ====="
    timeout 600 "${RUN[@]}" "$OUT/bench_future_opt"
done
