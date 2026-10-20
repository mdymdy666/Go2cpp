#!/usr/bin/env bash
# SUBSCRIBE 实时推送 demo（服务端 + Python 客户端）。
#
# 用法:
#   tests/debug/run_subscribe_demo.sh [build_dir] [seconds]
#
# 默认使用 build-debug（GO2CPP_DEBUG_DUMP=ON）。脚本会在临时 socket 上启动
# go2cpp_debug_subscribe_demo，等待 socket 就绪后用 subscribe_client.py 订阅
# `events` 并打印收到的 PUSH 帧；退出时清理进程与 socket。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-"$ROOT/build-debug"}
SECONDS_TO_RUN=${2:-30}
BIN="$BUILD/go2cpp_debug_subscribe_demo"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN" >&2
    echo "请先用 -DGO2CPP_DEBUG_DUMP=ON 构建该 target（ctest 不注册它）" >&2
    exit 1
fi

SOCK=/tmp/go2cpp-subscribe-demo-$$.sock
LOG=/tmp/go2cpp-subscribe-demo-$$.log

"$BIN" "$SOCK" "$SECONDS_TO_RUN" >"$LOG" 2>&1 &
SERVER_PID=$!

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f "$SOCK" "$LOG"
}
trap cleanup EXIT

for _ in $(seq 1 200); do
    if [ -S "$SOCK" ]; then
        break
    fi
    sleep 0.05
done
if [ ! -S "$SOCK" ]; then
    echo "socket not ready: $SOCK" >&2
    cat "$LOG" >&2 || true
    exit 1
fi

echo "--- server ---"
head -2 "$LOG"
echo "--- client ---"
python3 "$ROOT/tests/debug/subscribe_client.py" "$SOCK" \
    --frames 3 --interval 10 --max-events 64
