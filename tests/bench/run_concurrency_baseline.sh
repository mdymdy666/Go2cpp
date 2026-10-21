#!/usr/bin/env bash
# 并发原语「调试监控层」开销基线压测驱动。
#
# 做什么：
#   1. 同一份 tests/bench/concurrency_baseline.cpp 分别链接 OFF / ON 两个
#      构建，产出 conc_OFF 与 conc_ON 两个二进制；
#   2. 对 9 个并发原语场景 + 5 个微基准，跑 5 种 debug 粒度：
#        OFF               —— OFF 构建，宏完全消除
#        SAMPLED           —— park,sched + shift=5（父任务指定的采样配置）
#        SAMPLED_ALL       —— all + shift=5（补充：分离「类别过滤」与「采样」）
#        FULL_NO_CAPTURE   —— 全类别但排除 stacks/chains、shift=0
#        FULL              —— all + shift=0 + stacks/chains 全采样
#   3. 每场景 × 配置跑 ROUNDS 轮，每轮独立进程；loadavg 超限轮次标记
#      valid=0 并在汇总时剔除；
#   4. 汇总中位数、相对 OFF 的吞吐/尾延迟倍数、事件/操作、环形窗口覆盖
#      倍数与 dropped/栈样本指标，输出 TSV 与 Markdown。
#
# 用法：
#   bash tests/bench/run_concurrency_baseline.sh
#   N=100000 LAT_N=10000 ROUNDS=3 bash tests/bench/run_concurrency_baseline.sh
#   SCENARIOS="mutex8 rwmix mpmc" SKIP_BUILD=1 bash tests/bench/run_concurrency_baseline.sh
#   ON_BUILD=/tmp/go2cpp-conc-on SKIP_BUILD=1 bash tests/bench/run_concurrency_baseline.sh
#
# 环境变量：
#   N            每场景操作数（默认 50000）
#   LAT_N        每场景延迟采样上限（默认 5000）
#   ROUNDS       每配置轮数（默认 3）
#   SCENARIOS    场景列表（默认 9 个业务场景 + 5 个微基准）
#   LOAD_MAX     1 分钟 loadavg 上限（默认 2.0；超限轮次 valid=0）
#   TIMEOUT_SEC  单进程超时（默认 600 秒）
#   BUILD_OFF    OFF 构建目录（默认 build-work）
#   BUILD_ON     ON 构建目录（默认 build-debug-conc；不存在时自动配置并构建）
#   SKIP_BUILD   1 = 复用已有二进制
#   WORK_DIR     二进制/日志目录（默认 /tmp/go2cpp-concurrency-baseline）
#
# 输出：
#   tests/bench/results/concurrency_baseline_<ts>.tsv          原始轮次
#   tests/bench/results/concurrency_baseline_<ts>_summary.tsv  中位数 + 倍数
#   tests/bench/results/concurrency_baseline_<ts>.md           数据表
#   /tmp/concurrency_baseline_data.md                          数据表（完整分析见 docs/concurrency-baseline.md）
#
# 边界：
#   - 两个二进制都用 Release（-O3 -DNDEBUG），与库一致；
#   - 事件记录点在头文件里，编译宏必须与所链接构建一致（从 CMakeCache 读取）；
#   - 只新增 tests/bench/**，不改 src/，不 commit；
#   - 「窗口覆盖倍数」= 吞吐轮事件数 /（活跃 ring 数 × 每线程容量）。事件环
#     没有 dropped 计数器，这是历史事件必丢的下界，不是精确丢弃条数。

set -u

