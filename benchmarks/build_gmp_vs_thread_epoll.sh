#!/usr/bin/env bash
# 构建 gmp_vs_thread_epoll_benchmark（不改 cmake/Go2CppTests.cmake）。
#
# 为什么不直接加 CMake 目标：用户要求不修改 Go2CppTests.cmake，本脚本复用
# 已有 build-quality 里 go2cpp_runtime 的产物与同名链接顺序，编译一个独立
# 可执行文件。待用户确认后再把文件末尾的 CMake 片段并入测试构建。
#
# 用法：
#   bash benchmarks/build_gmp_vs_thread_epoll.sh [build-quality 目录]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$root/build-quality"}
out="$root/build-gmp-vs-thread"

if [[ ! -f "$build/libgo2cpp_network.so" ]]; then
    echo "找不到 $build/libgo2cpp_network.so，请先构建项目" >&2
    exit 1
fi

mkdir -p "$out"

g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
    -I"$root/include" -I"$root/third_party" \
    "$root/benchmarks/gmp_vs_thread_epoll_benchmark.cpp" \
    -o "$out/gmp_vs_thread_epoll_benchmark" \
    -Wl,-rpath,"$build" -L"$build" \
    -lgo2cpp_network -lgo2cpp_io -lgo2cpp_scheduler -lgo2cpp_fiber \
    -lgo2cpp_context -lgo2cpp_sync -lgo2cpp_hook -lgo2cpp_config \
    -lgo2cpp_log -lgo2cpp_error \
    -lz -ldl -lpthread -lm

echo "构建完成: $out/gmp_vs_thread_epoll_benchmark"
