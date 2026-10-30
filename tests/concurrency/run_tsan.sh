#!/usr/bin/env bash
# ThreadSanitizer 冒烟：构建插桩版 runtime，编译并运行锁/通道/CV/PC/Timer 测试。
#
# 只用于本地验证，不注册 ctest。用法：
#   tests/concurrency/run_tsan.sh
# 环境变量：
#   TSAN_BUILD_DIR  插桩构建目录（默认 /tmp/go2cpp-tsan-build-on）
#   TSAN_OUT_DIR    输出目录（默认 /tmp/go2cpp_tsan）
#   TSAN_REPORT     报告路径（默认 /tmp/tsan_report.txt）
#
# 注：当前工作树在 GO2CPP_DEBUG_DUMP=OFF 下编译失败（debug_event_ring.cpp
# 的 NowNs 只在 ON 分支可见），因此插桩构建使用 DEBUG_DUMP=ON；OFF 修复后
# 可把 -DGO2CPP_DEBUG_DUMP 改回 OFF 并去掉测试的 -DGO2CPP_DEBUG_DUMP=1。
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${TSAN_BUILD_DIR:-/tmp/go2cpp-tsan-build-on}
OUT=${TSAN_OUT_DIR:-/tmp/go2cpp_tsan}
REPORT=${TSAN_REPORT:-/tmp/tsan_report.txt}
CXX=${CXX:-g++}
JOBS=${JOBS:-$(nproc)}
mkdir -p "$OUT"

if [[ ! -f "$BUILD/libgo2cpp_sync.so" ]]; then
    echo "[tsan] configuring $BUILD"
    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DGO2CPP_BUILD_TESTS=OFF \
        -DGO2CPP_BUILD_EXAMPLES=OFF \
        -DGO2CPP_ENABLE_TSAN=ON \
        -DGO2CPP_DEBUG_DUMP=ON || { echo "[tsan] configure FAILED"; exit 2; }
    cmake --build "$BUILD" -j"$JOBS" || { echo "[tsan] build FAILED"; exit 2; }
fi

echo "[tsan] building test"
"$CXX" -std=c++20 -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
    -DGO2CPP_DEBUG_DUMP=1 \
    -I"$ROOT/include" -I"$ROOT/tests" -pthread \
    "$ROOT/tests/concurrency/test_tsan_smoke.cpp" \
    -o "$OUT/tsan_smoke" \
    -Wl,--no-as-needed -Wl,-rpath,"$BUILD" -L"$BUILD" \
    -lgo2cpp_control_flow -lgo2cpp_channel -lgo2cpp_future \
    -lgo2cpp_network -lgo2cpp_math -lgo2cpp_rand -lgo2cpp_path \
    -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding \
    -lgo2cpp_metrics -lgo2cpp_debug -lgo2cpp_sync -lgo2cpp_debug_event \
    -lgo2cpp_hook -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error \
    -lgo2cpp_config -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log \
    -lgo2cpp_hash -lz -ldl -lm || { echo "[tsan] test build FAILED"; exit 2; }

rm -f "$OUT"/tsan.*
RUN=(env "TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:log_path=$OUT/tsan" \
     "$OUT/tsan_smoke")
if command -v setarch >/dev/null 2>&1; then
    RUN=(setarch "$(uname -m)" -R "${RUN[@]}")
fi

echo "[tsan] running (setarch -R to avoid WSL ASLR incompatibility)"
"${RUN[@]}" >"$OUT/stdout.log" 2>&1
STATUS=$?

RACE_FILES=$(ls "$OUT"/tsan.* 2>/dev/null || true)
RACES=0
if [[ -n "$RACE_FILES" ]]; then
    RACES=$(grep -h "WARNING: ThreadSanitizer" $RACE_FILES 2>/dev/null | wc -l)
fi

{
    echo "=== Go2Cpp TSAN report ==="
    echo "build: $BUILD"
    echo "test exit: $STATUS"
    echo "race warnings: $RACES"
    echo "stdout:"
    sed 's/^/  /' "$OUT/stdout.log" 2>/dev/null || true
    if [[ "$RACES" -gt 0 ]]; then
        echo "first race:"
        grep -h -A 8 "WARNING: ThreadSanitizer" $RACE_FILES 2>/dev/null | head -30
    fi
    echo "status: $([[ $STATUS -eq 0 && $RACES -eq 0 ]] && echo CLEAN || echo RACES_OR_ERRORS)"
} | tee "$REPORT"

if [[ $STATUS -ne 0 || $RACES -ne 0 ]]; then
    exit 1
fi
echo "[tsan] CLEAN (no data races reported)"
exit 0
