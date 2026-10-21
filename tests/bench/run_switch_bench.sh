#!/usr/bin/env bash
# Fiber vs 线程切换开销基准。
#
# 方法：
#   fiber  —— 两个协程通过无缓冲 channel 来回交接（每次 = park + wake + 调度器切换）；
#   thread —— 两个 pthread 通过 mutex + condition_variable 来回交接（futex wait/wake）。
#
# 用法：
#   bash tests/bench/run_switch_bench.sh
#   NS="10000 100000" bash tests/bench/run_switch_bench.sh
#   BUILD_DIR=build-work bash tests/bench/run_switch_bench.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-work}"
SRC="$ROOT/tests/bench/fiber_vs_thread_switch.cpp"
OUT="${OUT:-/tmp/go2cpp-switch-bench}"
NS_LIST="${NS:-10000 100000 1000000}"

LIBS=$(ls "$BUILD_DIR"/libgo2cpp_*.so 2>/dev/null |
    sed 's|.*/lib||;s|\.so$||' | sed 's/^/-l/' | tr '\n' ' ')
if [ -z "$LIBS" ]; then
    echo "未找到 $BUILD_DIR/libgo2cpp_*.so（先构建）" >&2
    exit 2
fi

g++ -std=c++17 -O2 -I "$ROOT/include" "$SRC" -L "$BUILD_DIR" $LIBS \
    -Wl,-rpath,"$BUILD_DIR" -o "$OUT" || exit 3
echo "BUILD OK: $OUT"

for n in $NS_LIST; do
    timeout 300 "$OUT" "$n" || echo "n=$n 超时/失败" >&2
done
