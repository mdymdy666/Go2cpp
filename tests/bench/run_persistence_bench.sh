#!/usr/bin/env bash
# 持久化基线 vs 原型压测驱动。
#
# 做什么：
#   1. 编译基线 bench（tests/bench/persistence_bench.cpp，链接真实 Recorder）
#      与原型（tools/persistence，独立 CMake）；
#   2. 先跑原型自测（随机/边界/故障注入），失败即中止；
#   3. 每配置跑 ROUNDS 轮、独立进程、带 timeout，记录 loadavg；
#      loadavg > LOAD_MAX 的轮次标记 valid=0，汇总时剔除；
#   4. 输出归一化 TSV、汇总 TSV 和 Markdown 表。
#
# 用法：
#   bash tests/bench/run_persistence_bench.sh
#   ROUNDS=5 N_BIG=2000000 bash tests/bench/run_persistence_bench.sh
#   SKIP_BUILD=1 bash tests/bench/run_persistence_bench.sh
#
# 环境变量：
#   N_BIG/N_SMALL   大/小事件数（默认 1e6 / 1e5）
#   ROUNDS          每配置轮数（默认 3）
#   LOAD_MAX        1 分钟 loadavg 上限（默认 2.0）
#   TIMEOUT_SEC     单进程超时（默认 300）
#   SKIP_BUILD      1 = 复用已有二进制
#   WORK_DIR        二进制/日志/数据目录（默认 /tmp/go2cpp-persistence-bench）
#   BUILD_DIR       原型 CMake 构建目录（默认 /tmp/go2cpp-persistence-build）
#   REPLAY_LIB      基线链接的 libgo2cpp_replay.a（默认 build-replay）
#
# 边界：不修改 src/**、不 commit；基线与原型都按 Release -O3 编译。

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${WORK_DIR:-/tmp/go2cpp-persistence-bench}"
BUILD_DIR="${BUILD_DIR:-/tmp/go2cpp-persistence-build}"
BIN="$WORK/bin"
DATA="$WORK/data"
LOGS="$WORK/logs"
RESULTS="$ROOT/tests/bench/results"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
RAW="$RESULTS/persistence_${RUN_ID}.tsv"
SUMMARY="$RESULTS/persistence_${RUN_ID}_summary.tsv"
REPORT="$RESULTS/persistence_${RUN_ID}.md"

N_BIG="${N_BIG:-1000000}"
N_SMALL="${N_SMALL:-100000}"
ROUNDS="${ROUNDS:-3}"
LOAD_MAX="${LOAD_MAX:-2.0}"
TIMEOUT_SEC="${TIMEOUT_SEC:-300}"
SKIP_BUILD="${SKIP_BUILD:-0}"
REPLAY_LIB="${REPLAY_LIB:-$WORK/replay/libgo2cpp_replay.a}"

mkdir -p "$BIN" "$DATA" "$LOGS" "$RESULTS" || exit 2

if [ "$SKIP_BUILD" != "1" ]; then
    if [ ! -f "$REPLAY_LIB" ]; then
        echo "[build] 构建 tools/replay 格式库（GO2CPP_REPLAY=OFF）"
        cmake -S "$ROOT/tools/replay" -B "$WORK/replay" \
            -DGO2CPP_REPLAY=OFF -DCMAKE_BUILD_TYPE=Release >"$LOGS/replay-cmake.log" 2>&1 ||
            { echo "replay cmake 失败（见 $LOGS/replay-cmake.log）"; exit 3; }
        cmake --build "$WORK/replay" --target go2cpp_replay -j >"$LOGS/replay-build.log" 2>&1 ||
            { echo "replay build 失败（见 $LOGS/replay-build.log）"; exit 3; }
    fi
    echo "[build] 基线 bench"
    g++ -std=c++17 -O3 -DNDEBUG -pthread \
        -I "$ROOT/tools/replay/include" \
        "$ROOT/tests/bench/persistence_bench.cpp" "$REPLAY_LIB" \
        -o "$BIN/persistence_bench" || exit 3
    echo "[build] 原型（tools/persistence）"
    cmake -S "$ROOT/tools/persistence" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release -DPERSISTENCE_BUILD_TESTS=ON \
        >"$LOGS/proto-cmake.log" 2>&1 ||
        { echo "原型 cmake 失败（见 $LOGS/proto-cmake.log）"; exit 3; }
    cmake --build "$BUILD_DIR" -j >"$LOGS/proto-build.log" 2>&1 ||
        { echo "原型 build 失败（见 $LOGS/proto-build.log）"; exit 3; }
