#!/usr/bin/env bash
#
# verify_fixes.sh — 三个 transport 修复的定向验证（只跑 Bento 探针）。
#
# 复用 compare_transports.sh 的 netem 隔离方式（prio + u32 只作用于探针端口），
# 覆盖三个待验证修复各自的关键场景：
#   1) 不可靠流乱序容忍：20% + 200ms±50ms 送达率应 > 50%（修复前 5%），
#      且应用层 ordered=1（0 回退）；
#   2) 握手冗余：40% 丢包 3/3 轮成功（修复前 1/3 轮 15s 超时）；
#   3) PTO 自适应：5%+50ms p95 应 < 300ms、20%+200ms p95 应 < 2000ms
#      （修复前 527ms / 3441ms）。
#
# 输出：tests/bento/verify_fixes_results.tsv（每场景 3 轮）。
# 用法：bash tests/bento/verify_fixes.sh
# 依赖：tc（netem/u32/prio）、sudo（密码 root，按任务约定 echo root | sudo -S）。
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT_DIR/tests/bento/verify_fixes_results.tsv"
LOG_DIR="${LOG_DIR:-/tmp/verify-fixes-logs}"
BENTO_PROBE="${BENTO_PROBE:-/tmp/build-bento-wsl/bento_netem_probe}"

MESSAGES=200
PAYLOAD=256
RUNS=3
TIMEOUT_MS="${TIMEOUT_MS:-90000}"
RUN_TIMEOUT_S="${RUN_TIMEOUT_S:-180}"

SCENARIO_NAMES=(loss_5_delay_50 loss_20_delay_200_jitter_50 loss_40_delay_100)
SCENARIO_NETEM=(
    "loss 5% delay 50ms"
    "loss 20% delay 200ms 50ms distribution normal"
    "loss 40% delay 100ms"
)

SUDO_PASSWORD="${SUDO_PASSWORD:-root}"
as_root() {
    printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' "$@" 2>/dev/null
}

die() {
    echo "verify_fixes: ERROR: $*" >&2
    exit 1
}

qdisc_clear() {
    as_root tc qdisc del dev lo root >/dev/null 2>&1 || true
    local show
    show="$(tc qdisc show dev lo 2>/dev/null | head -n 1)"
    case "$show" in
        *noqueue* | *pfifo_fast* | *fq_codel*) return 0 ;;
        *)
            echo "verify_fixes: qdisc 未清理干净: $show" >&2
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
    echo $((47100 + scenario_index * 10 + run))
}

run_one() {
    local scenario="$1" run="$2" port="$3"
    local log="$LOG_DIR/${scenario}_r${run}.log"
    local start_ns end_ns wall_ms rc line reliable unreliable
    local delivered p50 p95 retrans unrel_delivered unrel_stale
    local reorder_buffered reorder_skipped ordered unrel_ordered

    start_ns="$(date +%s%N)"
    timeout "${RUN_TIMEOUT_S}s" "$BENTO_PROBE" --port "$port" --messages "$MESSAGES" \
        --payload "$PAYLOAD" --timeout-ms "$TIMEOUT_MS" >"$log" 2>&1
    rc=$?
    end_ns="$(date +%s%N)"
    wall_ms=$(((end_ns - start_ns) / 1000000))
    [[ "$wall_ms" -le 0 ]] && wall_ms=1

    delivered=0
    p50="-"
    p95="-"
    retrans="-"
    ordered=0
    unrel_delivered=0
    unrel_stale="-"
    unrel_ordered=0
    reorder_buffered="-"
    reorder_skipped="-"
    reliable="$(grep -m1 'netem_probe reliable:' "$log" 2>/dev/null || true)"
    if [[ -n "$reliable" ]]; then
        delivered="$(sed -n 's/.*delivered=\([0-9]*\)\/[0-9]*.*/\1/p' <<<"$reliable")"
        ordered="$(value_of ordered "$reliable")"
        p50="$(value_of p50_ms "$reliable")"
        p95="$(value_of p95_ms "$reliable")"
        retrans="$(value_of retransmits "$reliable")"
    fi
    unreliable="$(grep -m1 'netem_probe unreliable:' "$log" 2>/dev/null || true)"
    if [[ -n "$unreliable" ]]; then
        unrel_delivered="$(sed -n 's/.*delivered=\([0-9]*\)\/[0-9]*.*/\1/p' <<<"$unreliable")"
        unrel_ordered="$(value_of ordered "$unreliable")"
        unrel_stale="$(value_of stale_dropped "$unreliable")"
        reorder_buffered="$(value_of reorder_buffered "$unreliable")"
        reorder_skipped="$(value_of reorder_skipped "$unreliable")"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$scenario" "$run" "$rc" "${delivered:-0}" "${ordered:-0}" \
        "${p50:--}" "${p95:--}" "${retrans:--}" "${unrel_delivered:-0}" \
        "${unrel_ordered:-0}" "${unrel_stale:--}" "${wall_ms}" >>"$OUT"
    printf '  %-30s run=%d rc=%d reliable=%s/200 p50=%s p95=%s retrans=%s | unreliable=%s/200 ordered=%s stale=%s reorder=%s/%s\n' \
        "$scenario" "$run" "$rc" "${delivered:-0}" "${p50:--}" "${p95:--}" \
        "${retrans:--}" "${unrel_delivered:-0}" "${unrel_ordered:-0}" \
        "${unrel_stale:--}" "${reorder_buffered:--}" "${reorder_skipped:--}"
}

# ---- 预检 ----
[[ -x "$BENTO_PROBE" ]] || die "Bento 探针不可执行: $BENTO_PROBE"
command -v tc >/dev/null 2>&1 || die "缺少 tc"
printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' true 2>/dev/null || die "sudo 不可用"
mkdir -p "$LOG_DIR"
qdisc_clear || die "初始 qdisc 不干净"

printf 'scenario\trun\trc\tdelivered\tordered\tp50_ms\tp95_ms\tretrans\tunreliable_delivered\tunreliable_ordered\tunreliable_stale\twall_ms\n' >"$OUT"
echo "verify_fixes: 结果 -> $OUT，日志 -> $LOG_DIR"

for scenario_index in "${!SCENARIO_NAMES[@]}"; do
    scenario="${SCENARIO_NAMES[$scenario_index]}"
    netem_args="${SCENARIO_NETEM[$scenario_index]}"
    ports=()
    for run in $(seq 1 "$RUNS"); do
        ports+=("$(port_for "$scenario_index" "$run")")
    done

    echo "== 场景 $scenario ($netem_args) =="
    netem_apply "$netem_args" "${ports[@]}" || {
        qdisc_clear || true
        die "netem 注入失败: $scenario"
    }
    tc qdisc show dev lo | sed 's/^/  qdisc: /'

    for run in $(seq 1 "$RUNS"); do
        run_one "$scenario" "$run" "$(port_for "$scenario_index" "$run")"
    done

    if ! qdisc_clear; then
        die "场景 $scenario 结束后 qdisc 清理失败"
    fi
    echo "  qdisc 清理校验通过: $(tc qdisc show dev lo | head -n 1)"
done

echo "verify_fixes: 完成，共 $(( ${#SCENARIO_NAMES[@]} * RUNS )) 轮"
