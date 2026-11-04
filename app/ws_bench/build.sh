#!/usr/bin/env bash
# 构建 ws_bench 的三个二进制：Go2Cpp 服务端、C++ 压测客户端、Go 对照服务端。
#
# 依赖：
#   - go2cpp 仓库源码与 header（本脚本位于 app/ws_bench/ 下）；
#   - cmake / g++ / Go 1.27（/usr/local/go/bin/go）。
#
# 用法：
#   bash app/ws_bench/build.sh
#   BUILD=$HOME/wsbench-build BIN=$HOME/wsbench-bin bash app/ws_bench/build.sh
#
# 说明：go2cpp 的 WsServer 目标（go2cpp_bento_ws）默认不构建，本脚本用
# `-DGO2CPP_WITH_BENTO=ON` 在仓库外（$BUILD）单独配置构建，不改仓库文件。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$HOME/wsbench-build}
BIN=/home/ubunturoot/wsbench-bin
GO=${GO:-/usr/local/go/bin/go}

mkdir -p "$BIN"
cd "$ROOT"

if [[ ! -f "$BUILD/libgo2cpp_bento_ws.a" ]]; then
    echo "== configure go2cpp (BENTO=ON) -> $BUILD"
    cmake -S . -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DGO2CPP_WITH_BENTO=ON \
        -DGO2CPP_BUILD_TESTS=OFF \
        -DGO2CPP_BENTO_TESTS=OFF
    cmake --build "$BUILD" --target go2cpp_bento_ws -j "$(nproc)"
fi

echo "== build ws_bench_load"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
    app/ws_bench/load.cpp -o "$BIN/ws_bench_load"

echo "== build ws_bench_go"
(cd app/ws_bench/go_server && "$GO" build -o "$BIN/ws_bench_go" main.go)

echo "== build ws_bench_server"
libs=()
for so in "$BUILD"/*.so; do
    libs+=("$so")
done
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
    app/ws_bench/server.cpp \
    "$BUILD/libgo2cpp_bento_ws.a" "$BUILD/libgo2cpp_bento.a" "${libs[@]}" \
    -Wl,-rpath,"$BUILD" -lpthread -ldl -o "$BIN/ws_bench_server"

echo "== done"
ls -la "$BIN/ws_bench_server" "$BIN/ws_bench_load" "$BIN/ws_bench_go"
