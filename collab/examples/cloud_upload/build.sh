#!/usr/bin/env bash
# JinShan REQ-011 示例构建：编译 cloud_up（上传客户端）与 cloud_server（模拟服务端）。
# 复用仓库已有 .build-bento-wsl 动态库，不改任何 CMake 文件。
#
# 用法：
#   collab/examples/cloud_upload/build.sh
#
# 环境变量：BUILD_DIR（默认 <root>/.build-bento-wsl）、OUT_DIR（默认 /tmp/go2cpp_ex_cloud_upload）、CXX。
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/.build-bento-wsl}
OUT=${OUT_DIR:-/tmp/go2cpp_ex_cloud_upload}
CXX=${CXX:-g++}
SRC="$ROOT/collab/examples/cloud_upload"

mkdir -p "$OUT"
if [[ ! -f "$BUILD/libgo2cpp_network.so" || ! -f "$BUILD/libgo2cpp_hash.so" ]]; then
  echo "缺少构建产物：$BUILD/libgo2cpp_network.so 或 libgo2cpp_hash.so（先做一次全量构建）" >&2
  exit 2
fi

FLAGS=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$ROOT/include" -pthread)
LIBS=(-Wl,-rpath,"$BUILD" -L"$BUILD" -lgo2cpp_network -lgo2cpp_hash)

echo "[build] cloud_up"
"$CXX" "${FLAGS[@]}" "$SRC/uploader.cpp" "$SRC/main.cpp" -o "$OUT/cloud_up" "${LIBS[@]}"
echo "[build] cloud_server"
"$CXX" "${FLAGS[@]}" "$SRC/server.cpp" -o "$OUT/cloud_server" "${LIBS[@]}"

echo "[ok] $OUT/cloud_up"
echo "[ok] $OUT/cloud_server"
echo "运行：$OUT/cloud_server --root /tmp/upload_store"
echo "      $OUT/cloud_up --file <path> --endpoint http://127.0.0.1:29300 --chunk-size 8M"
