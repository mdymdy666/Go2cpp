#!/usr/bin/env bash
#
# tune_scenarios.sh — 只跑 Bento 的弱网场景矩阵（调参回归用）。
#
# 与 compare_transports.sh 使用完全相同的 netem 注入方式（prio + u32 端口过滤，
# 只影响探针端口），但不跑 TCP/UDP，单轮耗时约为完整对照的 1/3，便于对
# initial_cwnd / PTO / 快速重传等参数做多轮扫描。结果写 TSV。
#
# 用法：
#   OUT=/tmp/bento_tuning2/base.tsv bash tests/bento/tune_scenarios.sh
#   BENTO_EXTRA_ARGS="--initial-cwnd 96" OUT=/tmp/x.tsv bash tests/bento/tune_scenarios.sh
#
# 退出码：0 全部轮次完成且 qdisc 清理干净；1 环境/注入/清理失败。
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT:-/tmp/bento_tuning2/results.tsv}"
LOG_DIR="${LOG_DIR:-/tmp/bento-tuning-logs}"
BENTO_PROBE="${BENTO_PROBE:-/tmp/build-bento-wsl/bento_netem_probe}"
RUNS="${RUNS:-3}"
MESSAGES="${MESSAGES:-200}"
PAYLOAD="${PAYLOAD:-256}"
TIMEOUT_MS="${TIMEOUT_MS:-90000}"
RUN_TIMEOUT_S="${RUN_TIMEOUT_S:-180}"
SUDO_PASSWORD="${SUDO_PASSWORD:-root}"
BENTO_EXTRA_ARGS="${BENTO_EXTRA_ARGS:-}"

SCENARIO_NAMES=(
    loss_5_delay_50
    loss_20_delay_200_jitter_50
    loss_40_delay_100
    rate_1mbit_loss_10_delay_100
)
SCENARIO_NETEM=(
    "loss 5% delay 50ms"
    "loss 20% delay 200ms 50ms distribution normal"
    "loss 40% delay 100ms"
    "rate 1mbit loss 10% delay 100ms"
)

as_root() {
    printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' "$@" 2>/dev/null
}

die() {
    echo "tune_scenarios: ERROR: $*" >&2
    exit 1
}

qdisc_clear() {
    as_root tc qdisc del dev lo root >/dev/null 2>&1 || true
    local show
    show="$(tc qdisc show dev lo 2>/dev/null | head -n 1)"
    case "$show" in
        *noqueue* | *pfifo_fast* | *fq_codel*) return 0 ;;
        *)
            echo "tune_scenarios: qdisc 未清理干净: $show" >&2
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
    local run="$2"
    echo $((47000 + scenario_index * 100 + run))
}

[[ -x "$BENTO_PROBE" ]] || die "Bento 探针不可执行: $BENTO_PROBE"
command -v tc >/dev/null 2>&1 || die "缺少 tc"
printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' true 2>/dev/null || die "sudo 不可用"
mkdir -p "$LOG_DIR" "$(dirname "$OUT")"
qdisc_clear || die "初始 qdisc 不干净"

printf 'scenario\trun\tsent\tdelivered\tordered\tp50_ms\tp95_ms\tp99_ms\tretrans\tfast_retrans\tsrtt_ms\tpto_ms\twall_ms\textra\n' >"$OUT"
echo "tune_scenarios: 结果 -> $OUT，日志 -> $LOG_DIR，extra='${BENTO_EXTRA_ARGS}'"

for scenario_index in "${!SCENARIO_NAMES[@]}"; do
    scenario="${SCENARIO_NAMES[$scenario_index]}"
    # FILTER 为场景名子串：只跑匹配场景，便于单场景快速扫描。
    if [[ -n "${FILTER:-}" && "$scenario" != *"$FILTER"* ]]; then
        continue
    fi
    netem_args="${SCENARIO_NETEM[$scenario_index]}"
    ports=()
    for run in $(seq 1 "$RUNS"); do
        ports+=("$(port_for "$scenario_index" "$run")")
    done

    echo "== $scenario ($netem_args) =="
    netem_apply "$netem_args" "${ports[@]}" || {
        qdisc_clear || true
        die "netem 注入失败: $scenario"
    }

    for run in $(seq 1 "$RUNS"); do
        port="${ports[$((run - 1))]}"
        log="$LOG_DIR/${scenario}_r${run}_${RANDOM}.log"
        start_ns="$(date +%s%N)"
        # shellcheck disable=SC2086
        timeout "${RUN_TIMEOUT_S}s" "$BENTO_PROBE" --port "$port" --messages "$MESSAGES" \
            --payload "$PAYLOAD" --timeout-ms "$TIMEOUT_MS" $BENTO_EXTRA_ARGS >"$log" 2>&1
        rc=$?
        end_ns="$(date +%s%N)"
        wall_ms=$(((end_ns - start_ns) / 1000000))
        [[ "$wall_ms" -le 0 ]] && wall_ms=1

        line="$(grep -m1 'netem_probe reliable:' "$log" 2>/dev/null || true)"
        delivered=0
        ordered=0
        p50="-"
        p95="-"
        p99="-"
        retrans="-"
        fast_retrans="-"
        srtt="-"
        pto="-"
        if [[ -n "$line" ]]; then
            delivered="$(sed -n 's/.*delivered=\([0-9]*\)\/[0-9]*.*/\1/p' <<<"$line")"
            ordered="$(value_of ordered "$line")"
            p50="$(value_of p50_ms "$line")"
            p95="$(value_of p95_ms "$line")"
            p99="$(value_of p99_ms "$line")"
            retrans="$(value_of retransmits "$line")"
            fast_retrans="$(value_of fast_retransmits "$line")"
            srtt="$(value_of srtt_ms "$line")"
            pto="$(value_of pto_ms "$line")"
        fi
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$scenario" "$run" "$MESSAGES" "${delivered:-0}" "${ordered:-0}" "${p50:--}" \
            "${p95:--}" "${p99:--}" "${retrans:--}" "${fast_retrans:--}" "${srtt:--}" \
            "${pto:--}" "$wall_ms" "${BENTO_EXTRA_ARGS}" >>"$OUT"
        printf '  run=%d port=%s rc=%d delivered=%s p50=%s p95=%s retrans=%s wall=%sms\n' \
            "$run" "$port" "$rc" "${delivered:-0}" "${p50:--}" "${p95:--}" "${retrans:--}" \
            "$wall_ms"
        if [[ "$rc" -ne 0 ]]; then
            printf '    (rc=%d，详见 %s)\n' "$rc" "$log" >&2
        fi
    done

    qdisc_clear || die "场景 $scenario 结束后 qdisc 清理失败"
done

echo "tune_scenarios: 完成 $(( ${#SCENARIO_NAMES[@]} * RUNS )) 轮"
