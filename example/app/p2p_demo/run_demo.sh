#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 一键运行 p2p_demo：STUN echo 服务器 + 两个独立 peer 进程。
#
# 用法：
#   bash example/app/p2p_demo/run_demo.sh [可执行文件] [STUN 端口]
#   （默认从仓库根取 ./.build-bento-wsl/example/app/go2cpp_p2p_demo，端口 34780）
#
# 脚本会：
#   1. 起 STUN echo（127.0.0.1:PORT）；
#   2. 先后起 peer a（绑 127.0.0.1）与 peer b（绑 127.0.0.2），模拟两个 NAT；
#   3. 等待双方完成打洞与 3 轮请求/响应，打印两份日志与 STUN 统计；
#   4. 清理临时目录与子进程。
# ---------------------------------------------------------------------------
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../../.." && pwd)
BIN=${1:-$ROOT/.build-bento-wsl/example/app/go2cpp_p2p_demo}
PORT=${2:-34780}
WORK=$(mktemp -d /tmp/go2cpp_p2p_demo.XXXXXX)
STUN_PID=""
A_PID=""
B_PID=""

cleanup() {
    [ -n "$A_PID" ] && kill "$A_PID" 2>/dev/null
    [ -n "$B_PID" ] && kill "$B_PID" 2>/dev/null
    if [ -n "$STUN_PID" ]; then
        kill -TERM "$STUN_PID" 2>/dev/null
        wait "$STUN_PID" 2>/dev/null
    fi
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT

if [ ! -x "$BIN" ]; then
    echo "找不到可执行文件：$BIN"
    echo "先构建：cmake --build .build-bento-wsl --target go2cpp_p2p_demo -j8"
    exit 1
fi

"$BIN" stun --port "$PORT" > "$WORK/stun.log" 2>&1 &
STUN_PID=$!
for _ in $(seq 1 40); do
    grep -q '已监听' "$WORK/stun.log" 2>/dev/null && break
    sleep 0.05
done
if ! grep -q '已监听' "$WORK/stun.log" 2>/dev/null; then
    echo "STUN 启动失败（端口 $PORT 可能被占用）："
    cat "$WORK/stun.log"
    exit 1
fi

"$BIN" peer --role a --stun "127.0.0.1:$PORT" --signal-dir "$WORK/sig" \
    > "$WORK/a.log" 2>&1 &
A_PID=$!
sleep 0.2
"$BIN" peer --role b --stun "127.0.0.1:$PORT" --signal-dir "$WORK/sig" \
    > "$WORK/b.log" 2>&1 &
B_PID=$!

wait "$A_PID"
A_RC=$?
wait "$B_PID"
B_RC=$?

kill -TERM "$STUN_PID" 2>/dev/null
wait "$STUN_PID" 2>/dev/null
STUN_PID=""

echo "===== peer a（退出码 $A_RC）====="
cat "$WORK/a.log"
echo "===== peer b（退出码 $B_RC）====="
cat "$WORK/b.log"
echo "===== stun ====="
cat "$WORK/stun.log"

if [ "$A_RC" -ne 0 ] || [ "$B_RC" -ne 0 ]; then
    echo "P2P 演示失败"
    exit 1
fi
echo "P2P 演示成功：两个独立进程完成打洞与 3 轮收发"
