#!/usr/bin/env bash
#
# compare_transports.sh — TCP / UDP / Bento 三方对照实测
#
# 在 loopback 上用 tc netem 注入 5 种弱网场景，每场景对三种传输各跑 3 轮，
# 记录送达率、p50/p95、重传与吞吐，汇总到 tests/bento/compare_results.tsv。
#
# 关键约束：
#   1. netem 只作用于探针端口（prio + u32 过滤），不污染 127.0.0.1 上的
#      DSH Web GUI / 其他回环服务；未匹配流量走 band 0 直通。
#   2. 每场景结束强制清理 qdisc，并用 `tc qdisc show dev lo` 校验。
#   3. 探针路径可用环境变量覆盖，默认取 /tmp 下的构建产物。
#
# 用法：
#   bash tests/bento/compare_transports.sh
#   TCP_PROBE=... UDP_PROBE=... BENTO_PROBE=... bash tests/bento/compare_transports.sh
#
# 依赖：tc（netem/u32/prio）、sudo（密码 root，按任务约定 `echo root | sudo -S`）。
# 退出码：0 全部场景完成且 qdisc 清理干净；1 环境/注入/清理失败。
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT_DIR/tests/bento/compare_results.tsv"
LOG_DIR="${LOG_DIR:-/tmp/transport-compare-logs}"
TCP_PROBE="${TCP_PROBE:-/tmp/probes/tcp_probe}"
UDP_PROBE="${UDP_PROBE:-/tmp/probes/udp_probe}"
BENTO_PROBE="${BENTO_PROBE:-/tmp/build-bento-wsl/bento_netem_probe}"

MESSAGES=200
PAYLOAD=256
RUNS=3
TIMEOUT_MS="${TIMEOUT_MS:-90000}"
RUN_TIMEOUT_S="${RUN_TIMEOUT_S:-180}"

SCENARIO_NAMES=(baseline loss_5_delay_50 loss_20_delay_200_jitter_50 loss_40_delay_100 rate_1mbit_loss_10_delay_100)
SCENARIO_NETEM=(
    ""
    "loss 5% delay 50ms"
    "loss 20% delay 200ms 50ms distribution normal"
    "loss 40% delay 100ms"
    "rate 1mbit loss 10% delay 100ms"
)

SUDO_PASSWORD="${SUDO_PASSWORD:-root}"
as_root() {
    printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' "$@" 2>/dev/null
}

die() {
    echo "compare_transports: ERROR: $*" >&2
    exit 1
}

qdisc_clear() {
    as_root tc qdisc del dev lo root >/dev/null 2>&1 || true
    local show
    show="$(tc qdisc show dev lo 2>/dev/null | head -n 1)"
    case "$show" in
        *noqueue* | *pfifo_fast* | *fq_codel*) return 0 ;;
        *)
            echo "compare_transports: qdisc 未清理干净: $show" >&2
            return 1
            ;;
    esac
}

cleanup() {
    local rc=$?
    qdisc_clear || true
    exit "$rc"
}
trap cleanup EXIT

# 只把探针端口放进 netem class（band 3）；双向 filter 保证请求与响应都受限。
netem_apply() {
    local netem_args="$1"
    shift
    local port
    qdisc_clear || return 1
    as_root tc qdisc add dev lo root handle 1: prio bands 4 || return 1
    # shellcheck disable=SC2086
    as_root tc qdisc add dev lo parent 1:3 handle 30: netem $netem_args || return 1
    for port in "$@"; do
        as_root tc filter add dev lo protocol ip parent 1:0 prio 3 \
            u32 match ip dport "$port" 0xffff flowid 1:3 || return 1
        as_root tc filter add dev lo protocol ip parent 1:0 prio 3 \
            u32 match ip sport "$port" 0xffff flowid 1:3 || return 1
    done
    return 0
}

# TCP/UDP 的 RESULT 行与 Bento 的 reliable 行都是 key=value，用同一提取器。
value_of() {
    local key="$1"
    local line="$2"
    awk -v k="$key" '{
        for (i = 1; i <= NF; i++) {
            if (index($i, k "=") == 1) {
                sub(k "=", "", $i);
                print $i;
                exit;
            }
        }
    }' <<<"$line"
}

port_for() {
    local scenario_index="$1"
    local transport_index="$2"
    local run="$3"
    echo $((46000 + scenario_index * 100 + run * 10 + transport_index))
}

write_row() {
    local scenario="$1" transport="$2" run="$3" sent="$4" delivered="$5" ordered="$6"
    local p50="$7" p95="$8" p99="$9" retrans="${10}" wall_ms="${11}"
    local throughput
    throughput="$(awk -v d="$delivered" -v b="$PAYLOAD" -v w="$wall_ms" \
        'BEGIN { if (w > 0) printf "%.3f", (d * b * 8.0) / (w / 1000.0) / 1e6; else printf "0.000" }')"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$scenario" "$transport" "$run" "$sent" "$delivered" "$ordered" \
        "$p50" "$p95" "$p99" "$retrans" "$wall_ms" "$throughput" >>"$OUT"
}

