#!/usr/bin/env bash
#
# wait_low_load.sh —— 等待机器进入低负载，并给出一个可用于绑核的核号。
#
# 判定标准（两条同时满足才算低负载）：
#   1. load average(1min) < MAX_LOAD（默认 1.0；12 核机器约 8% 占用）；
#   2. 目标核 idle > MIN_IDLE（默认 90%；用 /proc/stat 的 1s 差值计算，
#      idle 计入 iowait）。
#
# 用法：
#   scripts/wait_low_load.sh            # 自动选当前最闲的核
#   scripts/wait_low_load.sh 6          # 只认 6 号核，不满足就等
#   MAX_LOAD=1.5 MIN_IDLE=85 scripts/wait_low_load.sh 6
#
# 输出约定：
#   成功：stdout 只打印一行 `LOW_LOAD_OK core=N`（可被 $(...) 直接捕获），退出码 0；
#   超时：退出码 1，最后一轮的状态写 stderr。
#   所有过程信息写 stderr，不污染 stdout。
#
# 只读 /proc/loadavg 与 /proc/stat；不改系统状态、不杀任何进程。
#
# @author go2cpp contributors
# @date 2026-11-05

set -uo pipefail

MAX_LOAD="${MAX_LOAD:-1.0}"
MIN_IDLE="${MIN_IDLE:-90}"
TIMEOUT_S="${TIMEOUT_S:-300}"
INTERVAL_S="${INTERVAL_S:-5}"

# 第一个位置参数是核号；也接受 CORE 环境变量。空串表示自动选核。
CORE="${1:-${CORE:-}}"
if [ -n "$CORE" ]; then
    case "$CORE" in
        *[!0-9]*)
            echo "[wait_low_load] 核号必须是整数：$CORE" >&2
            exit 2
            ;;
    esac
fi

# 读 /proc/stat 的每核累计计数，输出 `核号 idle 总计`。
# idle 取 idle + iowait（等待 I/O 的核同样不可用于算力基准）。
read_cpu_stats() {
    awk -v target="$1" '
        $1 ~ /^cpu[0-9]+$/ {
            idx = substr($1, 4) + 0
            if (target != "" && idx != target) next
            total = 0
            for (i = 2; i <= NF; i++) total += $i
            idle = $5 + $6
            printf "%d %d %d\n", idx, idle, total
        }
    ' /proc/stat
}

load_below_threshold() {
    awk -v got="$1" -v limit="$2" 'BEGIN { exit !(got < limit) }'
}

deadline=$(( $(date +%s) + TIMEOUT_S ))
attempt=0
last_load=""
last_core=""
last_idle=""

while :; do
    attempt=$((attempt + 1))
    load1=$(awk '{ print $1 }' /proc/loadavg)

    declare -A prev_idle=()
    declare -A prev_total=()
    while read -r idx idle total; do
        [ -n "${idx:-}" ] || continue
        prev_idle[$idx]=$idle
        prev_total[$idx]=$total
    done < <(read_cpu_stats "$CORE")
    sleep 1

    best_core=""
    best_idle=-1
    while read -r idx idle total; do
        [ -n "${idx:-}" ] || continue
        [ -n "${prev_total[$idx]:-}" ] || continue
        delta_idle=$(( idle - prev_idle[$idx] ))
        delta_total=$(( total - prev_total[$idx] ))
        [ "$delta_total" -gt 0 ] || continue
        pct=$(( delta_idle * 100 / delta_total ))
        if [ "$pct" -gt "$best_idle" ]; then
            best_idle=$pct
            best_core=$idx
        fi
    done < <(read_cpu_stats "$CORE")

    last_load=$load1
    last_core=$best_core
    last_idle=$best_idle

    if [ -n "$best_core" ] && load_below_threshold "$load1" "$MAX_LOAD" \
        && [ "$best_idle" -gt "$MIN_IDLE" ]; then
        echo "[wait_low_load] 第 ${attempt} 次检查：load=$load1 core=$best_core idle=${best_idle}% → 通过" >&2
        echo "LOW_LOAD_OK core=$best_core"
        exit 0
    fi

    echo "[wait_low_load] 第 ${attempt} 次检查：load=$load1（需 <$MAX_LOAD）core=${best_core:-?} idle=${best_idle}%（需 >$MIN_IDLE%）→ 等待 ${INTERVAL_S}s" >&2

    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "[wait_low_load] 超时 ${TIMEOUT_S}s：load=$last_load core=${last_core:-?} idle=${last_idle}%" >&2
        exit 1
    fi
    sleep "$INTERVAL_S"
done
