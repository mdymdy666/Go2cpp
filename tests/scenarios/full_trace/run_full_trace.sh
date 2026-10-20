#!/usr/bin/env bash
# 全链路视图验收：启动混合场景跑满 DURATION 秒，抓取 6 个 JSON 视图并做
# park↔wake / enqueue↔claim / timer→G 入队 的覆盖率校验。
#
# 用法：
#   tests/scenarios/full_trace/run_full_trace.sh [DURATION] [HOLD]
# 环境变量：
#   SCENARIO  场景可执行路径（默认 build-debug-full/go2cpp_full_trace_scenario）
#   CLI       go2cpp-cli 路径（默认 build-cli/tools/cli/go2cpp-cli）
#   OUT       输出目录（默认 /tmp/full_trace）
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
DURATION=${1:-10}
HOLD=${2:-8}
SCENARIO=${SCENARIO:-"$ROOT/build-debug-full/go2cpp_full_trace_scenario"}
CLI=${CLI:-"$ROOT/build-cli/tools/cli/go2cpp-cli"}
OUT=${OUT:-/tmp/full_trace}

if [[ ! -x "$SCENARIO" ]]; then
    echo "缺少场景可执行：$SCENARIO" >&2
    exit 2
fi
if [[ ! -x "$CLI" ]]; then
    echo "缺少 CLI：$CLI" >&2
    exit 2
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.json "$OUT"/coverage.txt "$OUT"/scenario.log

"$SCENARIO" "$DURATION" "$HOLD" >"$OUT/scenario.log" 2>&1 &
SCENARIO_PID=$!
cleanup() {
    kill "$SCENARIO_PID" 2>/dev/null || true
    wait "$SCENARIO_PID" 2>/dev/null || true
}
trap cleanup EXIT

wait_for_line() {
    local pattern=$1
    local rounds=${2:-200}
    for ((i = 0; i < rounds; ++i)); do
        if grep -q "$pattern" "$OUT/scenario.log" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

if ! wait_for_line '^READY$' 300; then
    echo "场景未就绪（READY 缺失）：" >&2
    cat "$OUT/scenario.log" >&2
    exit 3
fi
TARGET_PID=$(sed -n 's/^pid=//p' "$OUT/scenario.log" | head -1)
if [[ -z "$TARGET_PID" ]]; then
    echo "场景日志缺少 pid=" >&2
    exit 3
fi
SOCKET=$(sed -n 's/^socket=//p' "$OUT/scenario.log" | head -1)
export GO2CPP_DEBUG_SOCKET="$SOCKET"
echo "target pid=$TARGET_PID  socket=$SOCKET"

# 等业务阶段跑满 DURATION 秒（TICK 行数达标）。
TICK_TARGET=$((DURATION))
for ((i = 0; i < DURATION * 8 + 200; ++i)); do
    ticks=$(grep -c '^TICK ' "$OUT/scenario.log" 2>/dev/null || true)
    if [[ "$ticks" -ge "$TICK_TARGET" ]]; then
        break
    fi
    sleep 0.25
done
ticks=$(grep -c '^TICK ' "$OUT/scenario.log" 2>/dev/null || true)
echo "业务阶段：TICK=$ticks / $TICK_TARGET"

# 业务态稳定窗口（2s）：此时长期 G 都在运行/等待，gmp 与 park 视图有内容。
if ! wait_for_line '^BUSY_WINDOW$' 200; then
    echo "场景未进入 BUSY_WINDOW：" >&2
    tail -40 "$OUT/scenario.log" >&2
    exit 3
fi
for view in gmp sched park timer primitives; do
    "$CLI" dump --pid "$TARGET_PID" --view "$view" \
        --timeout-ms 10000 --out "$OUT/$view.json" >/dev/null
    echo "已抓取 $view.json（业务态）"
done

if ! wait_for_line '^QUIESCENT$' 200; then
    echo "场景未进入 QUIESCENT：" >&2
    tail -40 "$OUT/scenario.log" >&2
    exit 3
fi
if ! wait_for_line '^DEMO_DONE' 200; then
    echo "demo G 未完成：" >&2
    tail -40 "$OUT/scenario.log" >&2
    exit 3
fi
TIMELINE_GID=$(sed -n 's/^timeline_gid=//p' "$OUT/scenario.log" | tail -1)
echo "timeline gid=$TIMELINE_GID"
"$CLI" dump --pid "$TARGET_PID" --view timeline --gid "$TIMELINE_GID" \
    --timeout-ms 10000 --out "$OUT/timeline.json" >/dev/null
echo "已抓取 timeline.json（demo G 完整生命周期）"

python3 "$HERE/verify_coverage.py" "$OUT" | tee "$OUT/coverage.txt"

# 等场景自行退出（HOLD 秒 + 余量），失败不阻塞验收结果。
for ((i = 0; i < HOLD * 10 + 100; ++i)); do
    if ! kill -0 "$SCENARIO_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
echo "视图输出：$OUT/{timeline,gmp,sched,park,timer,primitives}.json"
echo "覆盖率报告：$OUT/coverage.txt"
echo "场景日志：$OUT/scenario.log"