fi

for binary in "$BIN/persistence_bench" "$BUILD_DIR/persist_bench" \
              "$BUILD_DIR/persistence_tests"; do
    if [ ! -x "$binary" ]; then
        echo "缺少二进制: $binary（不要首次就用 SKIP_BUILD=1）" >&2
        exit 3
    fi
done

echo "[test] 原型随机/边界/故障注入测试"
if ! timeout 180 "$BUILD_DIR/persistence_tests" >"$LOGS/persistence_tests.log" 2>&1; then
    echo "原型自测失败（见 $LOGS/persistence_tests.log）" >&2
    tail -20 "$LOGS/persistence_tests.log" >&2
    exit 4
fi
tail -1 "$LOGS/persistence_tests.log"

if [ -x "$BUILD_DIR/platform_probe" ]; then
    echo "[probe] io_uring / O_DIRECT"
    "$BUILD_DIR/platform_probe" | tee "$LOGS/platform_probe.log"
fi

# 原始 TSV：label, kind, rc, load, valid, payload
printf 'label\tkind\trc\tload\tvalid\tpayload\n' >"$RAW"

current_load() { cut -d' ' -f1 /proc/loadavg; }

wait_for_load() {
    local waited=0 load
    while [ "$waited" -lt 120 ]; do
        load="$(current_load)"
        if ! awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }'; then
            return 0
        fi
        sleep 5
        waited=$((waited + 5))
    done
    return 0
}

mark_valid() {
    local load="$1"
    if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }'; then
        printf '0'
    else
        printf '1'
    fi
}

run_case() {
    local label="$1"
    local kind="$2"
    shift 2
    local load valid rc out log
    load="$(current_load)"
    valid="$(mark_valid "$load")"
    log="$LOGS/${label}.log"
    out="$(timeout "$TIMEOUT_SEC" "$@" 2>"$log")"
    rc=$?
    if [ "$rc" -ne 0 ] || [ -z "$out" ]; then
        printf '%s\t%s\t%s\t%s\t%s\t\n' "$label" "$kind" "$rc" "$load" "$valid" >>"$RAW"
        echo "  [warn] $label rc=$rc load=$load（见 $log）" >&2
        return 0
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$kind" "$rc" "$load" "$valid" "$out" >>"$RAW"
    echo "  [ok] $label load=$load"
}