# 串行化：同一时间只允许一个压测实例（编译与采集共用一把锁）。
exec 9>/tmp/go2cpp-concurrency-baseline.lock
if ! flock -n 9; then
    echo "另一个 concurrency_baseline 压测正在运行（锁: /tmp/go2cpp-concurrency-baseline.lock）" >&2
    exit 4
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_OFF="${BUILD_OFF:-$ROOT/build-work}"
BUILD_ON="${BUILD_ON:-$ROOT/build-debug-conc}"
SRC="$ROOT/tests/bench/concurrency_baseline.cpp"
RESULTS="$ROOT/tests/bench/results"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
RAW="$RESULTS/concurrency_baseline_${RUN_ID}.tsv"
SUMMARY="$RESULTS/concurrency_baseline_${RUN_ID}_summary.tsv"
REPORT="$RESULTS/concurrency_baseline_${RUN_ID}.md"
WORK="${WORK_DIR:-/tmp/go2cpp-concurrency-baseline}"
BIN="$WORK/bin"
LOGS="$WORK/logs"

N="${N:-50000}"
LAT_N="${LAT_N:-5000}"
ROUNDS="${ROUNDS:-3}"
SCENARIOS="${SCENARIOS:-mutex4 mutex8 rwmix mpmc pc_rtt wg select2 select4 mixed record record_sampled32 lookup stack config}"
LOAD_MAX="${LOAD_MAX:-2.0}"
TIMEOUT_SEC="${TIMEOUT_SEC:-600}"
SKIP_BUILD="${SKIP_BUILD:-0}"

mkdir -p "$BIN" "$LOGS" "$RESULTS" || exit 2

# ---------------------------------------------------------------------------
# 构建
# ---------------------------------------------------------------------------
require_build() {
    local build="$1" label="$2"
    if [ ! -f "$build/CMakeCache.txt" ] || [ -z "$(ls "$build"/libgo2cpp_*.so 2>/dev/null)" ]; then
        echo "缺少 $label 构建: $build" >&2
        echo "先配置：cmake -S \"$ROOT\" -B \"$build\" -DCMAKE_BUILD_TYPE=Release ..." >&2
        return 2
    fi
}

if [ "$SKIP_BUILD" != "1" ] && [ ! -f "$BUILD_ON/CMakeCache.txt" ]; then
    echo "[configure] $BUILD_ON（DEBUG_DUMP=ON, REPLAY=OFF）"
    cmake -S "$ROOT" -B "$BUILD_ON" \
        -DCMAKE_BUILD_TYPE=Release \
        -DGO2CPP_DEBUG_DUMP=ON \
        -DGO2CPP_REPLAY=OFF \
        -DGO2CPP_BUILD_TESTS=OFF \
        -DGO2CPP_BUILD_EXAMPLES=OFF \
        -DGO2CPP_BUILD_BENCHMARKS=OFF || exit 2
    cmake --build "$BUILD_ON" -j"$(nproc)" || exit 2
fi

require_build "$BUILD_OFF" OFF || exit 2
require_build "$BUILD_ON" ON || exit 2

defs_for() {
    local build="$1"
    local cache="$build/CMakeCache.txt"
    local defs=""
    local dump replay cap crit
    if [ ! -f "$cache" ]; then
        return 2
    fi
    dump="$(grep -E '^GO2CPP_DEBUG_DUMP:BOOL=' "$cache" | cut -d= -f2)"
    if [ "$dump" = "ON" ]; then
        cap="$(grep -E '^GO2CPP_DEBUG_EVENT_CAPACITY:' "$cache" | cut -d= -f2)"
        crit="$(grep -E '^GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY:' "$cache" | cut -d= -f2)"
        replay="$(grep -E '^GO2CPP_REPLAY:BOOL=' "$cache" | cut -d= -f2)"
        defs="-DGO2CPP_DEBUG_DUMP=1 -DGO2CPP_DEBUG_EVENT_CAPACITY=${cap:-256} -DGO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY=${crit:-64}"
        if [ "$replay" = "ON" ]; then
            defs="$defs -DGO2CPP_REPLAY=1"
        fi
    fi
    printf '%s' "$defs"
}

libs_for() {
    ls "$1"/libgo2cpp_*.so 2>/dev/null |
        sed 's|.*/lib||;s|\.so$||;s|^|-l|' | tr '\n' ' '
}

