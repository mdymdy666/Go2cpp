#!/usr/bin/env bash
# 文件: scripts/run_concurrency_audit.sh
# 用途: 并发设施（channel / future / io）审计测试与性能基准的一键复现入口。
#
# 背景：
#   审计新增的三个测试/基准文件没有注册进 ctest（保持全量 ctest 113/113
#   的计数不变），因此在这里提供显式编译与运行命令。它们只链接 build-quality
#   下已构建的共享库，不修改 CMake，也不会被 ctest 统计。
#
# 用法：
#   scripts/run_concurrency_audit.sh tests   # 只跑语义/并发测试
#   scripts/run_concurrency_audit.sh bench   # 只跑性能基准（5 次中位数）
#   scripts/run_concurrency_audit.sh all     # 两者都跑（默认）
#
# 约束：
#   - 需要先有 Release 构建：cmake -S . -B build-quality -DCMAKE_BUILD_TYPE=Release
#   - 所有压测都套 `timeout 120`，避免异常路径失控污染宿主；
#   - 绑核运行（channel/future/bench 用 -c 0，io 用 -c 0-3），
#     并在前后打印 /proc/loadavg 与 procs_running（WSL2 下 loadavg 不可信）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-quality}"
CXX="${CXX:-g++}"
OUT="${OUT_DIR:-/tmp/go2cpp_concurrency_audit}"
MODE="${1:-all}"

if [[ ! -f "$BUILD/libgo2cpp_channel.so" ]]; then
    echo "找不到 $BUILD/libgo2cpp_channel.so；请先构建 Release 版本。" >&2
    exit 1
fi
mkdir -p "$OUT"

COMMON=(-std=c++20 -O2 -DNDEBUG -pthread -I"$ROOT/include")
LIBS=(-L"$BUILD" -Wl,-rpath,"$BUILD"
      -lgo2cpp_channel -lgo2cpp_future -lgo2cpp_io -lgo2cpp_context
      -lgo2cpp_error -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log)

build() {
    local src="$1" out="$2"
    echo "[build] $src"
    "$CXX" "${COMMON[@]}" "$src" -o "$out" "${LIBS[@]}"
}

env_snapshot() {
    echo "[env] loadavg: $(cat /proc/loadavg)"
    echo "[env] $(grep procs_running /proc/stat)"
}

run_tests() {
    build "$ROOT/tests/concurrency/test_channel_concurrency.cpp" "$OUT/channel_concurrency"
    build "$ROOT/tests/concurrency/test_future_audit.cpp" "$OUT/future_audit"
    build "$ROOT/tests/network/test_io_audit.cpp" "$OUT/io_audit"

    env_snapshot
    echo "=== channel 并发语义 ==="
    timeout 120 taskset -c 0 "$OUT/channel_concurrency"
    echo "=== future 语义 ==="
    timeout 120 taskset -c 0 "$OUT/future_audit"
    echo "=== io 并发/竞态 ==="
    timeout 120 taskset -c 0-3 "$OUT/io_audit"
    env_snapshot
}

run_bench() {
    build "$ROOT/benchmarks/audit_concurrency_benchmark.cpp" "$OUT/benchmark"
    env_snapshot
    echo "=== channel / future / io 基准（taskset -c 0，5 次中位数）==="
    timeout 120 taskset -c 0 "$OUT/benchmark"
    env_snapshot
}

case "$MODE" in
    tests) run_tests ;;
    bench) run_bench ;;
    all) run_tests; run_bench ;;
    *) echo "用法: $0 [tests|bench|all]" >&2; exit 2 ;;
esac
