#!/usr/bin/env bash
# REQ-004 有界队列测试：独立编译 + 运行（不注册 ctest，不改现有 CMake）。
#
# 用法：
#   tests/concurrency/run_bounded_queue_test.sh [--items N]
#
# 环境变量：
#   BUILD_DIR  构建目录（默认 <root>/.build-bento-wsl）
#   OUT_DIR    输出目录（默认 /tmp/go2cpp_bounded_queue）
#   CXX        编译器（默认 g++）
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/.build-bento-wsl}
OUT=${OUT_DIR:-/tmp/go2cpp_bounded_queue}
CXX=${CXX:-g++}
SRC="$ROOT/tests/concurrency/test_bounded_queue.cpp"
BIN="$OUT/test_bounded_queue"
LOG="$OUT/test_bounded_queue.log"

mkdir -p "$OUT" || exit 2

if [[ ! -d "$BUILD" ]]; then
  echo "构建目录不存在：$BUILD" >&2
  exit 2
fi

LIBS=(
  -Wl,--no-as-needed
  -Wl,-rpath,"$BUILD"
  -L"$BUILD"
  -lgo2cpp_control_flow -lgo2cpp_channel -lgo2cpp_future -lgo2cpp_network
  -lgo2cpp_sync -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error
  -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_debug
  -lgo2cpp_debug_event -lgo2cpp_hash -lgo2cpp_math -lgo2cpp_rand
  -lgo2cpp_path -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding
  -lgo2cpp_metrics -lz -ldl -lm
)

echo "[build] $SRC"
"$CXX" -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic \
  -I"$ROOT/include" -I"$ROOT/tests" -pthread "$SRC" -o "$BIN" "${LIBS[@]}" \
  || exit 3

echo "[run] $BIN $*"
"$BIN" "$@" 2>&1 | tee "$LOG"
exit "${PIPESTATUS[0]}"
