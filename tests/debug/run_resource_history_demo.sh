#!/usr/bin/env bash
# 资源级历史 + 栈捕获 + Fiber 链 demo/验收脚本。
#
# 用法：
#   bash tests/debug/run_resource_history_demo.sh [build-dir] [seconds]
#
# 前置：build-dir 必须是 GO2CPP_DEBUG_DUMP=ON 的构建（默认 build-debug-full）；
#       CLI 默认取 build-doctor-full/tools/cli/go2cpp-cli，可用 CLI=... 覆盖。
#
# 步骤：
#   1. 用 g++ 把 tests/debug/resource_history_demo.cpp 链接到 build-dir 的 .so，
#      -rdynamic 保证 dladdr 能解析 demo 自身的符号；
#   2. 启动 demo（DebugServer 默认路径 /tmp/go2cpp-<pid>.sock）；
#   3. 依次查询 resources / locks / channels / history 视图；
#   4. 校验 JSON 关键字段并打印摘要。
set -uo pipefail

BUILD="${1:-build-debug-full}"
SECONDS_RUN="${2:-25}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-g++}"
CLI="${CLI:-build-doctor-full/tools/cli/go2cpp-cli}"
OUT_DIR="${OUT_DIR:-/tmp/resource-history-demo}"
DEMO_BIN="$BUILD/resource_history_demo"
DEMO_LOG="$OUT_DIR/demo.log"
mkdir -p "$OUT_DIR"

if [[ ! -f "$BUILD/libgo2cpp_debug.so" ]]; then
  echo "FAIL: $BUILD 不是 GO2CPP_DEBUG_DUMP=ON 构建" >&2
  exit 2
fi
if [[ ! -x "$CLI" ]]; then
  echo "FAIL: 找不到 CLI: $CLI（用 CLI=... 指定）" >&2
  exit 2
fi

echo "== 编译 demo =="
# shellcheck disable=SC2046
"$CXX" -std=c++17 -O2 -g -rdynamic -DGO2CPP_DEBUG_DUMP=1 \
  -I include -I third_party \
  -Wl,-rpath,"$ROOT/$BUILD" \
  tests/debug/resource_history_demo.cpp -o "$DEMO_BIN" \
  $(ls "$BUILD"/libgo2cpp_*.so | grep -v '\.so\.') \
  -lpthread -ldl || { echo "FAIL: demo 编译失败" >&2; exit 2; }

echo "== 启动 demo（${SECONDS_RUN}s）=="
"$DEMO_BIN" "" "$SECONDS_RUN" > "$DEMO_LOG" 2>&1 &
PID=$!
cleanup() { kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; }
trap cleanup EXIT

for _ in $(seq 1 100); do
  grep -q '^ready=true' "$DEMO_LOG" && break
  sleep 0.1
done
if ! grep -q '^ready=true' "$DEMO_LOG"; then
  echo "FAIL: demo 未就绪" >&2
  cat "$DEMO_LOG" >&2
  exit 1
fi
cat "$DEMO_LOG"

resource_id() {
  grep "^resource $1=" "$DEMO_LOG" | cut -d= -f2 | head -1
}
MUTEX_ID="$(resource_id mutex)"
RW_ID="$(resource_id rwmutex)"
REC_ID="$(resource_id recursive)"
PC_ID="$(resource_id parking_condition)"
CH_ID="$(resource_id channel)"

fail=0
check_json() {
  local name="$1" file="$2" needle="$3"
  if grep -q "$needle" "$file"; then
    echo "PASS: $name 包含 $needle"
  else
    echo "FAIL: $name 缺少 $needle（$file）"
    fail=1
  fi
}

echo "== 查询视图 =="
"$CLI" dump --pid "$PID" --view resources > "$OUT_DIR/resources.json" 2>"$OUT_DIR/resources.err"
echo "resources exit=$?"
"$CLI" dump --pid "$PID" --view locks > "$OUT_DIR/locks.json" 2>"$OUT_DIR/locks.err"
echo "locks exit=$?"
"$CLI" dump --pid "$PID" --view channels > "$OUT_DIR/channels.json" 2>"$OUT_DIR/channels.err"
echo "channels exit=$?"
"$CLI" dump --pid "$PID" --view history --resource "$MUTEX_ID" \
  > "$OUT_DIR/history_mutex.json" 2>"$OUT_DIR/history_mutex.err"
