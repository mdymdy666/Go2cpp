#!/usr/bin/env bash
# `\uXXXX` 解码回归测试：编译 app/douyin/tests/test_json_unescape.cpp 并运行。
#
# 覆盖 douyin 的 JSON 字符串解析层（douyin_http.hpp::JsonStringField）：
# BMP 中文、UTF-16 代理对、边界码点、常规转义不回归、非法序列必须拒绝。
#
# 用法：
#   app/douyin/scripts/run_json_unescape_test.sh                 # 用默认 Go2Cpp 构建目录
#   GO2CPP_BUILD=/path/to/build app/douyin/scripts/run_json_unescape_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
GO2CPP="${GO2CPP_ROOT:-/UserData/CodexWorkSpace/Go2cpp}"
BUILD_LIB="${GO2CPP_BUILD:-$GO2CPP/.build-bento-wsl}"
CXX="${CXX:-g++}"
OUT_DIR="${TMPDIR:-/tmp}/go2cpp_json_unescape_test"
BIN="$OUT_DIR/test_json_unescape"

if [[ ! -d "$BUILD_LIB" ]]; then
  echo "找不到 Go2Cpp 构建产物：$BUILD_LIB" >&2
  echo "请用 GO2CPP_BUILD=... 指定构建目录" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
          -I"$GO2CPP/include" -I"$ROOT/app/douyin/include")

# json 提供 \uXXXX 解码（含代理对）；network 一组是 douyin_http.hpp 的间接依赖。
LDFLAGS=(-L"$BUILD_LIB" -Wl,-rpath,"$BUILD_LIB" -Wl,--start-group
         -lgo2cpp_json -lgo2cpp_network -lgo2cpp_io -lgo2cpp_sync
         -lgo2cpp_context -lgo2cpp_error -lgo2cpp_scheduler -lgo2cpp_fiber
         -lgo2cpp_log -lgo2cpp_hash -Wl,--end-group -lpthread)

echo "[build] $CXX ${CXXFLAGS[*]} app/douyin/tests/test_json_unescape.cpp"
"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/app/douyin/tests/test_json_unescape.cpp" \
  "${LDFLAGS[@]}" -o "$BIN"

echo "[run] $BIN"
exec "$BIN"
