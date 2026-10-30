#!/usr/bin/env bash
# ThreadSanitizer 全库插桩：用 -fsanitize=thread 构建 runtime 共享库，
# 再编译并运行 channel + sync 的线程级测试（不启动 Scheduler / Fiber）。
#
# 为什么只跑线程级测试：
#   Fiber 使用自定义栈，切换栈时 TSAN 无法维护 shadow stack，会得到假阳性；
#   只有不启动 Scheduler 的普通 std::thread 测试才是可靠的 race 检测面。
#   因此 test_channel_concurrency / test_sync / test_locks（都启动 Scheduler）
#   不纳入本脚本。
#
# 覆盖矩阵（共享库全部插桩，测试只负责并发驱动）：
#   tsan_smoke           锁 / ConditionVariable / Channel / ParkingCondition / Timer
#   tsan_channel_stress  817 行 channel 高频混合压测
#                        （mpmc64 / mpmc8_64 / mpmc8_0 / mpmc1 / mpmc0 / rtt0 / mixed）
#   tsan_primitives      10 类并发结构压力（Mutex/RWMutex/RecursiveMutex/
#                        TimedMutex/SpinMutex/FutexMutex/CV/WaitGroup/PC/Channel）
#   tsan_pc_stress       ParkingCondition futex 丢唤醒专项压测
#
# 只用于本地验证，不注册 ctest。用法：
#   tests/concurrency/run_tsan.sh
# 环境变量：
#   TSAN_BUILD_DIR    插桩构建目录（默认 /tmp/go2cpp-tsan-build-on）
#   TSAN_OUT_DIR      输出目录（默认 /tmp/go2cpp_tsan）
#   TSAN_REPORT       报告路径（默认 /tmp/tsan_report.txt）
#   TSAN_CHANNEL_MSGS channel 压测每场景消息数（默认 20000）
#   TSAN_DEBUG_DUMP   OFF=生产配置 / ON=dump 插桩配置（默认 ON）
#   CXX / JOBS        编译器与并行度
#
# 两种插桩配置（库与测试必须一致，否则 GO2CPP_DEBUG_RESOURCES 的 ABI 不同）：
#   TSAN_DEBUG_DUMP=OFF：接近生产配置，不编译资源事件调试读；
#   TSAN_DEBUG_DUMP=ON ：含 dump/回放插桩，会额外暴露 sync 调试读竞态。
# 默认 ON 是为了兼容既有 TSAN_BUILD_DIR；换配置请同时换构建目录，例如
#   TSAN_BUILD_DIR=/tmp/go2cpp-tsan-build-off TSAN_DEBUG_DUMP=OFF \
#   TSAN_OUT_DIR=/tmp/go2cpp_tsan_off tests/concurrency/run_tsan.sh
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${TSAN_BUILD_DIR:-/tmp/go2cpp-tsan-build-on}
OUT=${TSAN_OUT_DIR:-/tmp/go2cpp_tsan}
REPORT=${TSAN_REPORT:-/tmp/tsan_report.txt}
CXX=${CXX:-g++}
JOBS=${JOBS:-$(nproc)}
CHANNEL_MSGS=${TSAN_CHANNEL_MSGS:-20000}
DUMP_MODE=${TSAN_DEBUG_DUMP:-ON}
if [[ "${DUMP_MODE}" == "OFF" ]]; then
    DUMP_VALUE=0
else
    DUMP_MODE=ON
    DUMP_VALUE=1
fi
mkdir -p "$OUT"

if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    echo "[tsan] configuring $BUILD"
    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DGO2CPP_BUILD_TESTS=OFF \
        -DGO2CPP_BUILD_EXAMPLES=OFF \
        -DGO2CPP_ENABLE_TSAN=ON \
        -DGO2CPP_DEBUG_DUMP=${DUMP_MODE} || { echo "[tsan] configure FAILED"; exit 2; }
fi