compile_variant() {
    local label="$1"
    local build="$2"
    local out="$3"
    local defs libs
    defs="$(defs_for "$build")" || return 2
    libs="$(libs_for "$build")"
    if [ -z "$libs" ]; then
        echo "未找到 $build/libgo2cpp_*.so（先构建）" >&2
        return 2
    fi
    echo "[build] $label <- $build  defs='${defs:-<none>}'"
    # 故意不加引号：defs/libs 需要按空格拆成多个编译参数。
    # shellcheck disable=SC2086
    if ! g++ -std=c++17 -O3 -DNDEBUG -pthread -I "$ROOT/include" \
        $defs "$SRC" -L "$build" $libs -Wl,-rpath,"$build" -o "$out"; then
        echo "编译失败: $label" >&2
        return 3
    fi
    return 0
}

if [ "$SKIP_BUILD" != "1" ]; then
    compile_variant OFF "$BUILD_OFF" "$BIN/conc_OFF" || exit 3
    compile_variant ON "$BUILD_ON" "$BIN/conc_ON" || exit 3
fi

for variant in OFF ON; do
    if [ ! -x "$BIN/conc_$variant" ]; then
        echo "缺少二进制: $BIN/conc_$variant（不要用 SKIP_BUILD=1 首次运行）" >&2
        exit 3
    fi
done

# ---------------------------------------------------------------------------
# 原始 TSV
# ---------------------------------------------------------------------------
printf 'round\tload\tvalid\trc\tconfig\tscenario\tn\tops\tthreads\twall_ns\tops_per_sec\tp50_ns\tp99_ns\tp999_ns\tlat_ops\tevents\tevents_per_op\tevent_detail\twindow_capacity\twindow_coverage\tsnapshot_events\tstack_samples\tstack_dropped\tdropped_threads\trss_base_kb\trss_after_kb\trss_peak_kb\tdebug_build\n' >"$RAW"

run_one() {
    local variant="$1"
    local scenario="$2"
    local round="$3"
    local bin="$BIN/conc_ON"
    if [ "$variant" = "OFF" ]; then
        bin="$BIN/conc_OFF"
    fi
    local log="$LOGS/${variant}_${scenario}_r${round}.log"
    local load valid rc out
    load="$(cut -d' ' -f1 /proc/loadavg)"
    valid=1
    if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }'; then
        valid=0
    fi

    # 显式清除所有 debug 环境变量，再按配置追加。
    local -a env_args=(
        env
        -u GO2CPP_DEBUG_CATEGORIES
        -u GO2CPP_DEBUG_SAMPLE_SHIFT
        -u GO2CPP_DEBUG_STACK_SAMPLE
        -u GO2CPP_DEBUG_FIBER_CHAIN_SAMPLE
    )
    case "$variant" in
        OFF)
            ;;
        SAMPLED)
            env_args+=(
                GO2CPP_DEBUG_CATEGORIES=park,sched
                GO2CPP_DEBUG_SAMPLE_SHIFT=5
            )
            ;;
        SAMPLED_ALL)
            env_args+=(
                GO2CPP_DEBUG_CATEGORIES=all
                GO2CPP_DEBUG_SAMPLE_SHIFT=5
            )
            ;;
        FULL_NO_CAPTURE)
            env_args+=(
                GO2CPP_DEBUG_CATEGORIES=lifecycle,sched,park,primitives,timer,io
                GO2CPP_DEBUG_SAMPLE_SHIFT=0
            )
            ;;
        FULL)
            env_args+=(
                GO2CPP_DEBUG_CATEGORIES=all
                GO2CPP_DEBUG_SAMPLE_SHIFT=0
                GO2CPP_DEBUG_STACK_SAMPLE=1
                GO2CPP_DEBUG_FIBER_CHAIN_SAMPLE=1
            )
            ;;
    esac

    out="$(timeout "$TIMEOUT_SEC" "${env_args[@]}" "$bin" \
        --scenario="$scenario" --n="$N" --lat-n="$LAT_N" \
        --config="$variant" 2>"$log")"
    rc=$?
    if [ "$rc" -ne 0 ] || [ -z "$out" ]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$round" "$load" "$valid" "$rc" "$variant" "$scenario" "-" \
            >>"$RAW"
        echo "  [warn] $variant/$scenario r$round rc=$rc（详见 $log）" >&2
        return 0
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' \
        "$round" "$load" "$valid" "$rc" "$out" >>"$RAW"
    echo "  [ok] $variant/$scenario r$round load=$load $(
        echo "$out" | awk -F'\t' '{printf "%.0f ops/s p50=%.0fns p99=%.0fns ev/op=%.2f",
            $7, $8, $9, $13}'
    )"
}