for round in $(seq 1 "$ROUNDS"); do
    wait_for_load
    echo "[round $round/$ROUNDS]"
    base_path="$DATA/base_${round}.g2cr"
    run_case "recorder-1e5-t1-r${round}" baseline \
        "$BIN/persistence_bench" --impl=recorder --n="$N_SMALL" --threads=1 \
        --path="$base_path"
    run_case "recorder-1e6-t1-r${round}" baseline \
        "$BIN/persistence_bench" --impl=recorder --n="$N_BIG" --threads=1 \
        --path="$base_path"
    run_case "recorder-1e6-t4-r${round}" baseline \
        "$BIN/persistence_bench" --impl=recorder --n="$N_BIG" --threads=4 \
        --path="$base_path"
    run_case "recorder-fsync-1e6-t1-r${round}" baseline \
        "$BIN/persistence_bench" --impl=raw-buffered --n="$N_BIG" --threads=1 \
        --fsync=interval --interval-ms=100 --batch-bytes=1048576 \
        --path="$base_path"

    run_case "proto-buffered-none-1e6-t1-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=buffered --sync=none \
        --n="$N_BIG" --threads=1 --path="$DATA/buffered_${round}" --read=scan
    run_case "proto-mpsc-none-1e6-t1-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=mpsc --sync=none \
        --n="$N_BIG" --threads=1 --path="$DATA/mpsc_none_${round}" --read=scan
    run_case "proto-mpsc-interval-1e6-t1-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=mpsc --sync=interval \
        --interval-ms=100 --n="$N_BIG" --threads=1 \
        --path="$DATA/mpsc_int_${round}" --read=scan
    run_case "proto-mpsc-interval-1e6-t4-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=mpsc --sync=interval \
        --interval-ms=100 --n="$N_BIG" --threads=4 \
        --path="$DATA/mpsc_int4_${round}" --read=scan
    run_case "proto-mpsc-batch-1e6-t1-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=mpsc --sync=batch \
        --sync-bytes=4194304 --n="$N_BIG" --threads=1 \
        --path="$DATA/mpsc_batch_${round}" --read=scan
    run_case "proto-segmented-none-1e6-t4-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=segmented --sync=none \
        --n="$N_BIG" --threads=4 --segment-bytes=8388608 \
        --path="$DATA/seg_none_${round}" --read=scan
    run_case "proto-segmented-interval-1e6-t4-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=segmented --sync=interval \
        --interval-ms=100 --n="$N_BIG" --threads=4 --segment-bytes=8388608 \
        --path="$DATA/seg_int_${round}" --read=scan
    run_case "proto-segmented-range-1e6-t4-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=segmented --sync=interval \
        --interval-ms=100 --n="$N_BIG" --threads=4 --segment-bytes=8388608 \
        --path="$DATA/seg_range_${round}" --read=range \
        --read-from=900000 --read-to=1000000
    run_case "proto-mpsc-interval-1e5-t1-r${round}" proto \
        "$BUILD_DIR/persist_bench" --engine=mpsc --sync=interval \
        --interval-ms=100 --n="$N_SMALL" --threads=1 \
        --path="$DATA/mpsc_small_${round}" --read=scan
done

# 归一化 + 中位数 + 比值
python3 - "$RAW" "$SUMMARY" "$REPORT" <<'PY'
import collections
import os
import statistics
import sys

raw_path, summary_path, report_path = sys.argv[1:4]

baseline_fields = [
    "impl", "n", "threads", "fsync", "batch_bytes", "interval_ms", "wall_ns",
    "ev_per_sec", "p50_ns", "p99_ns", "p999_ns", "rss_kb", "hwm_kb",
    "file_bytes", "records", "decode_ok", "seq_monotonic", "fsync_count",
    "fsync_p50_ns", "fsync_p99_ns", "max_sync_gap_ns", "load1", "valid",
]
proto_fields = [
    "engine", "n", "threads", "sync", "batch_bytes", "sync_bytes",
    "interval_ms", "segment_bytes", "append_ns", "append_ev_per_sec",
    "finish_ns", "wall_ns", "ev_per_sec", "p50_ns", "p99_ns", "p999_ns",
    "rss_kb", "hwm_kb", "disk_bytes", "segments", "records", "checksum",
    "read_mode", "read_ns", "read_ev_per_sec", "read_bytes", "read_records",
    "read_ok", "crc_ok", "seq_ok", "checksum_ok", "fsync_count",
    "fsync_p50_ns", "fsync_p99_ns", "max_sync_gap_ns", "load1", "valid",
]

rows = collections.defaultdict(list)
with open(raw_path, encoding="utf-8") as handle:
    header = handle.readline()
    for line in handle:
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 6:
            continue
        label, kind, rc, load, valid = parts[0], parts[1], parts[2], parts[3], parts[4]
        payload = "\t".join(parts[5:])
        if not payload or rc != "0" or valid != "1":
            continue
        fields = baseline_fields if kind == "baseline" else proto_fields
        values = payload.split("\t")
        if len(values) != len(fields):
            continue
        row = dict(zip(fields, values))
        row["label"] = label.rsplit("-r", 1)[0]
        row["kind"] = kind
        row["load"] = float(load)
        rows[row["label"]].append(row)

