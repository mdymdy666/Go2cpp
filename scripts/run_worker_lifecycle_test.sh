#!/usr/bin/env bash
# 编译并运行「基础 M / 补充 M 回收策略」验收测试。
#
# 该测试不注册进 ctest（本任务不改 cmake/Go2CppTests.cmake），因此用
# 这个脚本手动编译、链接 build-quality 里的共享库并执行。
#
# 用法：
#   bash scripts/run_worker_lifecycle_test.sh
#   BUILD_DIR=build-debug-locks bash scripts/run_worker_lifecycle_test.sh
#
# 退出码：测试二进制自身的退出码；0 表示全部断言通过。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-quality}"
CXX="${CXX:-c++}"
OUT="${OUT:-/tmp/go2cpp_worker_lifecycle}"

if [[ ! -f "$BUILD/libgo2cpp_scheduler.so" ]]; then
    echo "缺少 $BUILD/libgo2cpp_scheduler.so，请先构建（cmake --build $BUILD）" >&2
    exit 2
fi

"$CXX" -std=c++20 -O3 -DNDEBUG -pthread \
    -I"$ROOT/include" -I"$ROOT/tests" \
    "$ROOT/tests/concurrency/test_worker_lifecycle.cpp" \
    -o "$OUT" \
    -Wl,--no-as-needed -Wl,-rpath,"$BUILD" -L"$BUILD" \
    -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_sync -lgo2cpp_channel \
    -lgo2cpp_future -lgo2cpp_network -lgo2cpp_math -lgo2cpp_rand \
    -lgo2cpp_path -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding \
    -lgo2cpp_hash -lgo2cpp_metrics -lgo2cpp_hook -lgo2cpp_io \
    -lgo2cpp_context -lgo2cpp_error -lgo2cpp_config -lgo2cpp_log \
    -lgo2cpp_control_flow -lz -ldl

# 每次压测加 timeout：卡死必须表现为失败而不是无限挂起。
timeout 300 "$OUT"