CONFIGS="OFF SAMPLED SAMPLED_ALL FULL_NO_CAPTURE FULL"
for scenario in $SCENARIOS; do
    echo "[scenario] $scenario (n=$N, lat_n=$LAT_N)"
    for round in $(seq 1 "$ROUNDS"); do
        # 同一轮内五种配置相邻运行，减少系统状态漂移对比较的干扰。
        for config in $CONFIGS; do
            run_one "$config" "$scenario" "$round"
        done
    done
done

# ---------------------------------------------------------------------------
# 汇总：中位数 + 相对 OFF 的倍数（Python 只读 TSV，不参与采集）
# ---------------------------------------------------------------------------
python3 - "$RAW" "$SUMMARY" "$REPORT" "$N" "$ROUNDS" "$LOAD_MAX" \
    "$BUILD_OFF" "$BUILD_ON" <<'PY'
import collections
import os
import statistics
import sys

raw_path, summary_path, report_path = sys.argv[1:4]
n_ops = int(sys.argv[4])
rounds = int(sys.argv[5])
load_max = sys.argv[6]
build_off, build_on = sys.argv[7:9]

rows = []
with open(raw_path, encoding="utf-8") as handle:
    header = handle.readline().rstrip("\n").split("\t")
    for line in handle:
        parts = line.rstrip("\n").split("\t")
        if len(parts) != len(header):
            continue
        row = dict(zip(header, parts))
        if row.get("valid") != "1" or row.get("rc") != "0":
            continue
        try:
            for key in ("ops_per_sec", "p50_ns", "p99_ns", "p999_ns",
                        "wall_ns", "events", "events_per_op",
                        "window_coverage", "snapshot_events",
                        "stack_samples", "stack_dropped", "dropped_threads",
                        "rss_after_kb", "rss_peak_kb", "lat_ops"):
                row[key] = float(row[key])
        except (ValueError, KeyError):
            continue
        rows.append(row)

groups = collections.defaultdict(list)
for row in rows:
    groups[(row["config"], row["scenario"])].append(row)

configs = ["OFF", "SAMPLED", "SAMPLED_ALL", "FULL_NO_CAPTURE", "FULL"]
scenarios = []
for row in rows:
    if row["scenario"] not in scenarios:
        scenarios.append(row["scenario"])


def med(group, key):
    values = [r[key] for r in group if key in r]
    return statistics.median(values) if values else None


summary = []
for scenario in scenarios:
    off = groups.get(("OFF", scenario), [])
    off_ops = med(off, "ops_per_sec") if off else None
    off_p50 = med(off, "p50_ns") if off else None
    off_p99 = med(off, "p99_ns") if off else None
    for config in configs:
        group = groups.get((config, scenario), [])
        entry = {
            "config": config, "scenario": scenario, "rounds": len(group),
            "ops_per_sec": med(group, "ops_per_sec") if group else None,
            "p50_ns": med(group, "p50_ns") if group else None,
            "p99_ns": med(group, "p99_ns") if group else None,
            "p999_ns": med(group, "p999_ns") if group else None,
            "events": med(group, "events") if group else None,
            "events_per_op": med(group, "events_per_op") if group else None,
            "window_coverage": med(group, "window_coverage") if group else None,
            "snapshot_events": med(group, "snapshot_events") if group else None,
            "stack_samples": med(group, "stack_samples") if group else None,
            "stack_dropped": med(group, "stack_dropped") if group else None,
            "dropped_threads": med(group, "dropped_threads") if group else None,
            "rss_after_kb": med(group, "rss_after_kb") if group else None,
            "rss_peak_kb": med(group, "rss_peak_kb") if group else None,
            "event_detail": group[0]["event_detail"] if group else "-",
            "off_ops_per_sec": off_ops,
            "slowdown": None, "p50_ratio": None, "p99_ratio": None,
            "overhead_pct": None,
        }
        if off_ops and entry["ops_per_sec"]:
            entry["slowdown"] = off_ops / entry["ops_per_sec"]
            entry["overhead_pct"] = (
                (off_ops - entry["ops_per_sec"]) / off_ops * 100.0)
        if off_p50 and entry["p50_ns"]:
            entry["p50_ratio"] = entry["p50_ns"] / off_p50
        if off_p99 and entry["p99_ns"]:
            entry["p99_ratio"] = entry["p99_ns"] / off_p99
        summary.append(entry)