echo "history mutex exit=$?"
"$CLI" dump --pid "$PID" --view history --resource "$PC_ID" \
  > "$OUT_DIR/history_pc.json" 2>"$OUT_DIR/history_pc.err"
echo "history pc exit=$?"
"$CLI" dump --pid "$PID" --view history --resource "$CH_ID" \
  > "$OUT_DIR/history_channel.json" 2>"$OUT_DIR/history_channel.err"
echo "history channel exit=$?"
"$CLI" dump --pid "$PID" --view history --resource 0 \
  > "$OUT_DIR/history_park.json" 2>"$OUT_DIR/history_park.err"
echo "history park exit=$?"

check_json "resources" "$OUT_DIR/resources.json" '"view": "resources"'
check_json "resources-coverage" "$OUT_DIR/resources.json" 'sync::SpinMutex'
check_json "locks" "$OUT_DIR/locks.json" '"view": "locks"'
check_json "locks-mutex-$MUTEX_ID" "$OUT_DIR/locks.json" "\"id\": $MUTEX_ID"
check_json "channels" "$OUT_DIR/channels.json" '"view": "channels"'
check_json "channels-id-$CH_ID" "$OUT_DIR/channels.json" "\"id\": $CH_ID"
check_json "history-mutex" "$OUT_DIR/history_mutex.json" "\"resource_id\": $MUTEX_ID"
check_json "history-mutex-stack" "$OUT_DIR/history_mutex.json" '"stack"'
check_json "history-mutex-chain" "$OUT_DIR/history_mutex.json" '"fiber_chain"'
check_json "history-pc" "$OUT_DIR/history_pc.json" "\"resource_id\": $PC_ID"
check_json "history-channel" "$OUT_DIR/history_channel.json" "\"resource_id\": $CH_ID"
check_json "history-park" "$OUT_DIR/history_park.json" '"kParkCaptured"'

echo "== 关键计数（python 解析）=="
python3 - "$OUT_DIR" "$MUTEX_ID" "$CH_ID" "$PC_ID" <<'PY'
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
mutex_id, channel_id, pc_id = (int(x) for x in sys.argv[2:5])


def load(name):
    with (out / name).open(encoding="utf-8") as handle:
        return json.load(handle)


resources = load("resources.json")
locks = load("locks.json")
channels = load("channels.json")
history = load("history_mutex.json")
pc_history = load("history_pc.json")

print("resources.count =", resources["resources"]["count"])
print("resources.by_kind =", json.dumps(resources["resources"]["by_kind"], ensure_ascii=False))
for item in resources["resources"]["resources"]:
    print("  resource", item["id"], item["kind"], item["name"], "alive=", item["alive"])

mutex_stats = None
for item in locks["locks"]["locks"]:
    if item["id"] == mutex_id:
        mutex_stats = item
print("mutex.holds =", mutex_stats.get("holds"), "waits =", mutex_stats.get("waits"),
      "wakes =", mutex_stats.get("wakes"), "max_waiters =", mutex_stats.get("max_waiters"),
      "hold_ns_max =", mutex_stats.get("hold_ns_max"),
      "wait_ns_max =", mutex_stats.get("wait_ns_max"))

chan_stats = None
for item in channels["channels"]["channels"]:
    if item["id"] == channel_id:
        chan_stats = item
print("channel.sends =", chan_stats.get("sends"), "recvs =", chan_stats.get("recvs"),
      "blocked =", chan_stats.get("send_blocked"), "/", chan_stats.get("recv_blocked"),
      "wakes =", chan_stats.get("wakes"))

print("history.mutex.events =", history["history"]["count"])
print("history.mutex.summary =", json.dumps(history["history"]["summary"], ensure_ascii=False))
print("history.pc.events =", pc_history["history"]["count"])
stack_events = [e for e in history["history"]["events"] if "stack" in e]
chain_events = [e for e in history["history"]["events"] if "fiber_chain" in e]
print("history.mutex.with_stack =", len(stack_events), "with_chain =", len(chain_events))
if stack_events:
    frames = stack_events[0]["stack"]["frames"]
    print("  first stack frames =", json.dumps(frames[:4], ensure_ascii=False))
if chain_events:
    nodes = chain_events[0]["fiber_chain"]["nodes"]
    print("  first chain nodes =", json.dumps(nodes, ensure_ascii=False))
PY

echo "== 结果 =="
if [[ "$fail" -ne 0 ]]; then
  echo "FAIL: 有检查项失败"
  exit 1
fi
echo "ALL PASS"
