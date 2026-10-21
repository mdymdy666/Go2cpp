#!/usr/bin/env bash
# Debug 能力增强测试矩阵（真实 DebugServer + GO2CPP-DEBUG/1 协议）。
#
# 覆盖：7 组件聚合/清单、事件环、SUBSCRIBE 推送、一致性标记、
#       正常/边界/错误/并发/资源限制；随机帧与 1000 次 dump 见同目录脚本。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=debug
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work debug)"
DEBUG_PID=""
trap 'stop_pid "${DEBUG_PID:-}"; rm -rf "$WORK"' EXIT

if [ ! -x "$DEBUG_DEMO" ]; then
    fail "fixture:debug-demo" normal "缺少 $DEBUG_DEMO（需 -DGO2CPP_DEBUG_DUMP=ON）"
    summary
    exit 1
fi
if ! start_debug_server 180; then
    fail "fixture:debug-server" normal "DebugServer 未就绪：$(tail -3 "$DEBUG_LOG" 2>/dev/null | tr '\n' '|')"
    summary
    exit 1
fi

CLI_DEBUG=(env GO2CPP_DEBUG_SOCKET="$DEBUG_SOCK" "$CLI")
PROBE=(python3 "$HERE/debug_probe.py" "$DEBUG_SOCK")

# ---------------- 正常路径 ----------------
expect_ok "status:CLI 读取协议/组件/订阅者" normal "协议: GO2CPP-DEBUG/1" \
    "${CLI_DEBUG[@]}" status --pid "$DEBUG_PID"
# 组件数随构建能力变化：仅 DEBUG_DUMP=3，再加 REPLAY/DIAGNOSTIC 时=4。
# 旧版硬编码「组件 3 个」，在 build-enhanced / build-doctor-full 上会误报；
# 这里断言「至少 3 个核心组件」（scheduler/runtime/events），与开关解耦。
STATUS_OUT="$(timeout "$CASE_TIMEOUT" "${CLI_DEBUG[@]}" status --pid "$DEBUG_PID" 2>&1)"
STATUS_CODE=$?
COMPONENTS="$(printf '%s' "$STATUS_OUT" | sed -n 's/.*组件 \([0-9][0-9]*\) 个.*/\1/p' | head -1)"
if [ "$STATUS_CODE" -eq 0 ] && [ -n "$COMPONENTS" ] && [ "$COMPONENTS" -ge 3 ]; then
    pass "status:组件数>=3" normal "组件 $COMPONENTS 个"
else
    fail "status:组件数>=3" normal "exit=$STATUS_CODE components='${COMPONENTS:-}' head=$(printf '%s' "$STATUS_OUT" | head -3 | tr '\n' '|')"
fi

expect_ok "dump-json-all:schema" normal '"schema": "go2cpp.dump.all/1"' \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID"
expect_ok "dump-json-all:一致性标记" normal '"consistent": "per_call"' \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID"
expect_ok "dump-json-all:changed/seq 字段" normal '"event_seq_supported": true' \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID"
expect_ok "dump-json-all:unavailable 清单" normal '"unavailable"' \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID"

expect_ok "dump-text-all:聚合段" normal "[component scheduler]" \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID" --format text
expect_ok "dump-text-all:runtime 段" normal "[component runtime]" \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID" --format text
expect_ok "dump-text-all:events 段" normal "[component events]" \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID" --format text

expect_ok "dump-scheduler:schema" normal '"schema": "go2cpp.dump.scheduler/1"' \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID" --sections scheduler
expect_ok "dump-selection:多组件聚合" normal '"schema": "go2cpp-cli.dump.selection/1"' \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID" --sections scheduler,events

expect_ok "ping:PONG" normal "pong" "${PROBE[@]}" ping
expect_ok "list:scheduler 可用" normal $'scheduler\tavailable' "${PROBE[@]}" list
expect_ok "list:fiber 未注册" normal $'fiber\tunavailable' "${PROBE[@]}" list
expect_ok "events:事件环 schema" normal "schema=go2cpp.dump.events/1" "${PROBE[@]}" events
expect_ok "events:包含事件行" normal "kRunQueue" "${PROBE[@]}" events

# ---------------- 错误路径 ----------------
expect_fail "dump-missing:未注册 fiber 被拒绝" error "未注册组件 'fiber'" \
    "${CLI_DEBUG[@]}" dump --pid "$DEBUG_PID" --sections goroutines
expect_fail "events-bad-limit:非法 limit" error "invalid-limit" \
    "${PROBE[@]}" raw --text "EVENTS abc"
expect_fail "subscribe-bad-topic:非法 topic" error "unknown-topic" \
    "${PROBE[@]}" subscribe --topic bogus
expect_fail "subscribe-bad-interval:interval=0" error "invalid-interval" \
    "${PROBE[@]}" subscribe --interval 0
expect_fail "subscribe-bad-max:max_events=0" error "invalid-max-events" \
    "${PROBE[@]}" subscribe --max-events 0

# ---------------- SUBSCRIBE 推送 ----------------
OUT="$(timeout 15 "${PROBE[@]}" subscribe --frames 2 --max-events 16 2>&1)"; CODE=$?
expect_contains "subscribe:header schema" normal "schema=go2cpp.dump.events.subscription/1" "$OUT"
expect_contains "subscribe:header capacity" normal "critical_capacity=" "$OUT"
expect_contains "subscribe:至少两帧 PUSH" normal "[frame 2]" "$OUT"
expect_contains "subscribe:帧含 event_count" normal "event_count=" "$OUT"
if [ "$CODE" -eq 0 ]; then
    pass "subscribe:客户端正常退出" normal
