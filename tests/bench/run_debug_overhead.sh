#!/usr/bin/env bash
# GO2CPP_DEBUG_DUMP 开/关端到端开销压测驱动。
#
# 做什么：
#   1. 用同一份 tests/bench/debug_overhead.cpp 分别链接 OFF / ON 两个构建，
#      产出三份二进制（ON 的两种运行配置共用同一二进制，用环境变量切换）；
#   2. 每个场景 × 配置跑 ROUNDS 轮，每轮独立进程、带 timeout；
#   3. 汇总中位数，计算 ON/OFF 吞吐与尾延迟比值，输出 TSV 与 Markdown。
#
# 用法：
#   bash tests/bench/run_debug_overhead.sh
#   N=100000 ROUNDS=3 bash tests/bench/run_debug_overhead.sh
#   SCENARIOS="channel mutex" bash tests/bench/run_debug_overhead.sh
#   SKIP_BUILD=1 bash tests/bench/run_debug_overhead.sh
#
# 环境变量：
#   N            每场景操作数（默认 200000，≥10 万）
#   LAT_N        每场景延迟采样上限（默认 100000）
#   ROUNDS       每配置轮数（默认 3）
#   THREADS      mutex 场景线程数（默认 4）
#   SCENARIOS    场景列表（默认全部 6 个）
#   LOAD_MAX     1 分钟 loadavg 上限（默认 2.0；超限轮次标记 valid=0）
#   TIMEOUT_SEC  单进程超时（默认 300 秒）
#   BUILD_OFF    OFF 构建目录（默认 build-work）
#   BUILD_ON     ON 构建目录（默认 build-debug-full）
#   SKIP_BUILD   1 = 复用已有二进制
#   WORK_DIR     二进制/日志目录（默认 /tmp/go2cpp-debug-overhead）
#
# 输出：
#   tests/bench/results/debug_overhead_<ts>.tsv          原始轮次
#   tests/bench/results/debug_overhead_<ts>_summary.tsv  中位数 + 比值
#   tests/bench/results/debug_overhead_<ts>.md           数据表
#
# 边界：
#   - 三份二进制都用 Release（-O3 -DNDEBUG），与库一致；
#   - channel/rwmutex 的事件记录点在头文件里，编译宏必须与所链接的构建一致
#     （脚本从 CMakeCache.txt 读取并传入），否则会测出混合配置；
#   - loadavg > LOAD_MAX 的轮次保留在原始 TSV 里但 valid=0，汇总时剔除；
#   - 不修改 src/，不 commit。

set -u

# 串行化：同一时间只允许一个压测实例（编译与采集共用一把锁）。
# 用 flock 避免并发构建/采集互相污染性能数据。
exec 9>/tmp/go2cpp-debug-overhead.lock
if ! flock -n 9; then
    echo "另一个 debug_overhead 压测正在运行（锁: /tmp/go2cpp-debug-overhead.lock）" >&2
    exit 4
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_OFF="${BUILD_OFF:-$ROOT/build-work}"
BUILD_ON="${BUILD_ON:-$ROOT/build-debug-full}"
SRC="$ROOT/tests/bench/debug_overhead.cpp"
RESULTS="$ROOT/tests/bench/results"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
RAW="$RESULTS/debug_overhead_${RUN_ID}.tsv"
SUMMARY="$RESULTS/debug_overhead_${RUN_ID}_summary.tsv"
REPORT="$RESULTS/debug_overhead_${RUN_ID}.md"
WORK="${WORK_DIR:-/tmp/go2cpp-debug-overhead}"
BIN="$WORK/bin"
LOGS="$WORK/logs"

N="${N:-200000}"
LAT_N="${LAT_N:-100000}"
ROUNDS="${ROUNDS:-3}"
THREADS="${THREADS:-4}"
SCENARIOS="${SCENARIOS:-channel mutex spawn timer resources select}"
LOAD_MAX="${LOAD_MAX:-2.0}"
TIMEOUT_SEC="${TIMEOUT_SEC:-300}"
SKIP_BUILD="${SKIP_BUILD:-0}"

mkdir -p "$BIN" "$LOGS" "$RESULTS" || exit 2

