#!/usr/bin/env bash
# 资源事件回放与 `--view history` 的交叉对照。
#
# 用法：
#   bash tests/replay/run_history_vs_replay_demo.sh [build-dir] [cli-bin]
#
# 前置：
#   - build-dir 是 `GO2CPP_DEBUG_DUMP=ON` + `GO2CPP_REPLAY=ON` 构建
#     （默认 build-debug-full，内含 resource_replay_demo）；
#   - cli-bin 是 go2cpp-cli（默认 build-doctor-full/tools/cli/go2cpp-cli）。
#
# 流程：
#   1. 以驻留模式启动 demo（跑完负载后启动 DebugServer 并等 8 秒）；
#   2. 用 CLI 查询 `--view history --resource <id>`；
#   3. compare_history.py 逐条比较 history 事件与 demo 的事件环序列
#      （两者同源：ResourceHistoryView 内部就是 EventRing::Snapshot()）。
set -uo pipefail

BUILD="${1:-build-debug-full}"
CLI="${2:-${CLI:-build-doctor-full/tools/cli/go2cpp-cli}}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT_DIR="${OUT_DIR:-/tmp/resource-history-vs-replay}"
DEMO_BIN="$BUILD/resource_replay_demo"
RECORDING="$OUT_DIR/history.g2cr"
LOG="$OUT_DIR/demo.log"
JSON="$OUT_DIR/history.json"
mkdir -p "$OUT_DIR"
rm -f "$RECORDING" "$LOG" "$JSON"

if [[ ! -x "$DEMO_BIN" ]]; then
  echo "FAIL: 找不到 $DEMO_BIN（先跑 run_resource_replay_demo.sh 编译）" >&2
  exit 2
fi
if [[ ! -x "$CLI" ]]; then
  echo "FAIL: 找不到 CLI: $CLI" >&2
  exit 2
fi

echo "== 启动 demo（驻留 8s）=="
GO2CPP_RESOURCE_DEMO_HOLD=8 "$DEMO_BIN" "$RECORDING" 20 > "$LOG" 2>&1 &
DEMO_PID=$!
cleanup() { kill "$DEMO_PID" 2>/dev/null || true; wait "$DEMO_PID" 2>/dev/null || true; }
trap cleanup EXIT

for _ in $(seq 1 100); do
  grep -q '^ready=true' "$LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^ready=true' "$LOG"; then
  echo "FAIL: demo 未就绪" >&2
  cat "$LOG" >&2
  exit 1
fi
TARGET_PID="$(grep '^pid=' "$LOG" | head -1 | cut -d= -f2)"
echo "target_pid=$TARGET_PID"
cat "$LOG" | head -8

RID="$(grep -m1 '^mutex_resource_id=' "$LOG" | cut -d= -f2)"
if [[ -z "$RID" ]]; then
  echo "FAIL: demo 输出里没有 mutex_resource_id" >&2
  exit 1
fi
echo "== CLI: --view history --resource $RID =="
"$CLI" dump --pid "$TARGET_PID" --view history --resource "$RID" --out "$JSON" || {
  echo "FAIL: CLI 查询 history 失败" >&2
  exit 1
}
wait "$DEMO_PID" 2>/dev/null || true
trap - EXIT
ls -l "$JSON"

echo "== 对照（history vs demo 事件环）=="
python3 tests/replay/compare_history.py "$LOG" "$JSON" 10
rc=$?
echo "RESULT: $([[ $rc -eq 0 ]] && echo PASS || echo FAIL)"
exit "$rc"
