#!/usr/bin/env bash
#
# test_netem.sh — 在 loopback 上用 tc netem（20% 丢包 + 200ms 延迟）跑 Bento 弱网探针。
#
# 为什么不是直接 `tc qdisc add dev lo root netem ...`：
#   本机的 DSH Web GUI 走 127.0.0.1:56911。对整个 lo 加丢包/延迟会波及它。
#   这里用 prio + u32 过滤器，只把源/目的端口 == 探针端口 的流量放进
#   netem class，其他 loopback 流量保持原样。
#
# 无权限 / 内核无 netem / lo 已有自定义 qdisc 时输出 SKIP 并 exit 77
# （CTest 用 SKIP_RETURN_CODE=77 识别）。
#
# 用法：bash test_netem.sh /path/to/bento_netem_probe
set -u

PROBE="${1:-}"
PORT="${BENTO_NETEM_PORT:-34567}"
if [[ -z "$PROBE" || ! -x "$PROBE" ]]; then
    echo "SKIP: probe binary missing or not executable: ${PROBE:-<none>}"
    exit 77
fi
if ! command -v tc >/dev/null 2>&1; then
    echo "SKIP: tc not found"
    exit 77
fi
if ! command -v ip >/dev/null 2>&1; then
    echo "SKIP: ip not found"
    exit 77
fi

TC=(tc)
if [[ "$(id -u)" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
        TC=(sudo -n tc)
    else
        echo "SKIP: no root/CAP_NET_ADMIN (uid=$(id -u), passwordless sudo unavailable)"
        exit 77
    fi
fi

ROOT_QDISC="$("${TC[@]}" qdisc show dev lo 2>/dev/null | head -n 1)"
if [[ "$ROOT_QDISC" != *"noqueue"* && "$ROOT_QDISC" != *"pfifo_fast"* && \
      "$ROOT_QDISC" != *"fq_codel"* && "$ROOT_QDISC" != *"prio"* ]]; then
    echo "SKIP: lo already has a custom root qdisc: $ROOT_QDISC"
    exit 77
fi

ADDED=0
cleanup() {
    # 保存脚本退出码：EXIT trap 的最后一个命令会把状态覆盖成 0，
    # 那样 SKIP(77) 会被 CTest 误判为 PASS。
    local rc=$?
    if [[ "$ADDED" -eq 1 ]]; then
        "${TC[@]}" qdisc del dev lo root >/dev/null 2>&1 || true
    fi
    exit "$rc"
}
trap cleanup EXIT

if ! "${TC[@]}" qdisc add dev lo root handle 1: prio bands 3 2>/dev/null; then
    echo "SKIP: cannot add prio qdisc on lo (kernel or permissions)"
    exit 77
fi
ADDED=1
if ! "${TC[@]}" qdisc add dev lo parent 1:3 handle 30: netem loss 20% delay 200ms 2>/dev/null; then
    echo "SKIP: netem unavailable on this kernel"
    exit 77
fi
if ! "${TC[@]}" filter add dev lo protocol ip parent 1:0 prio 3 \
        u32 match ip dport "$PORT" 0xffff flowid 1:3 2>/dev/null; then
    echo "SKIP: u32 filter unavailable"
    exit 77
fi
"${TC[@]}" filter add dev lo protocol ip parent 1:0 prio 3 \
    u32 match ip sport "$PORT" 0xffff flowid 1:3 2>/dev/null || true

echo "netem: loss=20% delay=200ms port=$PORT root_qdisc=$ROOT_QDISC"
"$PROBE" --port "$PORT" --messages 200 --payload 256 --timeout-ms 40000
RC=$?
if [[ "$RC" -eq 0 ]]; then
    echo "netem: PASS (reliable 100% ordered, unreliable 0 reorder)"
else
    echo "netem: FAIL (rc=$RC)"
fi
exit "$RC"
