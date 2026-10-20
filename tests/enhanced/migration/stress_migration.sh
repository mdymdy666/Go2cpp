#!/usr/bin/env bash
# Migration 随机/压测：1000 次随机位翻转 + 100 轮 dump/restore。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=migration
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work migration-stress)"
SNAP="$WORK/fiber.mig"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$MIGRATE" ]; then
    fail "fixture:migrate" stress "缺少 $MIGRATE"
    summary
    exit 1
fi
if ! "$MIGRATE" dump "$SNAP" >/dev/null 2>&1; then
    fail "fixture:dump" stress "无法生成快照"
    summary
    exit 1
fi

# ---------------- 1000 次随机位翻转 ----------------
START=$(now_ms)
OUT="$(timeout 300 python3 "$HERE/snapshot_fuzz.py" bitflip \
    --input "$SNAP" --migrate "$MIGRATE" --count 1000 2>&1)"; CODE=$?
END=$(now_ms)
echo "$OUT"
RESULT_LINE="$(printf '%s\n' "$OUT" | grep -oE 'RESULT .*')"
add_stat "bitflip_1000_ms" "$((END - START))" "$RESULT_LINE"
if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -qE 'accepted=0 crashed=0 timeout=0'; then
    pass "fuzz:1000 次随机位翻转全部被拒绝" random "$RESULT_LINE"
else
    fail "fuzz:1000 次随机位翻转全部被拒绝" random "exit=$CODE $RESULT_LINE"
fi

# ---------------- 100 轮 dump + restore ----------------
OUT="$(timeout 300 python3 "$HERE/stress_migration.py" \
    --migrate "$MIGRATE" --count 100 2>&1)"; CODE=$?
echo "$OUT"
RESULT_LINE="$(printf '%s\n' "$OUT" | grep -oE 'RESULT .*')"
if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -qE 'failures=0'; then
    pass "stress:100 轮 dump+restore 全部成功" stress "$RESULT_LINE"
else
    fail "stress:100 轮 dump+restore 全部成功" stress "exit=$CODE $RESULT_LINE"
fi
if printf '%s' "$RESULT_LINE" | grep -q 'p95_ms='; then
    P50="$(printf '%s' "$RESULT_LINE" | grep -oE 'p50_ms=[0-9.]+' | cut -d= -f2)"
    P95="$(printf '%s' "$RESULT_LINE" | grep -oE 'p95_ms=[0-9.]+' | cut -d= -f2)"
    P99="$(printf '%s' "$RESULT_LINE" | grep -oE 'p99_ms=[0-9.]+' | cut -d= -f2)"
    RSS="$(printf '%s' "$RESULT_LINE" | grep -oE 'rss_kb=[0-9]+' | cut -d= -f2)"
    add_stat "stress_p50_ms" "$P50" "100 轮每轮 dump+restore 延迟分位"
    add_stat "stress_p95_ms" "$P95" "100 轮每轮 dump+restore 延迟分位"
    add_stat "stress_p99_ms" "$P99" "100 轮每轮 dump+restore 延迟分位"
    add_stat "stress_max_rss_kb" "$RSS" "子进程峰值 RSS"
fi

summary
