#!/usr/bin/env bash
# 通用网关组件测试：编译 src/app/gateway.cpp + tests/app/test_gateway.cpp，
# 起 2 个 echo 上游，跑路由/鉴权/透传/超时/502/健康聚合/中间件/Resolver/配置/压测。
#
# 用法：
#   tests/app/build_and_run_gateway.sh
#   GO2CPP_BUILD=/path/to/build tests/app/build_and_run_gateway.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_LIB="${GO2CPP_BUILD:-$ROOT/.build-bento-wsl}"
CXX="${CXX:-g++}"
OUT_DIR="${TMPDIR:-/tmp}/go2cpp_gateway_test"
BIN="$OUT_DIR/test_gateway"

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

CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror
          -I"$ROOT/include" -I"$ROOT/tests/app")

# network 依赖 io/sync/context 等；--start-group 消掉静态链接顺序问题。
LDFLAGS=(-L"$BUILD_LIB" -Wl,-rpath,"$BUILD_LIB" -Wl,--start-group
         -lgo2cpp_network -lgo2cpp_io -lgo2cpp_sync -lgo2cpp_context
         -lgo2cpp_error -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log
         -lgo2cpp_hash -Wl,--end-group -lpthread)

echo "[env] $(uname -srm)"
echo "[env] $("$CXX" --version | head -n 1)"
echo "[env] build_lib=$BUILD_LIB"
echo "[build] $CXX ${CXXFLAGS[*]} src/app/gateway.cpp tests/app/test_gateway.cpp"

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/src/app/gateway.cpp" \
  "$ROOT/tests/app/test_gateway.cpp" \
  "${LDFLAGS[@]}" -o "$BIN"

echo "[run] $BIN"
exec "$BIN" "$@"
