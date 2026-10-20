#!/usr/bin/env bash
# Replay 随机/压测：500 轮事件交错 + 100 轮 record/play。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=replay
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work replay-stress)"
FANIN="$WORK/fanin.g2cr"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$REPLAY_BIN" ] || [ ! -x "$RECORD_BIN" ]; then
    fail "fixture:replay" stress "缺少 replay/record 可执行文件"
    summary
    exit 1
fi
if ! "$RECORD_BIN" "$FANIN" >/dev/null 2>&1; then
    fail "fixture:record" stress "无法生成 fanin 记录"
    summary
    exit 1
fi

# ---------------- 500 轮随机事件交错 ----------------
START=$(now_ms)
OUT="$(timeout 180 python3 "$HERE/replay_fuzz.py" --input "$FANIN" \
    --replay "$REPLAY_BIN" --count 500 --mode mixed 2>&1)"; CODE=$?
END=$(now_ms)
echo "$OUT"
RESULT_LINE="$(printf '%s\n' "$OUT" | grep -oE 'RESULT .*')"
add_stat "event_interleave_500_ms" "$((END - START))" "$RESULT_LINE"
if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -qE 'crash=0 timeout=0'; then
    pass "fuzz:500 轮随机事件交错无崩溃/超时" random "$RESULT_LINE"
else
    fail "fuzz:500 轮随机事件交错无崩溃/超时" random "exit=$CODE $RESULT_LINE"
fi

# ---------------- 100 轮 record + play ----------------
OUT="$(timeout 180 python3 "$HERE/stress_replay.py" \
    --record "$RECORD_BIN" --replay "$REPLAY_BIN" --count 100 2>&1)"; CODE=$?
echo "$OUT"
RESULT_LINE="$(printf '%s\n' "$OUT" | grep -oE 'RESULT .*')"
if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -qE 'failures=0'; then
    pass "stress:100 轮 record+play 全部一致" stress "$RESULT_LINE"
else
    fail "stress:100 轮 record+play 全部一致" stress "exit=$CODE $RESULT_LINE"
fi
if printf '%s' "$RESULT_LINE" | grep -q 'p95_ms='; then
    P50="$(printf '%s' "$RESULT_LINE" | grep -oE 'p50_ms=[0-9.]+' | cut -d= -f2)"
    P95="$(printf '%s' "$RESULT_LINE" | grep -oE 'p95_ms=[0-9.]+' | cut -d= -f2)"
    P99="$(printf '%s' "$RESULT_LINE" | grep -oE 'p99_ms=[0-9.]+' | cut -d= -f2)"
    RSS="$(printf '%s' "$RESULT_LINE" | grep -oE 'rss_kb=[0-9]+' | cut -d= -f2)"
    add_stat "stress_p50_ms" "$P50" "100 轮每轮 record+play 延迟分位"
    add_stat "stress_p95_ms" "$P95" "100 轮每轮 record+play 延迟分位"
    add_stat "stress_p99_ms" "$P99" "100 轮每轮 record+play 延迟分位"
    add_stat "stress_max_rss_kb" "$RSS" "子进程峰值 RSS"
fi

summary