# ---------------------------------------------------------------------------
# 编译宏：必须与所链接构建一致（模板/头文件内的事件点按这些宏实例化）
# ---------------------------------------------------------------------------
defs_for() {
    local build="$1"
    local cache="$build/CMakeCache.txt"
    local defs=""
    local dump replay cap crit
    if [ ! -f "$cache" ]; then
        echo "未找到 $cache" >&2
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
        sed 's|.*/lib||;s|\.so$||' | sed 's/^/-l/' | tr '\n' ' '
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
    compile_variant OFF "$BUILD_OFF" "$BIN/debug_overhead_OFF" || exit 3
    compile_variant ON_FULL "$BUILD_ON" "$BIN/debug_overhead_ON_FULL" || exit 3
    # 采样配置与 ON_FULL 共用同一份二进制：类别/采样位移只是运行时环境变量。
    cp -f "$BIN/debug_overhead_ON_FULL" "$BIN/debug_overhead_ON_SAMPLED" || exit 3
fi

for variant in OFF ON_FULL ON_SAMPLED; do
    if [ ! -x "$BIN/debug_overhead_$variant" ]; then
        echo "缺少二进制: $BIN/debug_overhead_$variant（不要用 SKIP_BUILD=1 首次运行）" >&2
        exit 3
    fi
done

# ---------------------------------------------------------------------------
# 原始 TSV
# ---------------------------------------------------------------------------
printf 'round\tload\tvalid\trc\tconfig\tscenario\tn\tops\twall_ns\tops_per_sec\tp50_ns\tp99_ns\tp999_ns\trss_base_kb\trss_after_kb\trss_peak_kb\tevents\tevents_per_op\tlat_ops\tthreads\tdebug_build\tevent_detail\n' >"$RAW"

run_one() {
    local variant="$1"
    local config="$2"
    local scenario="$3"
    local round="$4"
    local bin="$BIN/debug_overhead_$variant"
    local log="$LOGS/${variant}_${scenario}_r${round}.log"
    local load valid rc out
    load="$(cut -d' ' -f1 /proc/loadavg)"
    valid=1
    if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }'; then
        valid=0
    fi

    # 三种配置的环境变量；OFF 显式清除，避免外部环境污染。
    local -a env_args=(
        env
        -u GO2CPP_DEBUG_CATEGORIES
        -u GO2CPP_DEBUG_SAMPLE_SHIFT
        -u GO2CPP_DEBUG_STACK_SAMPLE
        -u GO2CPP_DEBUG_FIBER_CHAIN_SAMPLE
    )
    case "$variant" in
        ON_FULL)
            env_args+=(
                GO2CPP_DEBUG_CATEGORIES=all
                GO2CPP_DEBUG_SAMPLE_SHIFT=0
            )
            ;;
        ON_SAMPLED)
            env_args+=(
                GO2CPP_DEBUG_CATEGORIES=sched,park
                GO2CPP_DEBUG_SAMPLE_SHIFT=5
            )
            ;;
    esac

    out="$(timeout "$TIMEOUT_SEC" "${env_args[@]}" "$bin" \
        --scenario="$scenario" --n="$N" --lat-n="$LAT_N" \
        --threads="$THREADS" --config="$config" 2>"$log")"
    rc=$?
    if [ "$rc" -ne 0 ] || [ -z "$out" ]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$round" "$load" "$valid" "$rc" "$config" "$scenario" "-" \
            >>"$RAW"
        echo "  [warn] $config/$scenario r$round rc=$rc（详见 $log）" >&2
        return 0
    fi
    # 程序输出 18 列，前面补 round/load/valid/rc。
    printf '%s\t%s\t%s\t%s\t%s\n' \
        "$round" "$load" "$valid" "$rc" "$out" >>"$RAW"
    echo "  [ok] $config/$scenario r$round load=$load $(
        echo "$out" | awk -F'\t' '{printf "%.0f ops/s p50=%.0fns p99=%.0fns", $6, $7, $8}'
    )"
}

for scenario in $SCENARIOS; do
    echo "[scenario] $scenario (n=$N, lat_n=$LAT_N)"
    for round in $(seq 1 "$ROUNDS"); do
        # 同一轮内三配置相邻运行，减少系统状态漂移对比较的干扰。
        run_one OFF OFF "$scenario" "$round"
        run_one ON_FULL ON_FULL "$scenario" "$round"
        run_one ON_SAMPLED ON_SAMPLED "$scenario" "$round"
    done
done

# ---------------------------------------------------------------------------
# 汇总：中位数 + ON/OFF 比值（Python 只读 TSV，不参与采集）
# ---------------------------------------------------------------------------
python3 - "$RAW" "$SUMMARY" "$REPORT" "$N" "$ROUNDS" "$THREADS" <<'PY'
import collections
import os
import statistics
import sys

raw_path, summary_path, report_path = sys.argv[1:4]
n_ops, rounds, threads = (int(value) for value in sys.argv[4:7])

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
                        "wall_ns", "events"):
                row[key] = float(row[key])
            for key in ("rss_after_kb", "rss_peak_kb"):
                row[key] = int(row[key])
        except (ValueError, KeyError):
            continue
        rows.append(row)

groups = collections.defaultdict(list)
for row in rows:
    groups[(row["config"], row["scenario"])].append(row)

