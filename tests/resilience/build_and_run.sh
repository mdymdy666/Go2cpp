#!/usr/bin/env bash
# 韧性组件测试：编译 + 运行（header-only，不链接 go2cpp 运行时，不改主 CMake）。
#
# 用法：
#   tests/resilience/build_and_run.sh
#
# 环境变量：
#   OUT_DIR   输出目录（默认 /tmp/go2cpp_resilience）
#   CXX       编译器（默认 g++）
#   SANITIZE  传 "thread" 或 "address" 时启用对应 sanitizer（默认不启用）
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT_DIR:-/tmp/go2cpp_resilience}
CXX=${CXX:-g++}
SRC="$ROOT/tests/resilience/test_resilience.cpp"
BIN="$OUT/test_resilience"
SANITIZE=${SANITIZE:-}

mkdir -p "$OUT" || exit 2

EXTRA=()
if [[ -n "$SANITIZE" ]]; then
  EXTRA+=("-fsanitize=$SANITIZE" "-fno-omit-frame-pointer" "-g")
fi

echo "[build] $SRC"
"$CXX" -std=c++17 -O2 -DNDEBUG \
  -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" -pthread \
  "${EXTRA[@]}" \
  "$SRC" -o "$BIN" || exit 3

echo "[run] $BIN"
exec "$BIN"
