#!/usr/bin/env bash
# 通用实时房间组件测试：编译 src/app/realtime_room.cpp + tests/app/test_realtime_room.cpp，
# 跑同房间广播、房间隔离、慢连接踢出、Leave/CloseAll、100 连接并发广播。
#
# 用法：
#   tests/app/build_and_run_realtime.sh
#   CXX=g++-13 tests/app/build_and_run_realtime.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-g++}"
OUT_DIR="${TMPDIR:-/tmp}/go2cpp_realtime_room_test"
BIN="$OUT_DIR/test_realtime_room"

if [[ ! -d "$ROOT/include" ]]; then
  echo "找不到头文件目录：$ROOT/include" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

# 只用标准库与 pthread，不依赖已构建的 go2cpp 静态库。
CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror
          -I"$ROOT/include" -I"$ROOT/tests/app")

echo "[env] $(uname -srm)"
echo "[env] $("$CXX" --version | head -n 1)"
echo "[build] $CXX ${CXXFLAGS[*]} src/app/realtime_room.cpp tests/app/test_realtime_room.cpp"

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/src/app/realtime_room.cpp" \
  "$ROOT/tests/app/test_realtime_room.cpp" \
  -pthread -o "$BIN"

echo "[run] $BIN"
exec "$BIN" "$@"
