#!/usr/bin/env bash
# WebSocket 长连接压测编排：启动服务端 → 采服务端 RSS/CPU → 跑客户端 → 收尾。
#
# 用法：
#   BIN=/tmp/ws_bench_bin bash app/ws_bench/run_bench.sh <go2cpp|go> <conn> <duration_s> <port> [workers]
#
# 输出（stdout）：一行合并后的 JSON（服务端 RSS/CPU + 客户端统计）。
# 中间日志：$OUT/server-$kind.log；结果追加到 $OUT/results.jsonl。
#
# 约束：服务端 pin 到 CPU 2-9，客户端 pin 到 CPU 0，避免压测机自身抢核干扰。
set -euo pipefail

KIND=${1:?usage: run_bench.sh <go2cpp|go> <conn> <duration_s> <port> [workers]}
CONN=${2:?conn}
DURATION=${3:?duration}
PORT=${4:?port}
WORKERS=${5:-4}

BIN=/home/ubunturoot/wsbench-bin
OUT=${OUT:-$HOME/wsbench-results}
mkdir -p "$OUT"
ulimit -n 200000

SERVER_LOG="$OUT/server-$KIND-$CONN.log"
RESULT_LINE=""

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.05
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$OUT/stop-$KIND-$CONN"
}
trap cleanup EXIT

# 端口必须空闲，否则上一档没退干净。
if ss -ltn "sport = :$PORT" 2>/dev/null | grep -q LISTEN; then
    echo "port $PORT still in use" >&2
    exit 1
fi

pushd "$(dirname "$0")/../.." >/dev/null
if [[ "$KIND" == "go" ]]; then
    taskset -c 2-9 "$BIN/ws_bench_go" --port "$PORT" --workers "$WORKERS" \
        >"$SERVER_LOG" 2>&1 &
else
    taskset -c 2-9 "$BIN/ws_bench_server" --port "$PORT" --workers "$WORKERS" \
        >"$SERVER_LOG" 2>&1 &
fi
SERVER_PID=$!
popd >/dev/null

for _ in $(seq 1 100); do
    grep -q '^READY ' "$SERVER_LOG" 2>/dev/null && break
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "server died, see $SERVER_LOG" >&2
        cat "$SERVER_LOG" >&2
        exit 1
    fi
    sleep 0.1
done
grep -q '^READY ' "$SERVER_LOG" || { echo "server not ready" >&2; exit 1; }

STOP_FILE="$OUT/stop-$KIND-$CONN"
rm -f "$STOP_FILE"
SAMPLE_FILE="$OUT/rss-$KIND-$CONN.txt"
: >"$SAMPLE_FILE"

# 每秒采样服务端 RSS 与 CPU（utime+stime，单位 USER_HZ）。
(
    ticks=$(getconf CLK_TCK)
    prev_total=0
    first=1
    while [[ ! -f "$STOP_FILE" ]]; do
        if [[ -r "/proc/$SERVER_PID/status" ]]; then
            rss=$(awk '/^VmRSS:/{print $2}' "/proc/$SERVER_PID/status" 2>/dev/null || echo 0)
            read -r utime stime < <(awk '{print $14, $15}' "/proc/$SERVER_PID/stat" 2>/dev/null || echo "0 0")
            total=$((utime + stime))
            if [[ $first -eq 1 ]]; then
                cpu=0
                first=0
            else
                cpu=$(( (total - prev_total) * 100 / ticks ))
            fi
            prev_total=$total
            echo "$rss $cpu" >>"$SAMPLE_FILE"
        fi
        sleep 1
    done
) &
SAMPLER_PID=$!

CLIENT_LOG="$OUT/client-$KIND-$CONN.log"
taskset -c 0 "$BIN/ws_bench_load" --conn "$CONN" --url "ws://127.0.0.1:$PORT/ws" \
    --duration "$DURATION" --ping-ms 1000 --label "$KIND" >"$CLIENT_LOG" 2>&1 || {
    echo "client failed, see $CLIENT_LOG" >&2
    cat "$CLIENT_LOG" >&2
    exit 1
}

touch "$STOP_FILE"
wait "$SAMPLER_PID" 2>/dev/null || true

# 收尾：SIGTERM 服务端，读它打印的 STATS。
kill -TERM "$SERVER_PID" 2>/dev/null || true
for _ in $(seq 1 200); do
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.05
done
kill -KILL "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

peak_rss=$(awk 'BEGIN{m=0} {if ($1>m) m=$1} END{print m+0}' "$SAMPLE_FILE")
avg_rss=$(awk 'BEGIN{s=0;n=0} {s+=$1;n++} END{if(n==0) print 0; else printf "%d", s/n}' "$SAMPLE_FILE")
avg_cpu=$(awk 'BEGIN{s=0;n=0} {s+=$2;n++} END{if(n==0) print 0; else printf "%.1f", s/n}' "$SAMPLE_FILE")
max_cpu=$(awk 'BEGIN{m=0} {if ($2>m) m=$2} END{print m+0}' "$SAMPLE_FILE")
stats=$(grep '^STATS ' "$SERVER_LOG" | tail -1 || true)

client_json=$(tail -1 "$CLIENT_LOG")
merged=$(printf '%s' "$client_json" | sed "s/^{/{\"server\":\"$KIND\",\"rss_peak_kb\":$peak_rss,\"rss_avg_kb\":$avg_rss,\"cpu_avg_pct\":$avg_cpu,\"cpu_max_pct\":$max_cpu,/")
echo "$merged" | tee -a "$OUT/results.jsonl"
echo "$stats" >&2
