#!/usr/bin/env bash
# REQ-004 有界队列示例：编译 + 运行（不修改任何现有 CMake 文件）。
#
# 用法：
#   examples/bounded_queue/build_and_run.sh [--items N] [--scenario a|b|c|all]
#
# 环境变量：
#   BUILD_DIR  构建目录（默认 <root>/.build-bento-wsl）
#   OUT_DIR    输出目录（默认 /tmp/go2cpp_bounded_queue）
#   CXX        编译器（默认 g++）
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/.build-bento-wsl}
OUT=${OUT_DIR:-/tmp/go2cpp_bounded_queue}
CXX=${CXX:-g++}
SRC="$ROOT/examples/bounded_queue/bounded_queue_demo.cpp"
BIN="$OUT/bounded_queue_demo"

mkdir -p "$OUT" || exit 2

if [[ ! -d "$BUILD" ]]; then
  echo "构建目录不存在：$BUILD" >&2
  echo "先构建（Release）：cmake -S \"$ROOT\" -B \"$BUILD\" -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_BENTO=ON" >&2
  exit 2
fi

# go2cpp_runtime 被拆成多个共享库；这里按 ldd go2cpp_tests 的依赖顺序列出，
# 与 tests/concurrency/build_and_run_tests.sh 的做法一致（不新增 CMake 目标）。
LIBS=(
  -Wl,--no-as-needed
  -Wl,-rpath,"$BUILD"
  -L"$BUILD"
  -lgo2cpp_control_flow -lgo2cpp_channel -lgo2cpp_future -lgo2cpp_network
  -lgo2cpp_sync -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error
  -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_debug
  -lgo2cpp_debug_event -lgo2cpp_hash -lgo2cpp_math -lgo2cpp_rand
  -lgo2cpp_path -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding
  -lgo2cpp_metrics -lz -ldl -lm
)

echo "[build] $SRC"
"$CXX" -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic \
  -I"$ROOT/include" -pthread "$SRC" -o "$BIN" "${LIBS[@]}" || exit 3

echo "[run] $BIN $*"
exec "$BIN" "$@"
