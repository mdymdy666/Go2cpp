#!/usr/bin/env bash
# 线程版 Channel 混合挂起复现装置驱动。
#
# 做什么：
#   1. 用 build-work（OFF）编译 tests/bench/channel_thread_mixed_retry.cpp；
#   2. 跑 ROUNDS 轮「吞吐轮 + 延迟轮」，每轮 watchdog 监控 Gate 等待；
#   3. 输出每轮耗时；挂起时打印线程 stage / Channel 快照并退出 124。
#
# 用法：
#   bash tests/bench/run_channel_thread_mixed_retry.sh
#   bash tests/bench/run_channel_thread_mixed_retry.sh --legacy-gate-bug --rounds=1
#   ROUNDS=1000 N=2000 bash tests/bench/run_channel_thread_mixed_retry.sh
#   ROUNDS=100 N=100000 CAP=0 WORKERS=4 bash tests/bench/run_channel_thread_mixed_retry.sh
#   TASKSET="taskset -c 0" ROUNDS=200 bash tests/bench/run_channel_thread_mixed_retry.sh
#
# 环境变量：
#   ROUNDS=100 N=2000 LAT_N=500 CAP=64 WORKERS=2 WATCHDOG_SEC=10
#   EXTRA="--no-stage"        额外参数
#   TASKSET="taskset -c 0"    可选的 CPU 绑定
#   BUILD=build-work          链接的 OFF 构建
#   INCLUDE=include           头文件目录（复现旧库时可指向旧 worktree）
#   SKIP_BUILD=1              复用 /tmp/channel_mixed_retry
#
# 边界：只新增 tests/bench/**，不改 src/，不 commit；默认 OFF 构建（与当初一致）。

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-work}"
INCLUDE="${INCLUDE:-$ROOT/include}"
SRC="$ROOT/tests/bench/channel_thread_mixed_retry.cpp"
BIN="${BIN:-/tmp/channel_mixed_retry}"
ROUNDS="${ROUNDS:-100}"
N="${N:-2000}"
LAT_N="${LAT_N:-500}"
CAP="${CAP:-64}"
WORKERS="${WORKERS:-2}"
WATCHDOG_SEC="${WATCHDOG_SEC:-10}"
EXTRA="${EXTRA:-}"
TASKSET="${TASKSET:-}"
LOG="${LOG:-/tmp/channel_mixed_retry.log}"
SKIP_BUILD="${SKIP_BUILD:-0}"

if [ "$SKIP_BUILD" != "1" ]; then
    exec 9>/tmp/go2cpp-channel-retry-build.lock
    if ! flock -n 9; then
        echo "另一个编译正在进行（锁: /tmp/go2cpp-channel-retry-build.lock）" >&2
        exit 4
    fi
    LIBS="$(ls "$BUILD"/libgo2cpp_*.so 2>/dev/null |
        sed 's|.*/lib||;s|\.so$||;s|^|-l|' | tr '\n' ' ')"
    if [ -z "$LIBS" ]; then
        echo "缺少 $BUILD/libgo2cpp_*.so（先构建 OFF 库）" >&2
        exit 2
    fi
    echo "[build] $SRC <- $BUILD"
    # shellcheck disable=SC2086
    if ! g++ -std=c++17 -O3 -DNDEBUG -pthread -I "$INCLUDE" "$SRC" \
        -L "$BUILD" $LIBS -Wl,-rpath,"$BUILD" -o "$BIN"; then
        echo "编译失败" >&2
        exit 3
    fi
    flock -u 9
fi

if [ ! -x "$BIN" ]; then
    echo "缺少二进制: $BIN" >&2
    exit 3
fi

timeout $((ROUNDS * 15 + 120)) $TASKSET "$BIN" \
    --rounds="$ROUNDS" --n="$N" --lat-n="$LAT_N" --cap="$CAP" \
    --workers="$WORKERS" --watchdog-sec="$WATCHDOG_SEC" $EXTRA \
    >"$LOG" 2>&1
rc=$?
rounds_done=$(grep -c '^\[round' "$LOG" 2>/dev/null || true)
hang=$(grep -c '^\[hang\]' "$LOG" 2>/dev/null || true)
echo "RC=$rc rounds=$rounds_done hang=$hang log=$LOG"
if [ "$hang" -gt 0 ]; then
    sed -n '/^\[hang\]/,$p' "$LOG"
fi
exit "$rc"
