#!/usr/bin/env bash
# 资源事件并入回放流（G2CRPLY v4）验收脚本。
#
# 用法：
#   bash tests/replay/run_resource_replay_demo.sh [build-dir] [replay-bin]
#
# 前置：
#   - build-dir 必须是 `GO2CPP_DEBUG_DUMP=ON` **且** `GO2CPP_REPLAY=ON`
#     （默认 build-debug-full；两者同时打开才能做「事件环 vs 回放」对照）；
#   - replay-bin 是 tools/replay 构建出的 `replay`（默认 /tmp/go2cpp-replay-work/replay）。
#
# 步骤：
#   1. 编译 tests/replay/resource_replay_demo.cpp（链接 build 的 .so +
#      libgo2cpp_replay_core.a）；
#   2. 运行 demo：两个 Fiber 争锁 → 录制 → ReplayFile 回放 → 序列/计数对比；
#   3. 断言 sequence_match=OK、counts_match=OK、replay_locks>0、lock_hold_ns>0；
#   4. 用 `replay step` 逐条打印资源事件（until 命令），验证 v4 事件可读。
set -uo pipefail

BUILD="${1:-build-debug-full}"
REPLAY_BIN="${2:-${REPLAY_BIN:-/tmp/go2cpp-replay-work/replay}}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-g++}"
OUT_DIR="${OUT_DIR:-/tmp/resource-replay-demo}"
DEMO_BIN="$BUILD/resource_replay_demo"
RECORDING="$OUT_DIR/two-fiber-mutex.g2cr"
mkdir -p "$OUT_DIR"

if [[ ! -f "$BUILD/libgo2cpp_debug.so" || ! -f "$BUILD/libgo2cpp_debug_event.so" ]]; then
  echo "FAIL: $BUILD 不是 GO2CPP_DEBUG_DUMP=ON 构建" >&2
  exit 2
fi
if [[ ! -f "$BUILD/libgo2cpp_replay_core.a" ]]; then
  echo "FAIL: 缺少 $BUILD/libgo2cpp_replay_core.a（GO2CPP_BUILD_TESTS=ON）" >&2
  exit 2
fi

echo "== 编译 resource_replay_demo =="
# shellcheck disable=SC2046
"$CXX" -std=c++17 -O2 -g -rdynamic -DGO2CPP_DEBUG_DUMP=1 -DGO2CPP_REPLAY=1 \
  -I include -I third_party -I tools/replay/include \
  -Wl,-rpath,"$ROOT/$BUILD" \
  tests/replay/resource_replay_demo.cpp -o "$DEMO_BIN" \
  "$BUILD/libgo2cpp_replay_core.a" \
  $(ls "$BUILD"/libgo2cpp_*.so | grep -v '\.so\.') \
  -lpthread -ldl || { echo "FAIL: demo 编译失败" >&2; exit 2; }

echo "== 运行 demo（录制 + 回放 + 对比）=="
"$DEMO_BIN" "$RECORDING" 80 > "$OUT_DIR/demo.txt" 2>&1
demo_rc=$?
cat "$OUT_DIR/demo.txt"
if [[ "$demo_rc" -ne 0 ]]; then
  echo "FAIL: demo 返回 $demo_rc（序列或计数不一致）" >&2
  exit 1
fi

fail=0
check_has() {
  local name="$1" file="$2" needle="$3"
  if grep -Fq "$needle" "$file"; then
    echo "PASS: $name 包含 $needle"
  else
    echo "FAIL: $name 缺少 $needle"
    fail=1
  fi
}

# 一行里可能有多个 key=value：先按空格/等号拆成 token 再匹配。
check_num_positive() {
  local file="$1" key="$2" value
  value="$(tr ' ' '\n' < "$file" | sed -n "s/^$key=\([0-9][0-9]*\)$/\1/p" | tail -1)"
  if [[ -n "$value" && "$value" -gt 0 ]]; then
    echo "PASS: $key=$value (>0)"
  else
    echo "FAIL: $key=$value (expected >0)"
    fail=1
  fi
}

echo "== 断言：录制 vs 回放 =="
check_has "demo" "$OUT_DIR/demo.txt" "replay_status=ok"
check_has "demo" "$OUT_DIR/demo.txt" "sequence_match=OK"
check_has "demo" "$OUT_DIR/demo.txt" "counts_match=OK"
check_num_positive "$OUT_DIR/demo.txt" "ring_mutex_events"
check_num_positive "$OUT_DIR/demo.txt" "replay_locks"
check_num_positive "$OUT_DIR/demo.txt" "replay_unlocks"
check_num_positive "$OUT_DIR/demo.txt" "lock_hold_ns"
check_has "demo" "$OUT_DIR/demo.txt" "recorded[0]"
check_has "demo" "$OUT_DIR/demo.txt" "replayed[0]"

if [[ -x "$REPLAY_BIN" ]]; then
  echo "== replay step：资源事件逐条输出（until 30）=="
  printf 'until 30\nwhere\nquit\n' | "$REPLAY_BIN" step "$RECORDING" \
    > "$OUT_DIR/step.txt" 2>&1
  cat "$OUT_DIR/step.txt" | head -40
  check_has "step" "$OUT_DIR/step.txt" "type=res-mutex"
  check_has "step" "$OUT_DIR/step.txt" "until 30:"
else
  echo "SKIP: 找不到 replay 可执行文件：$REPLAY_BIN"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: PASS"
