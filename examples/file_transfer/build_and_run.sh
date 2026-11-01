#!/usr/bin/env bash
# 文件传输示例（JinShan MOD-28）：编译 + 运行（不修改主 CMake 文件）。
#
# 用法：
#   examples/file_transfer/build_and_run.sh [--suite] [--transport tcp] [--size 1G]
#
# 环境变量：
#   BUILD_DIR  主构建目录（默认 <root>/.build-bento-wsl）
#   OUT_DIR    示例输出目录（默认 /tmp/go2cpp_file_transfer_build）
#   CXX        编译器（默认 g++）
#
# 依赖主构建产物：libgo2cpp_app.a（资源传输）+ libgo2cpp_hash.so（SHA-256/CRC-32）。
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/.build-bento-wsl}
OUT=${OUT_DIR:-/tmp/go2cpp_file_transfer_build}
CXX=${CXX:-g++}
SRC="$ROOT/examples/file_transfer/file_transfer_demo.cpp"
BIN="$OUT/file_transfer_demo"

mkdir -p "$OUT" || exit 2

if [[ ! -d "$BUILD" ]]; then
  echo "构建目录不存在：$BUILD" >&2
  echo "先构建（Release）：cmake -S \"$ROOT\" -B \"$BUILD\" -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_APP=ON" >&2
  exit 2
fi
if [[ ! -f "$BUILD/libgo2cpp_app.a" || ! -f "$BUILD/libgo2cpp_hash.so" ]]; then
  echo "缺少 $BUILD/libgo2cpp_app.a 或 libgo2cpp_hash.so" >&2
  echo "先构建主工程（-DGO2CPP_WITH_APP=ON）" >&2
  exit 2
fi

echo "[build] $SRC"
"$CXX" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" -pthread "$SRC" -o "$BIN" \
  -Wl,--no-as-needed -Wl,-rpath,"$BUILD" -L"$BUILD" \
  -lgo2cpp_app -lgo2cpp_hash || exit 3

echo "[run] $BIN $*"
exec "$BIN" "$@"