# 跑一轮并解析；探针失败也记录（送达数可能非零），不中断整个矩阵。
run_one() {
    local transport="$1" scenario="$2" run="$3" port="$4"
    local log="$LOG_DIR/${scenario}_${transport}_r${run}.log"
    local start_ns end_ns wall_ms rc line delivered sent ordered p50 p95 p99 retrans
    start_ns="$(date +%s%N)"
    case "$transport" in
        tcp)
            timeout "${RUN_TIMEOUT_S}s" "$TCP_PROBE" --port "$port" --messages "$MESSAGES" \
                --timeout-ms "$TIMEOUT_MS" --label "$scenario" --run "$run" >"$log" 2>&1
            ;;
        udp)
            timeout "${RUN_TIMEOUT_S}s" "$UDP_PROBE" --port "$port" --messages "$MESSAGES" \
                --timeout-ms "$TIMEOUT_MS" --label "$scenario" --run "$run" >"$log" 2>&1
            ;;
        bento)
            timeout "${RUN_TIMEOUT_S}s" "$BENTO_PROBE" --port "$port" --messages "$MESSAGES" \
                --payload "$PAYLOAD" --timeout-ms "$TIMEOUT_MS" >"$log" 2>&1
            ;;
        *)
            die "未知传输: $transport"
            ;;
    esac
    rc=$?
    end_ns="$(date +%s%N)"
    wall_ms=$(((end_ns - start_ns) / 1000000))
    [[ "$wall_ms" -le 0 ]] && wall_ms=1

    sent="$MESSAGES"
    delivered=0
    ordered=0
    p50="-"
    p95="-"
    p99="-"
    retrans="-"
    if [[ "$transport" == "bento" ]]; then
        line="$(grep -m1 'netem_probe reliable:' "$log" 2>/dev/null || true)"
        if [[ -n "$line" ]]; then
            delivered="$(sed -n 's/.*delivered=\([0-9]*\)\/[0-9]*.*/\1/p' <<<"$line")"
            ordered="$(value_of ordered "$line")"
            p50="$(value_of p50_ms "$line")"
            p95="$(value_of p95_ms "$line")"
            p99="$(value_of p99_ms "$line")"
            retrans="$(value_of retransmits "$line")"
        fi
    else
        line="$(grep -m1 '^RESULT ' "$log" 2>/dev/null || true)"
        if [[ -n "$line" ]]; then
            delivered="$(value_of delivered "$line")"
            ordered="$(value_of ordered "$line")"
            p50="$(value_of p50_ms "$line")"
            p95="$(value_of p95_ms "$line")"
            p99="-"
            retrans="$(value_of retrans "$line")"
        fi
    fi
    delivered="${delivered:-0}"
    ordered="${ordered:-0}"
    p50="${p50:--}"
    p95="${p95:--}"
    p99="${p99:--}"
    retrans="${retrans:--}"

    write_row "$scenario" "$transport" "$run" "$sent" "$delivered" "$ordered" \
        "$p50" "$p95" "$p99" "$retrans" "$wall_ms"
    printf '  %-8s run=%d port=%s rc=%d delivered=%s/%s p50=%s p95=%s retrans=%s wall=%sms\n' \
        "$transport" "$run" "$port" "$rc" "$delivered" "$sent" "$p50" "$p95" "$retrans" "$wall_ms"
    if [[ "$rc" -ne 0 ]]; then
        printf '    (rc=%d，详见 %s)\n' "$rc" "$log" >&2
    fi
}

# ---- 预检 ----
[[ -x "$TCP_PROBE" ]] || die "TCP 探针不可执行: $TCP_PROBE"
[[ -x "$UDP_PROBE" ]] || die "UDP 探针不可执行: $UDP_PROBE"
[[ -x "$BENTO_PROBE" ]] || die "Bento 探针不可执行: $BENTO_PROBE"
command -v tc >/dev/null 2>&1 || die "缺少 tc"
printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' true 2>/dev/null || die "sudo 不可用（密码错误或未安装）"
mkdir -p "$LOG_DIR"
qdisc_clear || die "初始 qdisc 不干净"

printf 'scenario\ttransport\trun\tsent\tdelivered\tordered\tp50_ms\tp95_ms\tp99_ms\tretrans\twall_ms\tthroughput_mbps\n' >"$OUT"
echo "compare_transports: 结果 -> $OUT，日志 -> $LOG_DIR"

TRANSPORTS=(tcp udp bento)
for scenario_index in "${!SCENARIO_NAMES[@]}"; do
    scenario="${SCENARIO_NAMES[$scenario_index]}"
    netem_args="${SCENARIO_NETEM[$scenario_index]}"
    ports=()
    for transport_index in 1 2 3; do
        for run in $(seq 1 "$RUNS"); do
            ports+=("$(port_for "$scenario_index" "$transport_index" "$run")")
        done
    done

    echo "== 场景 $scenario (${netem_args:-无 netem}) =="
    if [[ -n "$netem_args" ]]; then
        netem_apply "$netem_args" "${ports[@]}" || {
            qdisc_clear || true
            die "netem 注入失败: $scenario"
        }
        tc qdisc show dev lo | sed 's/^/  qdisc: /'
    else
        qdisc_clear || die "baseline 清理失败"
    fi

    for transport_index in 1 2 3; do
        transport="${TRANSPORTS[$((transport_index - 1))]}"
        for run in $(seq 1 "$RUNS"); do
            port="$(port_for "$scenario_index" "$transport_index" "$run")"
            run_one "$transport" "$scenario" "$run" "$port"
        done
    done

    if ! qdisc_clear; then
        die "场景 $scenario 结束后 qdisc 清理失败"
    fi
    echo "  qdisc 清理校验通过: $(tc qdisc show dev lo | head -n 1)"
done

echo "compare_transports: 完成，共 $(( ${#SCENARIO_NAMES[@]} * 3 * RUNS )) 轮"
