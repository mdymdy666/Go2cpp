#!/usr/bin/env bash
# 上传读取时限回归测试：编译 src/app/gateway.cpp + app/douyin/tests/test_upload_timeout.cpp
# 并运行（真实网关 + 裸 socket 客户端，覆盖 1/10/32/33/64 MiB、CL 异常、chunked、慢速、并发慢速）。
#
# 用法：
#   app/douyin/scripts/run_upload_timeout_test.sh
#   GO2CPP_BUILD=/path/to/build app/douyin/scripts/run_upload_timeout_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_LIB="${GO2CPP_BUILD:-$ROOT/.build-bento-wsl}"
CXX="${CXX:-g++}"
OUT_DIR="${TMPDIR:-/tmp}/go2cpp_upload_timeout_test"
BIN="$OUT_DIR/test_upload_timeout"

if [[ ! -d "$BUILD_LIB" ]]; then
  echo "找不到 Go2Cpp 构建产物：$BUILD_LIB" >&2
  echo "请用 GO2CPP_BUILD=... 指定构建目录" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror
          -I"$ROOT/include" -I"$ROOT/tests/app")

# network 依赖 io/sync/context 等；--start-group 消掉静态链接顺序问题。
LDFLAGS=(-L"$BUILD_LIB" -Wl,-rpath,"$BUILD_LIB" -Wl,--start-group
         -lgo2cpp_network -lgo2cpp_io -lgo2cpp_sync -lgo2cpp_context
         -lgo2cpp_error -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log
         -lgo2cpp_hash -Wl,--end-group -lpthread)

echo "[build] $CXX ${CXXFLAGS[*]} src/app/gateway.cpp app/douyin/tests/test_upload_timeout.cpp"
"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/src/app/gateway.cpp" \
  "$ROOT/app/douyin/tests/test_upload_timeout.cpp" \
  "${LDFLAGS[@]}" -o "$BIN"

echo "[run] $BIN"
exec "$BIN"