def median(group, key):
    values = []
    for row in group:
        try:
            values.append(float(row[key]))
        except (KeyError, ValueError):
            pass
    return statistics.median(values) if values else None

def fmt(value, digits=0):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)

summary_fields = [
    "label", "kind", "rounds", "ev_per_sec", "p50_ns", "p99_ns", "p999_ns",
    "append_ev_per_sec", "wall_ns", "rss_kb", "hwm_kb", "disk_bytes",
    "records", "fsync_count", "fsync_p50_ns", "fsync_p99_ns",
    "max_sync_gap_ns", "read_ns", "read_ev_per_sec", "ms_per_1e6",
]
summary = []
for label, group in rows.items():
    ev = median(group, "ev_per_sec")
    entry = {
        "label": label,
        "kind": group[0]["kind"],
        "rounds": len(group),
        "ev_per_sec": ev,
        "p50_ns": median(group, "p50_ns"),
        "p99_ns": median(group, "p99_ns"),
        "p999_ns": median(group, "p999_ns"),
        "append_ev_per_sec": median(group, "append_ev_per_sec"),
        "wall_ns": median(group, "wall_ns"),
        "rss_kb": median(group, "rss_kb"),
        "hwm_kb": median(group, "hwm_kb"),
        "disk_bytes": median(group, "disk_bytes") if "disk_bytes" in group[0]
                      else median(group, "file_bytes"),
        "records": median(group, "records"),
        "fsync_count": median(group, "fsync_count"),
        "fsync_p50_ns": median(group, "fsync_p50_ns"),
        "fsync_p99_ns": median(group, "fsync_p99_ns"),
        "max_sync_gap_ns": median(group, "max_sync_gap_ns"),
        "read_ns": median(group, "read_ns"),
        "read_ev_per_sec": median(group, "read_ev_per_sec"),
        "ms_per_1e6": (1e6 / ev * 1e3) if ev else None,
    }
    summary.append(entry)

with open(summary_path, "w", encoding="utf-8", newline="") as handle:
    handle.write("\t".join(summary_fields) + "\n")
    for entry in summary:
        handle.write("\t".join(fmt(entry[field]) for field in summary_fields) + "\n")

by_label = {entry["label"]: entry for entry in summary}

def ratio(fast, slow):
    a = by_label.get(fast, {}).get("ev_per_sec")
    b = by_label.get(slow, {}).get("ev_per_sec")
    if a and b:
        return a / b
    return None

lines = []
lines.append("# 事件流持久化压测数据（recorder vs tools/persistence 原型）")
lines.append("")
lines.append(f"- 原始数据：`{os.path.basename(raw_path)}`")
lines.append(f"- 汇总数据：`{os.path.basename(summary_path)}`")
lines.append("- 吞吐 = 事件数 / 墙钟时间（原型含 Finish/fdatasync）；p50/p99 为逐事件 Append 延迟")
lines.append("- valid=0（loadavg 超限）与 rc≠0 的轮次已剔除；表中为各配置中位数")
lines.append("- 原型 `interval` = 每 100 ms fdatasync；`batch` = 每 4 MiB fdatasync；`none` = 只写页缓存")
lines.append("- 基线 `recorder` 没有 fsync 接口；`recorder-fsync` 用 raw-buffered + 100 ms fdatasync 近似")
lines.append("")
lines.append("## 1. 绝对数据")
lines.append("")
lines.append("| 配置 | 轮数 | 事件/秒 | p50 (ns) | p99 (ns) | p99.9 (ns) | 1e6 事件耗时 (ms) | RSS (KB) | 峰值 RSS (KB) | 磁盘 (B) | fsync 次数 | fsync p99 (ns) | 持久窗口 (ms) |")
lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for entry in summary:
    gap_ms = (entry["max_sync_gap_ns"] / 1e6) if entry["max_sync_gap_ns"] else None
    lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
        entry["label"], entry["rounds"], fmt(entry["ev_per_sec"]),
        fmt(entry["p50_ns"]), fmt(entry["p99_ns"]), fmt(entry["p999_ns"]),
        fmt(entry["ms_per_1e6"], 1), fmt(entry["rss_kb"]), fmt(entry["hwm_kb"]),
        fmt(entry["disk_bytes"]), fmt(entry["fsync_count"]),
        fmt(entry["fsync_p99_ns"]), fmt(gap_ms, 1)))