summary_fields = [
    "config", "scenario", "rounds", "ops_per_sec", "p50_ns", "p99_ns",
    "p999_ns", "events", "events_per_op", "window_coverage",
    "snapshot_events", "stack_samples", "stack_dropped", "dropped_threads",
    "rss_after_kb", "rss_peak_kb", "off_ops_per_sec", "slowdown",
    "p50_ratio", "p99_ratio", "overhead_pct", "event_detail",
]


def fmt(value, digits=1):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


with open(summary_path, "w", encoding="utf-8", newline="") as handle:
    handle.write("\t".join(summary_fields) + "\n")
    for entry in summary:
        handle.write("\t".join(fmt(entry[field]) if field != "event_detail"
                               else entry[field]
                               for field in summary_fields) + "\n")

by_key = {(e["config"], e["scenario"]): e for e in summary}
micro = {"record", "record_sampled32", "lookup", "stack", "config"}
latency_scenarios = [s for s in scenarios if s not in micro]

lines = []
lines.append("# 并发原语调试监控开销基线")
lines.append("")
lines.append(f"- 原始数据：`{os.path.basename(raw_path)}`")
lines.append(f"- 汇总数据：`{os.path.basename(summary_path)}`")
lines.append(f"- 每场景操作数 N={n_ops}，每配置 {rounds} 轮取中位数，"
             f"延迟采样上限 LAT_N，loadavg 上限 {load_max}")
lines.append(f"- OFF 构建：`{build_off}`；ON 构建：`{build_on}`")
lines.append("- 吞吐 = ops / 墙钟时间；p50/p99 含逐操作 `steady_clock`"
             " 计时成本（约 20–30 ns）")
lines.append("- 事件量 = `EventRing::TotalCountAll()` 累计增量；OFF 构建恒 0")
lines.append("- 窗口覆盖倍数 = 事件数 /（活跃 ring 数 × 每线程 320 槽）；"
             ">1 表示环形窗口在压测期间至少被覆盖一遍，历史事件必有丢失"
             "（事件环没有 dropped 计数器，这是下界）")
lines.append("")

lines.append("## 1. 场景 × 配置（吞吐 / 延迟 / 事件）")
lines.append("")
lines.append("| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | 事件/操作 | 窗口覆盖倍数 |")
lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|")
for scenario in latency_scenarios:
    for config in configs:
        e = by_key.get((config, scenario), {})
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            scenario, config, e.get("rounds", 0),
            fmt(e.get("ops_per_sec"), 0), fmt(e.get("p50_ns")),
            fmt(e.get("p99_ns")), fmt(e.get("p999_ns")),
            fmt(e.get("events_per_op"), 3),
            fmt(e.get("window_coverage"), 2)))

lines.append("")
lines.append("## 2. 相对 OFF 的开销倍数（同场景、中位数）")
lines.append("")
lines.append("| 场景 | 配置 | 慢倍数 | p50 倍数 | p99 倍数 | 事件/操作 |")
lines.append("|---|---|---:|---:|---:|---:|")
for scenario in latency_scenarios:
    for config in configs[1:]:
        e = by_key.get((config, scenario), {})
        lines.append("| {} | {} | {} | {} | {} | {} |".format(
            scenario, config, fmt(e.get("slowdown"), 2),
            fmt(e.get("p50_ratio"), 2), fmt(e.get("p99_ratio"), 2),
            fmt(e.get("events_per_op"), 3)))

