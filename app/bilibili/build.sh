#!/usr/bin/env bash
# 一键编译 B 站弹幕服务（danmaku_service）。
#
# 依赖 Go2Cpp 已编译好的库（默认 $GO2CPP_ROOT/.build-bento-wsl）。
# 只读引用 Go2Cpp 的头文件与 .so，不改动其源码树。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GO2CPP="${GO2CPP_ROOT:-/UserData/CodexWorkSpace/Go2cpp}"
BUILD_LIB="${GO2CPP_BUILD:-$GO2CPP/.build-bento-wsl}"
CXX="${CXX:-g++}"

if [[ ! -d "$GO2CPP/include" ]]; then
  echo "找不到 Go2Cpp 头文件：$GO2CPP/include" >&2
  echo "请用 GO2CPP_ROOT=... 指定源码树路径" >&2
  exit 1
fi
if [[ ! -d "$BUILD_LIB" ]]; then
  echo "找不到 Go2Cpp 构建产物：$BUILD_LIB" >&2
  echo "请用 GO2CPP_BUILD=... 指定构建目录" >&2
  exit 1
fi

mkdir -p "$ROOT/build" "$ROOT/logs" "$ROOT/data"

CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
          -I"$GO2CPP/include" -I"$ROOT/include")

# network 依赖 io/sync/context 等；storage 依赖 sqlite3。
LDFLAGS=(-L"$BUILD_LIB" -Wl,-rpath,"$BUILD_LIB" -Wl,--start-group
         -lgo2cpp_network -lgo2cpp_io -lgo2cpp_sync -lgo2cpp_context
         -lgo2cpp_error -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log
         -lgo2cpp_hash -lgo2cpp_storage -lgo2cpp_encoding -Wl,--end-group
         -lsqlite3 -lpthread)

echo "[build] Go2Cpp 头文件: $GO2CPP/include"
echo "[build] Go2Cpp 库目录: $BUILD_LIB"

printf '  CC %-20s ' "danmaku_service"
if "$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/src/danmaku_service.cpp" \
  "$ROOT/src/bili_danmaku_room.cpp" \
  "$ROOT/src/bili_store.cpp" \
  "${LDFLAGS[@]}" -o "$ROOT/build/danmaku_service"; then
  echo "ok"
else
  echo "失败"
  exit 1
fi

echo "[build] 完成：$ROOT/build/danmaku_service"
