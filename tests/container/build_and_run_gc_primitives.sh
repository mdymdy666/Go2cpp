#!/usr/bin/env bash
# groupcache 前置组件测试：singleflight + consistent hash。
#
# 只依赖头文件与 src/hash/crc32.cpp（默认哈希）；不引入 groupcache 主体。
#
# 用法：
#   tests/container/build_and_run_gc_primitives.sh
#   CXX=clang++ tests/container/build_and_run_gc_primitives.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-g++}"
OUT_DIR="${TMPDIR:-/tmp}/go2cpp_gc_primitives_test"
BIN="$OUT_DIR/test_groupcache_primitives"

if [[ ! -d "$ROOT/include" ]]; then
  echo "找不到头文件目录：$ROOT/include" >&2
  exit 1
fi
mkdir -p "$OUT_DIR"

CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror
          -I"$ROOT/include")

echo "[env] $(uname -srm)"
echo "[env] $("$CXX" --version | head -n 1)"
echo "[build] $CXX ${CXXFLAGS[*]} src/hash/crc32.cpp tests/container/test_groupcache_primitives.cpp"

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/src/hash/crc32.cpp" \
  "$ROOT/tests/container/test_groupcache_primitives.cpp" \
  -pthread -o "$BIN"

echo "[run] $BIN"
exec "$BIN" "$@"
