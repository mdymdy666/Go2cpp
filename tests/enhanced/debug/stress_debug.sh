#!/usr/bin/env bash
# Debug 随机/压测（两阶段，避免 fuzz 后的 RSS 膨胀污染 dump 延迟）：
#   A. 10000 条随机协议帧，检查服务存活与 RSS；
#   B. 全新服务器上连续 1000 次 CLI dump，统计延迟分位。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=debug
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work debug-stress)"
DEBUG_PID=""
trap 'stop_pid "${DEBUG_PID:-}"; rm -rf "$WORK"' EXIT

# ---------------- A. 10000 条随机协议帧 ----------------
if ! start_debug_server 180; then
    fail "fixture:debug-server(fuzz)" stress "DebugServer 未就绪"
    summary
    exit 1
fi
RSS_BEFORE_FUZZ="$(ps -o rss= -p "$DEBUG_PID" 2>/dev/null | tr -d ' ')"
OUT="$(timeout 180 python3 "$HERE/debug_fuzz.py" --socket "$DEBUG_SOCK" --count 10000 2>&1)"; CODE=$?
echo "$OUT"
RESULT_LINE="$(printf '%s\n' "$OUT" | grep -oE 'RESULT .*')"
RSS_AFTER_FUZZ="$(ps -o rss= -p "$DEBUG_PID" 2>/dev/null | tr -d ' ')"
add_stat "protocol_fuzz_10000_ms" "$(printf '%s' "$RESULT_LINE" | grep -oE 'elapsed_ms=[0-9.]+' | cut -d= -f2)" "$RESULT_LINE"
add_stat "debug_rss_before_fuzz_kb" "${RSS_BEFORE_FUZZ:-0}" "10000 帧前 RSS"
add_stat "debug_rss_after_fuzz_kb" "${RSS_AFTER_FUZZ:-0}" "10000 帧后 RSS"
if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -q "server_alive=1"; then
    pass "fuzz:10000 条随机协议帧后服务存活" random \
        "$RESULT_LINE rss_before=${RSS_BEFORE_FUZZ}KB rss_after=${RSS_AFTER_FUZZ}KB"
else
    fail "fuzz:10000 条随机协议帧后服务存活" random "exit=$CODE $RESULT_LINE"
fi
stop_pid "$DEBUG_PID"
DEBUG_PID=""

# ---------------- B. 全新服务器上连续 1000 次 dump ----------------
if ! start_debug_server 180; then
    fail "fixture:debug-server(dump)" stress "第二台 DebugServer 未就绪"
    summary
    exit 1
fi
LAT_FILE="$WORK/dump_latencies.txt"
: > "$LAT_FILE"
FAILURES=0
START=$(now_ms)
for index in $(seq 1 1000); do
    T0=$(now_ms)
    if ! env GO2CPP_DEBUG_SOCKET="$DEBUG_SOCK" timeout 5 \
        "$CLI" dump --pid "$DEBUG_PID" --sections runtime >/dev/null 2>&1; then
        FAILURES=$((FAILURES + 1))
    fi
    T1=$(now_ms)
    echo $((T1 - T0)) >> "$LAT_FILE"
done
END=$(now_ms)
TOTAL_MS=$((END - START))
SORTED="$WORK/dump_sorted.txt"
sort -n "$LAT_FILE" > "$SORTED"
P50="$(sed -n '500p' "$SORTED")"
P95="$(sed -n '950p' "$SORTED")"
P99="$(sed -n '990p' "$SORTED")"
RSS_AFTER="$(ps -o rss= -p "$DEBUG_PID" 2>/dev/null | tr -d ' ')"
PING_OUT="$(timeout 5 python3 "$HERE/debug_probe.py" "$DEBUG_SOCK" ping 2>&1)"
add_stat "dump_1000_total_ms" "$TOTAL_MS" "连续 1000 次 CLI dump"
add_stat "dump_1000_p50_ms" "$P50" "每次一个 CLI 进程 + 一条协议请求"
add_stat "dump_1000_p95_ms" "$P95" "每次一个 CLI 进程 + 一条协议请求"
add_stat "dump_1000_p99_ms" "$P99" "每次一个 CLI 进程 + 一条协议请求"
add_stat "debug_rss_after_1000_dumps_kb" "${RSS_AFTER:-0}" "1000 次 dump 后服务端 RSS"
if [ "$FAILURES" -eq 0 ] && printf '%s' "$PING_OUT" | grep -q pong; then
    pass "stress:连续 1000 次 dump 全部成功" stress \
        "total_ms=$TOTAL_MS p50=${P50}ms p95=${P95}ms p99=${P99}ms rss_kb=$RSS_AFTER"
else
    fail "stress:连续 1000 次 dump 全部成功" stress \
        "failures=$FAILURES total_ms=$TOTAL_MS ping=$(echo "$PING_OUT" | head -1)"
fi

# ---------------- C. 全量 scheduler dump 的膨胀缺陷 ----------------
# 当前源码下 tasks 仍会累积大量 kDead G；30s 即可达十余万条、数十 MB。
# 用 --out 落盘后统计 kDead 数量与文件大小，避免把 64MiB 传输上限当作唯一判据。
sleep 40
FULL_FILE="$WORK/full.json"
FULL_OUT="$(env GO2CPP_DEBUG_SOCKET="$DEBUG_SOCK" timeout 60 "$CLI" dump --pid "$DEBUG_PID" \
    --sections scheduler --out "$FULL_FILE" 2>&1)"; FULL_CODE=$?
FULL_SIZE="$(stat -c%s "$FULL_FILE" 2>/dev/null || echo 0)"
DEAD_COUNT="$(grep -oE '"state"[[:space:]]*:[[:space:]]*"kDead"' "$FULL_FILE" 2>/dev/null | wc -l)"
add_stat "full_dump_30s_bytes" "$FULL_SIZE" "30s+ 后全量 scheduler dump 大小"
add_stat "full_dump_30s_kdead" "$DEAD_COUNT" "30s+ 后 dump 中的 kDead G 数量"
if [ "$FULL_CODE" -ne 0 ]; then
    fail "dump:30s+ 全量 scheduler 膨胀" stress \
        "exit=$FULL_CODE size=$FULL_SIZE $(printf '%s' "$FULL_OUT" | head -2 | tr '\n' '|')"
elif [ "$DEAD_COUNT" -gt 1000 ]; then
    fail "dump:30s+ 全量 scheduler 膨胀" stress \
        "kDead=$DEAD_COUNT size=${FULL_SIZE}B（终态 G 未回收；期望 tasks 只含存活 G）"
else
    pass "dump:30s+ 全量 scheduler 有界" stress \
        "kDead=$DEAD_COUNT size=${FULL_SIZE}B"
fi
stop_pid "$DEBUG_PID"
DEBUG_PID=""

# ---------------- D. SIGSEGV 注入：进程必须有界终止，不得挂死 ----------------
if start_debug_server 60; then
    kill -SEGV "$DEBUG_PID" 2>/dev/null || true
    for _ in $(seq 1 150); do
        kill -0 "$DEBUG_PID" 2>/dev/null || break
        sleep 0.02
    done
    if ! kill -0 "$DEBUG_PID" 2>/dev/null; then
        wait "$DEBUG_PID" 2>/dev/null
        pass "sigsegv:SIGSEGV 后有界终止" inject
    else
        fail "sigsegv:SIGSEGV 后有界终止" inject "3s 内未退出"
        stop_pid "$DEBUG_PID"
    fi
    DEBUG_PID=""
else
    fail "sigsegv:SIGSEGV 后有界终止" inject "无法启动独立 DebugServer"
fi

summary
