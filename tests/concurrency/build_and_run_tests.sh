#!/usr/bin/env bash
# 并发原语正确性测试的独立编译/运行入口（不修改 CMake，不注册 ctest）。
#
# 用法：
#   tests/concurrency/build_and_run_tests.sh [all|comb|fuzz|deadlock|debug]
#   tests/concurrency/build_and_run_tests.sh fuzz --rounds 1000 --ops 10000 --seed 42
#
# 环境变量：
#   OFF_BUILD  默认构建目录（默认 <root>/build-work）
#   ON_BUILD   DEBUG_DUMP=ON 构建目录（默认 <root>/build-debug-full）
#   OUT_DIR    输出二进制/日志目录（默认 /tmp/go2cpp_concurrency_tests）
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OFF_BUILD=${OFF_BUILD:-$ROOT/build-work}
ON_BUILD=${ON_BUILD:-$ROOT/build-debug-full}
OUT=${OUT_DIR:-/tmp/go2cpp_concurrency_tests}
CXX=${CXX:-g++}
JOBS=${JOBS:-$(nproc)}
mkdir -p "$OUT"

runtime_libs() {
    local build=$1
    printf '%s\n' \
        -Wl,--no-as-needed -Wl,-rpath,"$build" -L"$build" \
        -lgo2cpp_control_flow -lgo2cpp_channel -lgo2cpp_future \
        -lgo2cpp_network -lgo2cpp_math -lgo2cpp_rand -lgo2cpp_path \
        -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding \
        -lgo2cpp_metrics -lgo2cpp_debug -lgo2cpp_sync -lgo2cpp_debug_event \
        -lgo2cpp_hook -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error \
        -lgo2cpp_config -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log \
        -lgo2cpp_hash -lz -ldl -lm
}

build_test() {
    local src=$1 name=$2 build=$3
    shift 3
    local libs
    mapfile -t libs < <(runtime_libs "$build")
    echo "[build] $name ($src)"
    "$CXX" -std=c++20 -O2 -DNDEBUG -I"$ROOT/include" -I"$ROOT/tests" \
        -pthread "$@" "$ROOT/$src" -o "$OUT/$name" "${libs[@]}" \
        || return 1
}

run_test() {
    local name=$1
    shift
    echo "[run] $name"
    timeout 600 "$OUT/$name" "$@" 2>&1 | tee "$OUT/$name.log"
    return "${PIPESTATUS[0]}"
}

build_off() {
    build_test tests/concurrency/test_concurrency_combinations.cpp \
        combinations "$OFF_BUILD" &&
    build_test tests/concurrency/test_concurrency_fuzz.cpp \
        fuzz "$OFF_BUILD" &&
    build_test tests/concurrency/test_deadlock_detection.cpp \
        deadlock "$OFF_BUILD"
}

build_debug() {
    build_test tests/debug/test_debug_record_correctness.cpp \
        debug_record "$ON_BUILD" -DGO2CPP_DEBUG_DUMP=1
}

CMD=${1:-all}
case "$CMD" in
    comb)
        build_off >/dev/null 2>&1 || true
        build_test tests/concurrency/test_concurrency_combinations.cpp \
            combinations "$OFF_BUILD" || exit 2
        run_test combinations
        ;;
    fuzz)
        shift
        build_test tests/concurrency/test_concurrency_fuzz.cpp fuzz \
            "$OFF_BUILD" || exit 2
        run_test fuzz "$@"
        ;;
    deadlock)
        build_test tests/concurrency/test_deadlock_detection.cpp deadlock \
            "$OFF_BUILD" || exit 2
        run_test deadlock
        ;;
    debug)
        build_debug || exit 2
        run_test debug_record
        ;;
    tsan)
        exec "$ROOT/tests/concurrency/run_tsan.sh"
        ;;
    all)
        build_off || exit 2
        build_debug || exit 2
        run_test combinations || exit 1
        run_test fuzz --rounds 1000 --ops 10000 --seed 20261021 || exit 1
        run_test deadlock || exit 1
        run_test debug_record || exit 1
        ;;
    *)
        echo "usage: $0 [all|comb|fuzz|deadlock|debug|tsan]" >&2
        exit 2
        ;;
esac