else
    fail "subscribe:客户端正常退出" normal "exit=$CODE out=$(echo "$OUT" | tail -2 | tr '\n' '|')"
fi

# ---------------- 并发 / 资源限制 ----------------
OUT="$(timeout 30 "${PROBE[@]}" concurrent --count 20 --command-text STATUS 2>&1)"; CODE=$?
expect_contains "concurrent-20:全部成功" concurrency "ok=20 failures=0" "$OUT"
if [ "$CODE" -eq 0 ]; then
    pass "concurrent-20:退出码 0" concurrency
else
    fail "concurrent-20:退出码 0" concurrency "$OUT"
fi

# 慢连接（不发送任何字节）会占住单线程 accept + 5s SO_RCVTIMEO。
python3 "$HERE/debug_probe.py" "$DEBUG_SOCK" slow-hold --seconds 3.0 >/dev/null 2>&1 &
SLOW_PID=$!
sleep 0.3
START=$(now_ms)
SLOW_OUT="$(timeout 10 "${CLI_DEBUG[@]}" status --pid "$DEBUG_PID" 2>&1)"; SLOW_CODE=$?
END=$(now_ms)
SLOW_MS=$((END - START))
wait "$SLOW_PID" 2>/dev/null || true
if [ "$SLOW_CODE" -eq 0 ] && [ "$SLOW_MS" -lt 1000 ]; then
    pass "slow-client:慢连接不阻塞 STATUS" concurrency "elapsed_ms=$SLOW_MS"
else
    fail "slow-client:慢连接不阻塞 STATUS" concurrency \
        "elapsed_ms=$SLOW_MS exit=$SLOW_CODE（慢连接无数据时服务端串行阻塞；期望 <1000ms）"
fi
add_stat "slow_client_status_ms" "$SLOW_MS" "1 条慢连接占用 accept 时 STATUS 延迟"

# 超长请求（8192 字节无换行）：服务端请求上限 4096，应快速拒绝而不是挂死。
LONG_TEXT="$(printf 'A%.0s' $(seq 1 8192))"
START=$(now_ms)
OUT="$(timeout 10 "${PROBE[@]}" raw --text "$LONG_TEXT" --no-newline 2>&1)"; CODE=$?
END=$(now_ms)
LONG_MS=$((END - START))
if [ "$LONG_MS" -lt 3000 ] && { printf '%s' "$OUT" | grep -qF "GO2CPP-DEBUG/1 ERR" || [ "$CODE" -eq 3 ]; }; then
    pass "oversize-request:超长请求有界拒绝" boundary "elapsed_ms=$LONG_MS exit=$CODE"
else
    fail "oversize-request:超长请求有界拒绝" boundary \
        "elapsed_ms=$LONG_MS exit=$CODE head=$(printf '%s' "$OUT" | head -2 | tr '\n' '|')"
fi
expect_ok "oversize-request:服务仍可 PING" boundary "pong" "${PROBE[@]}" ping

# 截断请求（半个命令后关闭写端）：不能崩溃，后续请求仍可用。
OUT="$(timeout 10 "${PROBE[@]}" raw --text "STAT" --no-newline 2>&1)"; CODE=$?
if printf '%s' "$OUT" | grep -qF "GO2CPP-DEBUG/1 ERR"; then
    pass "truncated-request:半条命令返回 ERR" error
else
    fail "truncated-request:半条命令返回 ERR" error \
        "exit=$CODE head=$(printf '%s' "$OUT" | head -2 | tr '\n' '|')"
fi
expect_ok "truncated-request:服务仍可 PING" error "pong" "${PROBE[@]}" ping

# 慢订阅者（不读 PUSH）：accept 循环不应被拖住。
python3 "$HERE/debug_probe.py" "$DEBUG_SOCK" hold-subscribe --seconds 2.0 >/dev/null 2>&1 &
HOLD_PID=$!
sleep 0.3
START=$(now_ms)
HOLD_OUT="$(timeout 10 "${CLI_DEBUG[@]}" status --pid "$DEBUG_PID" 2>&1)"; HOLD_CODE=$?
END=$(now_ms)
HOLD_MS=$((END - START))
wait "$HOLD_PID" 2>/dev/null || true
if [ "$HOLD_CODE" -eq 0 ] && [ "$HOLD_MS" -lt 1500 ]; then
    pass "slow-subscriber:慢订阅者不阻塞 STATUS" resource "elapsed_ms=$HOLD_MS"
else
    fail "slow-subscriber:慢订阅者不阻塞 STATUS" resource \
        "elapsed_ms=$HOLD_MS exit=$HOLD_CODE"
fi

# ---------------- 失败注入：SIGTERM 清理 ----------------
kill -TERM "$DEBUG_PID" 2>/dev/null || true
for _ in $(seq 1 100); do
    kill -0 "$DEBUG_PID" 2>/dev/null || break
    sleep 0.02
done
wait "$DEBUG_PID" 2>/dev/null
SIG_CODE=$?
if [ ! -e "$DEBUG_SOCK" ]; then
    pass "sigterm:退出后清理 socket" inject
else
    fail "sigterm:退出后清理 socket" inject "socket 仍存在：$DEBUG_SOCK"
fi
if grep -qE 'stopped sent=' "$DEBUG_LOG" 2>/dev/null; then
    pass "sigterm:优雅退出并打印统计" inject
else
    fail "sigterm:优雅退出并打印统计" inject "log=$(tail -2 "$DEBUG_LOG" 2>/dev/null | tr '\n' '|')"
fi
DEBUG_PID=""

summary