lines.append("")
lines.append("## 3. 微基准（单次调用成本）")
lines.append("")
lines.append("| 微基准 | 配置 | ns/调用 | 事件/调用 | 说明 |")
lines.append("|---|---|---:|---:|---|")
notes = {
    "record": "EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写",
    "record_sampled32": "RecordSampled(every=32)：采样判断 + 命中时同上",
    "lookup": "FindResourceByAddress：shared_mutex + 哈希查找",
    "stack": "CaptureStackSample：backtrace + 栈表去重锁",
    "config": "DebugConfig::Enabled：原子掩码读",
}
for scenario in [s for s in scenarios if s in micro]:
    for config in configs:
        e = by_key.get((config, scenario), {})
        ops = e.get("ops_per_sec")
        ns = (1e9 / ops) if ops else None
        lines.append("| {} | {} | {} | {} | {} |".format(
            scenario, config, fmt(ns, 2), fmt(e.get("events_per_op"), 4),
            notes.get(scenario, "")))

lines.append("")
lines.append("## 4. 事件构成（FULL 配置，按类别）")
lines.append("")
lines.append("| 场景 | 事件明细 |")
lines.append("|---|---|")
for scenario in latency_scenarios:
    e = by_key.get(("FULL", scenario), {})
    lines.append("| {} | {} |".format(scenario, e.get("event_detail", "-")))

lines.append("")
lines.append("## 5. 数据完整性与说明")
lines.append("")
valid_counts = collections.Counter()
for row in rows:
    valid_counts[(row["config"], row["scenario"])] += 1
missing = []
for scenario in scenarios:
    for config in configs:
        if valid_counts[(config, scenario)] < rounds:
            missing.append(f"{config}/{scenario}={valid_counts[(config, scenario)]}")
if missing:
    lines.append(f"- 有效轮次不足 {rounds} 的组合：{', '.join(missing)}")
else:
    lines.append(f"- 所有 (配置, 场景) 组合均达到 {rounds} 轮有效样本。")
total_dropped = sum(int(r.get("dropped_threads", 0) or 0) for r in rows)
max_stack_drop = max((int(r.get("stack_dropped", 0) or 0) for r in rows),
                     default=0)
max_cov = max((float(r.get("window_coverage", 0) or 0) for r in rows),
              default=0.0)
lines.append(f"- dropped_threads 合计：{total_dropped}；"
             f"stack_samples dropped 峰值：{max_stack_drop}")
lines.append(f"- 窗口覆盖倍数峰值：{max_cov:.1f}×"
             f"（每线程 320 槽，事件多于此即回绕）")
lines.append("- `SAMPLED` 关闭 primitives/stacks/chains 类别；`SAMPLED_ALL`"
             " 打开全部类别但 shift=5。`GO2CPP_DEBUG_SAMPLE_SHIFT` 只降低"
             " 快路径采样事件与栈捕获频率，**不降低资源事件的记录频率**。")
lines.append("- `FULL_NO_CAPTURE` 关闭 stacks/chains 类别，用于把"
             "「事件编码 + 资源路径」与「backtrace / Fiber 链」拆开归因。")
lines.append("- valid=0（loadavg 超限）与 rc≠0 的轮次已从汇总剔除，原始 TSV 保留。")
lines.append("")

text = "\n".join(lines)
with open(report_path, "w", encoding="utf-8") as handle:
    handle.write(text + "\n")
try:
    with open("/tmp/concurrency_baseline_data.md", "w", encoding="utf-8") as handle:
        handle.write(text + "\n")
except OSError as exc:
    print(f"写 /tmp/concurrency_baseline_data.md 失败: {exc}", file=sys.stderr)

print(text)
print(f"\n原始: {raw_path}\n汇总: {summary_path}\n报告: {report_path}")
PY

echo "完成。"
