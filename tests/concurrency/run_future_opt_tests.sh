#!/usr/bin/env bash
# 文件: tests/concurrency/run_future_opt_tests.sh
# 用途: Future 无锁优化的正确性测试一键复现（普通 / ASan / TSan）。
#
# 用法:
#   tests/concurrency/run_future_opt_tests.sh                # 普通 Release
#   BUILD_DIR=.build-future-asan MODE=asan tests/concurrency/run_future_opt_tests.sh
#   BUILD_DIR=.build-future-tsan MODE=tsan tests/concurrency/run_future_opt_tests.sh
#
# 说明:
#   - MODE=asan/tsan 时库必须也用同一 sanitizer 构建（BUILD_DIR 指向插桩构建），
#     否则跨库不插桩会导致误报或漏检；
#   - 测试覆盖 6 个场景 + 随机 fuzz + 抢占竞态 + 边界，见 test_future_opt.cpp。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/.build-bento-wsl}"
MODE="${MODE:-plain}"
CXX="${CXX:-g++}"
OUT="${OUT_DIR:-/tmp/future_opt_tests}"
mkdir -p "$OUT"

case "$MODE" in
    plain)
        SAN_FLAGS=(-O2 -DNDEBUG)
        ;;
    asan)
        SAN_FLAGS=(-fsanitize=address -fno-omit-frame-pointer -g -O1)
        ;;
    tsan)
        SAN_FLAGS=(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
        ;;
    *)
        echo "未知 MODE=$MODE（plain/asan/tsan）" >&2
        exit 2
        ;;
esac

COMMON=(-std=c++17 -pthread -I"$ROOT/include" "${SAN_FLAGS[@]}")
LIBS=(-L"$BUILD" -Wl,-rpath,"$BUILD"
      -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context -lgo2cpp_future
      -lgo2cpp_sync -lgo2cpp_error -lgo2cpp_log -lgo2cpp_debug_event
      -lgo2cpp_debug -lgo2cpp_control_flow)

echo "[env] mode=$MODE build=$BUILD"
"$CXX" "${COMMON[@]}" "$ROOT/tests/concurrency/test_future_opt.cpp" \
    -o "$OUT/test_future_opt_${MODE}" "${LIBS[@]}"

if command -v taskset >/dev/null 2>&1; then
    RUN=(taskset -c 0-3)
else
    RUN=()
fi

echo "=== Future 优化测试（$MODE）==="
timeout 600 "${RUN[@]}" "$OUT/test_future_opt_${MODE}"