CACHED_DUMP=$(grep -m1 '^GO2CPP_DEBUG_DUMP:BOOL=' "$BUILD/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
if [[ -n "${CACHED_DUMP}" && "${CACHED_DUMP}" != "${DUMP_MODE}" ]]; then
    echo "[tsan] 配置不一致：$BUILD 的 GO2CPP_DEBUG_DUMP=${CACHED_DUMP}，" \
        "但 TSAN_DEBUG_DUMP=${DUMP_MODE}；请换构建目录或删掉该目录" >&2
    exit 2
fi

# 每次都做增量构建：插桩库与源码同步，避免复用旧的 .so。
echo "[tsan] building instrumented runtime in $BUILD"
cmake --build "$BUILD" -j"$JOBS" || { echo "[tsan] build FAILED"; exit 2; }

LIBS=(
    -lgo2cpp_control_flow -lgo2cpp_channel -lgo2cpp_future
    -lgo2cpp_network -lgo2cpp_math -lgo2cpp_rand -lgo2cpp_path
    -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding
    -lgo2cpp_metrics -lgo2cpp_debug -lgo2cpp_sync -lgo2cpp_debug_event
    -lgo2cpp_hook -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error
    -lgo2cpp_config -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log
    -lgo2cpp_hash -lz -ldl -lm
)

build_test() { # <name> <source>
    local name="$1"
    local source="$2"
    echo "[tsan] compiling $name <- $source"
    "$CXX" -std=c++20 -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
        -DGO2CPP_DEBUG_DUMP=${DUMP_VALUE} \
        -I"$ROOT/include" -I"$ROOT/tests" -pthread \
        "$ROOT/$source" \
        -o "$OUT/$name" \
        -Wl,--no-as-needed -Wl,-rpath,"$BUILD" -L"$BUILD" \
        "${LIBS[@]}"
}

build_test tsan_smoke tests/concurrency/test_tsan_smoke.cpp ||
    { echo "[tsan] tsan_smoke build FAILED"; exit 2; }
build_test tsan_channel_stress \
    tests/concurrency/test_channel_thread_stress.cpp ||
    { echo "[tsan] tsan_channel_stress build FAILED"; exit 2; }
build_test tsan_primitives tests/concurrency/test_primitives_stress.cpp ||
    { echo "[tsan] tsan_primitives build FAILED"; exit 2; }
build_test tsan_pc_stress tests/concurrency/test_parking_condition_stress.cpp ||
    { echo "[tsan] tsan_pc_stress build FAILED"; exit 2; }

HAVE_SETARCH=0
if command -v setarch >/dev/null 2>&1; then
    HAVE_SETARCH=1
    echo "[tsan] 用 setarch -R 关闭 ASLR（规避 WSL 下 TSAN 的 unexpected memory mapping）"
fi

NAMES=()
STATUSES=()
RACES=()
LOGS=()

run_test() { # <name> [test args...]
    local name="$1"
    shift
    local log="$OUT/$name.log"
    local files
    rm -f "$OUT/$name".tsan.*
    local cmd=("$OUT/$name" "$@")
    if [[ $HAVE_SETARCH -eq 1 ]]; then
        cmd=(setarch "$(uname -m)" -R "${cmd[@]}")
    fi
    echo "[tsan] running $name $*"
    env "TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:log_path=$OUT/$name.tsan" \
        "${cmd[@]}" >"$log" 2>&1
    local status=$?
    local races=0
    files=$(ls "$OUT/$name".tsan.* 2>/dev/null || true)
    if [[ -n "$files" ]]; then
        races=$(grep -h "WARNING: ThreadSanitizer" $files 2>/dev/null | wc -l | tr -d ' ')
    fi
    NAMES+=("$name")
    STATUSES+=("$status")
    RACES+=("$races")
    LOGS+=("$log")
    echo "[tsan] $name exit=$status races=$races"
}

run_test tsan_smoke
run_test tsan_channel_stress --rounds=1 --msgs="$CHANNEL_MSGS" \
    --scenario=all --hang-sec=120 --no-gdb
run_test tsan_primitives
run_test tsan_pc_stress

FAILED=0
TOTAL_RACES=0
for i in "${!NAMES[@]}"; do
    if [[ "${STATUSES[$i]}" -ne 0 || "${RACES[$i]}" -ne 0 ]]; then
        FAILED=1
    fi
    TOTAL_RACES=$((TOTAL_RACES + RACES[i]))
done

{
    echo "=== Go2Cpp TSAN report（全库插桩 + channel/sync 线程级测试）==="
    echo "build: $BUILD"
    echo "插桩配置: GO2CPP_DEBUG_DUMP=$DUMP_MODE"
    echo "channel 压测规模: rounds=1 msgs=$CHANNEL_MSGS scenario=all"
    echo
    printf '%-22s %-8s %-8s %s\n' "test" "exit" "races" "log"
    for i in "${!NAMES[@]}"; do
        printf '%-22s %-8s %-8s %s\n' \
            "${NAMES[$i]}" "${STATUSES[$i]}" "${RACES[$i]}" "${LOGS[$i]}"
    done
    echo
    echo "race warnings total: $TOTAL_RACES"
    for i in "${!NAMES[@]}"; do
        if [[ "${RACES[$i]}" -gt 0 ]]; then
            echo
            echo "--- first race in ${NAMES[$i]} ---"
            grep -h -A 12 "WARNING: ThreadSanitizer" \
                "$OUT/${NAMES[$i]}".tsan.* 2>/dev/null | head -40
        fi
        if [[ "${STATUSES[$i]}" -ne 0 ]]; then
            echo
            echo "--- ${NAMES[$i]} stdout tail (exit ${STATUSES[$i]}) ---"
            tail -20 "${LOGS[$i]}" 2>/dev/null || true
        fi
    done
    echo
    echo "status: $([[ $FAILED -eq 0 ]] && echo CLEAN || echo RACES_OR_ERRORS)"
} | tee "$REPORT"

if [[ $FAILED -ne 0 ]]; then
    exit 1
fi
echo "[tsan] CLEAN (no data races reported)"
exit 0