configs = ["OFF", "ON_FULL", "ON_SAMPLED"]
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
        if not group:
            summary.append({
                "config": config, "scenario": scenario, "rounds": 0,
                "ops_per_sec": None, "p50_ns": None, "p99_ns": None,
                "p999_ns": None, "rss_after_kb": None,
                "rss_peak_kb": None, "events": None,
                "events_per_op": None, "off_ops_per_sec": off_ops,
                "overhead_pct": None, "slowdown": None,
                "p50_ratio": None, "p99_ratio": None,
            })
            continue
        ops = med(group, "ops_per_sec")
        p50 = med(group, "p50_ns")
        p99 = med(group, "p99_ns")
        p999 = med(group, "p999_ns")
        rss = med(group, "rss_after_kb")
        peak = med(group, "rss_peak_kb")
        events = med(group, "events")
        entry = {
            "config": config, "scenario": scenario, "rounds": len(group),
            "ops_per_sec": ops, "p50_ns": p50, "p99_ns": p99,
            "p999_ns": p999, "rss_after_kb": rss, "rss_peak_kb": peak,
            "events": events,
            "events_per_op": (events / n_ops) if events is not None else None,
            "off_ops_per_sec": off_ops,
            "overhead_pct": None, "slowdown": None,
            "p50_ratio": None, "p99_ratio": None,
        }
        if off_ops and ops:
            entry["overhead_pct"] = (off_ops - ops) / off_ops * 100.0
            entry["slowdown"] = off_ops / ops
        if off_p50 and p50:
            entry["p50_ratio"] = p50 / off_p50
        if off_p99 and p99:
            entry["p99_ratio"] = p99 / off_p99
        summary.append(entry)

summary_fields = [
    "config", "scenario", "rounds", "ops_per_sec", "p50_ns", "p99_ns",
    "p999_ns", "rss_after_kb", "rss_peak_kb", "events", "events_per_op",
    "off_ops_per_sec", "overhead_pct", "slowdown", "p50_ratio", "p99_ratio",
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
        handle.write("\t".join(fmt(entry[field]) for field in summary_fields) + "\n")

lines = []
lines.append("# GO2CPP_DEBUG_DUMP 开销压测数据")
lines.append("")
lines.append(f"- 原始数据：`{os.path.basename(raw_path)}`")
lines.append(f"- 每场景操作数 N={n_ops}，每配置 {rounds} 轮取中位数，mutex 线程数 {threads}")
lines.append("- 吞吐 = ops / 墙钟时间；p50/p99 含逐操作 `steady_clock` 计时成本（约 20–30 ns）")
lines.append("- 事件量 = `EventRing::TotalCountAll()` 累计计数增量；OFF 构建恒为 0")
lines.append("- `overhead_pct > 0` 表示 ON 比 OFF 慢；负值表示测量噪声内更快")
lines.append("")
lines.append("## 1. 场景 × 配置")
lines.append("")
lines.append("| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | RSS (KB) | 峰值 RSS (KB) | 事件量 | 事件/操作 |")
lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for entry in summary:
    lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
        entry["scenario"], entry["config"], entry["rounds"],
        fmt(entry["ops_per_sec"], 0), fmt(entry["p50_ns"]),
        fmt(entry["p99_ns"]), fmt(entry["p999_ns"]),
        fmt(entry["rss_after_kb"], 0), fmt(entry["rss_peak_kb"], 0),
        fmt(entry["events"], 0), fmt(entry["events_per_op"], 3)))
lines.append("")
lines.append("## 2. ON/OFF 比值（同场景、同轮次中位数）")
lines.append("")
lines.append("| 场景 | OFF 吞吐 | ON_FULL 吞吐 | 全开开销 | 全开慢倍数 | p50 倍数 | p99 倍数 | 采样配置开销 | 采样 p99 倍数 |")
lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
by_key = {(e["config"], e["scenario"]): e for e in summary}
for scenario in scenarios:
    off = by_key.get(("OFF", scenario), {})
    full = by_key.get(("ON_FULL", scenario), {})
    sampled = by_key.get(("ON_SAMPLED", scenario), {})
    lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
        scenario,
        fmt(off.get("ops_per_sec"), 0),
        fmt(full.get("ops_per_sec"), 0),
        (fmt(full.get("overhead_pct")) + "%") if full.get("overhead_pct") is not None else "-",
        fmt(full.get("slowdown"), 2),
        fmt(full.get("p50_ratio"), 2),
        fmt(full.get("p99_ratio"), 2),
        (fmt(sampled.get("overhead_pct")) + "%") if sampled.get("overhead_pct") is not None else "-",
        fmt(sampled.get("p99_ratio"), 2)))
lines.append("")
lines.append("## 3. 备注")
lines.append("")
missing = [s for s in scenarios if ("OFF", s) not in by_key]
if missing:
    lines.append(f"- 缺失 OFF 基准的场景：{', '.join(missing)}")
verified = sum(1 for e in summary if e["rounds"] >= 3)
lines.append(f"- 达到 3 轮有效样本的 (配置, 场景) 组合：{verified}/{len(summary)}")
lines.append("- valid=0（loadavg 超限）与 rc≠0 的轮次已从汇总剔除，原始 TSV 保留。")
lines.append("")

with open(report_path, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines) + "\n")

print("\n".join(lines))
print(f"\n原始: {raw_path}\n汇总: {summary_path}\n报告: {report_path}")
PY

echo "完成。"