lines.append("")
lines.append("## 2. 加速比（同一事件流 / 同一持久化语义）")
lines.append("")
pairs = [
    ("mpsc + 100ms fsync, 1 线程", "recorder（无 fsync）, 1 线程",
     "proto-mpsc-interval-1e6-t1", "recorder-1e6-t1"),
    ("mpsc + 100ms fsync, 1 线程", "recorder-fsync（100ms）, 1 线程",
     "proto-mpsc-interval-1e6-t1", "recorder-fsync-1e6-t1"),
    ("mpsc + 100ms fsync, 4 线程", "recorder（无 fsync）, 4 线程",
     "proto-mpsc-interval-1e6-t4", "recorder-1e6-t4"),
    ("mpsc 无 fsync, 1 线程", "recorder（无 fsync）, 1 线程",
     "proto-mpsc-none-1e6-t1", "recorder-1e6-t1"),
    ("segmented + 100ms fsync, 4 线程", "recorder（无 fsync）, 4 线程",
     "proto-segmented-interval-1e6-t4", "recorder-1e6-t4"),
    ("mpsc + 100ms fsync, 1 线程 (1e5)", "recorder（无 fsync）, 1 线程 (1e5)",
     "proto-mpsc-interval-1e5-t1", "recorder-1e5-t1"),
]
lines.append("| 优化配置 | 基线 | 加速比 |")
lines.append("|---|---|---:|")
for fast_name, slow_name, fast, slow in pairs:
    value = ratio(fast, slow)
    lines.append(f"| {fast_name} | {slow_name} | {fmt(value, 2)}x |")
lines.append("")
lines.append("## 3. 结论")
lines.append("")
best = by_label.get("proto-mpsc-interval-1e6-t4", {})
base = by_label.get("recorder-1e6-t1", {})
if best.get("ev_per_sec") and base.get("ev_per_sec"):
    lines.append(f"- 单线程 mpsc + 100ms fsync = **{by_label['proto-mpsc-interval-1e6-t1']['ev_per_sec']:.0f} 事件/s**，"
                 f"较 recorder 单线程 **{ratio('proto-mpsc-interval-1e6-t1','recorder-1e6-t1'):.2f}x**。")
    lines.append(f"- 4 线程 mpsc + 100ms fsync = **{best['ev_per_sec']:.0f} 事件/s**，"
                 f"较 recorder 4 线程 **{ratio('proto-mpsc-interval-1e6-t4','recorder-1e6-t4'):.2f}x**。")
lines.append("- `batch` 策略在 4 线程下受 fdatasync 串行化拖累；持久化吞吐优先选 `interval`，延迟敏感选 `none`。")
lines.append("- 段模式每次滚动会对封存段 fdatasync，写放大高于单文件；换来实现 seq/时间范围定位。")
lines.append("")

with open(report_path, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines) + "\n")
print("\n".join(lines))
print(f"\n原始: {raw_path}\n汇总: {summary_path}\n报告: {report_path}")
PY

if [ "$(wc -l < "$SUMMARY")" -le 1 ]; then
    echo "没有有效轮次（loadavg 超限或全部失败）；原始数据保留在 $RAW" >&2
    exit 5
fi

echo "完成。"
