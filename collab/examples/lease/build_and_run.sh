#!/usr/bin/env bash
# JinShan REQ-005 示例：Lease 到期一次 / Renew 延后 / Stop 不再触发。编译 + 运行（不改 CMake）。
#
# 用法：
#   collab/examples/lease/build_and_run.sh
#
# 环境变量：BUILD_DIR（默认 <root>/.build-bento-wsl）、OUT_DIR（默认 /tmp/go2cpp_ex_lease）、CXX。
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/.build-bento-wsl}
OUT=${OUT_DIR:-/tmp/go2cpp_ex_lease}
CXX=${CXX:-g++}
BIN="$OUT/ex_lease"

mkdir -p "$OUT" || exit 2
if [[ ! -d "$BUILD" || ! -f "$BUILD/libgo2cpp_relay.so" ]]; then
  echo "缺少构建产物：$BUILD/libgo2cpp_relay.so（先做一次全量构建）" >&2
  exit 2
fi

echo "[build] $ROOT/collab/examples/lease/main.cpp"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$ROOT/include" -pthread \
  "$ROOT/collab/examples/lease/main.cpp" -o "$BIN" \
  -Wl,--no-as-needed -Wl,-rpath,"$BUILD" -L"$BUILD" \
  -lgo2cpp_relay -lgo2cpp_context -lgo2cpp_error || exit 3

echo "[run] $BIN"
exec "$BIN"
