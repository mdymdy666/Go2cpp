#!/usr/bin/env bash
# 通用实时房间组件测试：编译 src/app/realtime_room.cpp + tests/app/test_realtime_room.cpp，
# 跑同房间广播、房间隔离、慢连接踢出、Leave/CloseAll、100 连接并发广播。
#
# 用法：
#   tests/app/build_and_run_realtime.sh
#   CXX=g++-13 tests/app/build_and_run_realtime.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_LIB="${GO2CPP_BUILD:-$ROOT/.build-bento-wsl}"
CXX="${CXX:-g++}"
OUT_DIR="${TMPDIR:-/tmp}/go2cpp_realtime_room_test"
BIN="$OUT_DIR/test_realtime_room"

if [[ ! -d "$ROOT/include" ]]; then
  echo "找不到头文件目录：$ROOT/include" >&2
  exit 1
fi
if [[ ! -d "$BUILD_LIB" ]]; then
  echo "找不到 Go2Cpp 构建产物：$BUILD_LIB" >&2
  echo "请用 GO2CPP_BUILD=... 指定构建目录" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

# realtime_room.cpp 用 GO2CPP_LOG_WARN，必须链接日志实现。两种路径与 CMake
# 的 go2cpp_app 保持一致：构建目录有 libgo2cpp_log 就链接它；否则（例如
# GO2CPP_WITH_LOGGING=OFF 的 standalone 构建）直接把 src/logging/log.cpp
# 作为额外编译单元，避免脚本因缺库直接失败。
CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror
          -I"$ROOT/include" -I"$ROOT/tests/app")
LOG_SRCS=()
LDFLAGS=(-L"$BUILD_LIB" -Wl,-rpath,"$BUILD_LIB")
if compgen -G "$BUILD_LIB/libgo2cpp_log.*" > /dev/null; then
  LDFLAGS+=(-lgo2cpp_log)
else
  LOG_SRCS+=("$ROOT/src/logging/log.cpp")
fi

echo "[env] $(uname -srm)"
echo "[env] $("$CXX" --version | head -n 1)"
echo "[env] build_lib=$BUILD_LIB"
echo "[build] $CXX ${CXXFLAGS[*]} src/app/realtime_room.cpp tests/app/test_realtime_room.cpp"

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/src/app/realtime_room.cpp" \
  "$ROOT/tests/app/test_realtime_room.cpp" \
  "${LOG_SRCS[@]}" \
  "${LDFLAGS[@]}" -pthread -o "$BIN"

echo "[run] $BIN"
exec "$BIN" "$@"
