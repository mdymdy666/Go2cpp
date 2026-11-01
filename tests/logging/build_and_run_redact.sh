#!/usr/bin/env bash
# 日志脱敏测试（JinShan MOD-01）：编译 + 运行（不修改主 CMake 文件）。
#
# 用法：
#   tests/logging/build_and_run_redact.sh
#
# 环境变量：
#   BUILD_DIR  构建目录（默认 <root>/.build-bento-wsl）
#   OUT_DIR    输出目录（默认 /tmp/go2cpp_log_redact）
#   CXX        编译器（默认 g++）
#
# 依赖主构建产物：libgo2cpp_log.so（日志）与 libgo2cpp_hash.so（对拍 SHA-256）。
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/.build-bento-wsl}
OUT=${OUT_DIR:-/tmp/go2cpp_log_redact}
CXX=${CXX:-g++}
SRC="$ROOT/tests/logging/test_log_redact.cpp"
BIN="$OUT/test_log_redact"

mkdir -p "$OUT" || exit 2

if [[ ! -d "$BUILD" ]]; then
  echo "构建目录不存在：$BUILD" >&2
  echo "先构建（Release）：cmake -S \"$ROOT\" -B \"$BUILD\" -DCMAKE_BUILD_TYPE=Release" >&2
  exit 2
fi
if [[ ! -f "$BUILD/libgo2cpp_log.so" || ! -f "$BUILD/libgo2cpp_hash.so" ]]; then
  echo "缺少 $BUILD/libgo2cpp_log.so 或 libgo2cpp_hash.so" >&2
  exit 2
fi

echo "[build] $SRC"
"$CXX" -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" -pthread "$SRC" -o "$BIN" \
  -Wl,--no-as-needed -Wl,-rpath,"$BUILD" -L"$BUILD" \
  -lgo2cpp_log -lgo2cpp_hash || exit 3

echo "[run] $BIN $*"
exec "$BIN" "$@"
