#!/usr/bin/env bash
# Go2Cpp 全量测试统一入口（可一键复现，不依赖 /tmp 固定构建目录）。
#
# 顺序：
#   1) ctest          —— 构建目录（默认 build-work）全部用例；
#   2) enhanced       —— 五能力增强矩阵（tests/enhanced/run_all.sh）；
#   3) scenarios      —— ctest 已注册（仅增强构建）；ctest 未覆盖时独立跑兜底；
#   4) tools          —— 兜底：migration / cli / replay 已注册进 ctest；
#                        ctest 已跑过同一构建目录时默认跳过
#                        （RUN_TOOLS_STANDALONE=1 强制独立跑）。
#
# 设计：
# - 每步独立计时并写 TSV 汇总；某一步失败不会中断后续步骤（一次拿到全貌）；
# - 依赖缺失（未构建的目录）记 SKIP 并打印构建命令，不伪装成通过；
# - 最终任一 FAIL → exit 1；全部 SKIP（等于空跑）也 → exit 1。
#
# 环境变量：
#   BUILD_DIR       ctest 构建目录（默认 build-work → build-enhanced）
#   SCENARIO_BUILD  场景/demo 构建目录（默认 build-enhanced → build-doctor-full）
#   RUN_SCENARIOS_STANDALONE  1 = 即使 ctest 已覆盖也独立跑场景
#   CLI_BIN         go2cpp-cli 路径（默认随场景构建目录解析）
#   MIG_DIR         tools/migration 构建目录（默认 build-migration）
#   REPLAY_DIR      tools/replay 构建目录（默认 build-replay）
#   JOBS            ctest 并行度（默认 nproc）
#   OUT_DIR         日志与汇总输出目录（默认 /tmp/go2cpp-run-all-<pid>）
#   RUN_MEMORY_BENCH 1 = 额外跑内存基准小矩阵（默认 0，记 SKIP，不拖慢 CI）
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${OUT_DIR:-/tmp/go2cpp-run-all-$$}"
mkdir -p "$OUT_DIR"
SUMMARY="$OUT_DIR/summary.tsv"
printf 'step\tstatus\tseconds_ms\tdetail\n' > "$SUMMARY"

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
PASS=0
FAIL=0
SKIP=0
FAILED=()

# 输出第一个存在的候选；都不存在时返回第一个候选（便于报错）。
pick() {
    local first="" candidate
    for candidate in "$@"; do
        [ -n "$candidate" ] || continue
        [ -z "$first" ] && first="$candidate"
        if [ -e "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    printf '%s' "$first"
    return 1
}

log_tail() { # file [lines]
    tail -"${2:-1}" "$1" 2>/dev/null | tr '\n' ' ' | cut -c1-140
}

record() { # name status ms detail
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$SUMMARY"
    printf '%-34s %-5s %8s  %s\n' "$1" "$2" "$3" "$4"
}

mark_pass() { PASS=$((PASS + 1)); record "$1" PASS "$2" "$3"; }
mark_skip() { SKIP=$((SKIP + 1)); record "$1" SKIP "$2" "$3"; }
mark_fail() {
    FAIL=$((FAIL + 1))
    FAILED+=("$1")
    record "$1" FAIL "$2" "$3"
}

# run_step <name> <timeout> <外部命令...>
run_step() {
    local name="$1" limit="$2"
    shift 2
    local start end ms code
    start=$(date +%s%N)
    timeout "$limit" "$@" >"$OUT_DIR/$name.log" 2>&1
    code=$?
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    if [ "$code" -eq 0 ]; then
        mark_pass "$name" "$ms" "$(log_tail "$OUT_DIR/$name.log")"
    elif [ "$code" -eq 77 ]; then
        mark_skip "$name" "$ms" "$(log_tail "$OUT_DIR/$name.log")"
    else
        mark_fail "$name" "$ms" \
            "exit=$code log=$OUT_DIR/$name.log $(log_tail "$OUT_DIR/$name.log")"
    fi
}

# ---------------------------------------------------------------------------
# 1) ctest
# ---------------------------------------------------------------------------
CTEST_BUILD="$(pick "${BUILD_DIR:-}" "$ROOT/build-work" "$ROOT/build-enhanced")" || true
CTEST_TOTAL=""
if [ -f "$CTEST_BUILD/CTestTestfile.cmake" ]; then
    CTEST_TOTAL="$(ctest --test-dir "$CTEST_BUILD" -N 2>/dev/null |
        sed -n 's/^Total Tests: //p' | head -1)"
    run_step "ctest" 1800 \
        ctest --test-dir "$CTEST_BUILD" --output-on-failure -j "$JOBS"
    echo "ctest: ${CTEST_TOTAL:-?} 条，构建目录 $CTEST_BUILD（日志 $OUT_DIR/ctest.log）"
else
    mark_skip "ctest" 0 \
        "缺少 $CTEST_BUILD/CTestTestfile.cmake（cmake -S . -B build-work -DGO2CPP_BUILD_TESTS=ON）"
fi


# tools 三套脚本已注册进 ctest（cmake/Go2CppTests.cmake 的 tools-migration /
# tools-cli / tools-replay）。ctest 步骤已实跑含这三条用例的构建目录时，
# 第 4 步默认不再重复执行，只记 SKIP 标注覆盖关系；
# RUN_TOOLS_STANDALONE=1 可强制独立跑。
TOOLS_IN_CTEST=0
if [ -f "$CTEST_BUILD/CTestTestfile.cmake" ] &&
    grep -q "tools-migration" "$CTEST_BUILD/CTestTestfile.cmake" 2>/dev/null; then
    TOOLS_IN_CTEST=1
fi

# ---------------------------------------------------------------------------
# 2) enhanced 五能力矩阵
# ---------------------------------------------------------------------------
if [ -f "$ROOT/tests/enhanced/run_all.sh" ]; then
    start=$(date +%s%N)
    timeout 1800 env \
        MATRIX_FILE="$OUT_DIR/enhanced_matrix.tsv" \
        STATS_FILE="$OUT_DIR/enhanced_stats.tsv" \
        LOG_DIR="$OUT_DIR/enhanced_logs" \
        bash "$ROOT/tests/enhanced/run_all.sh" >"$OUT_DIR/enhanced.log" 2>&1
    code=$?
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    matrix_fails=0
    if [ -f "$OUT_DIR/enhanced_matrix.tsv" ]; then
        matrix_fails=$(awk -F'\t' '$4 == "FAIL"' "$OUT_DIR/enhanced_matrix.tsv" |
            wc -l)
    fi
    if [ "$code" -eq 77 ]; then
        mark_skip "enhanced" "$ms" "$(log_tail "$OUT_DIR/enhanced.log" 2)"
    elif [ "$code" -ne 0 ] || [ "$matrix_fails" -gt 0 ]; then
        mark_fail "enhanced" "$ms" \
            "exit=$code product_fail=$matrix_fails log=$OUT_DIR/enhanced.log"
    else
        mark_pass "enhanced" "$ms" "$(log_tail "$OUT_DIR/enhanced.log" 2)"
    fi
else
    mark_skip "enhanced" 0 "缺少 tests/enhanced/run_all.sh"
fi

# ---------------------------------------------------------------------------
# 3) 场景测试（ctest 兜底）
# ---------------------------------------------------------------------------
# 四条场景已注册进 ctest（仅增强构建）：scenario-ctx-cancel /
# scenario-deadlock-routes / scenario-full-trace / scenario-channel-deadlock。
# ctest 步骤若已跑过包含这四条的构建目录（例如 BUILD_DIR=build-enhanced），
# 本节默认只记 SKIP 标注覆盖关系，避免同一场景跑两遍；
# RUN_SCENARIOS_STANDALONE=1 强制独立跑。
SCENARIO_BUILD="$(pick "${SCENARIO_BUILD_DIR:-}" "$ROOT/build-enhanced" \
    "$ROOT/build-doctor-full")" || true
CLI_BIN_RESOLVED="$(pick "${CLI_BIN:-}" \
    "$SCENARIO_BUILD/tools/cli/go2cpp-cli" \
    "$ROOT/build-cli/tools/cli/go2cpp-cli" \
    "$ROOT/build-doctor-full/tools/cli/go2cpp-cli")" || true

SCENARIOS_IN_CTEST=0
if [ -f "$CTEST_BUILD/CTestTestfile.cmake" ] &&
    grep -q "scenario-deadlock-routes" "$CTEST_BUILD/CTestTestfile.cmake" 2>/dev/null; then
    SCENARIOS_IN_CTEST=1
fi

run_scenario() { # <name> <timeout> <包装脚本>
    if [ -d "$SCENARIO_BUILD" ]; then
        run_step "$1" "$2" env GO2CPP_SCENARIO_BUILD_DIR="$SCENARIO_BUILD" \
            bash "$3"
    else
        run_step "$1" "$2" bash "$3"
    fi
}

if [ "$SCENARIOS_IN_CTEST" -eq 1 ] && [ "${RUN_SCENARIOS_STANDALONE:-0}" != "1" ]; then
    mark_skip "scenario-ctx-cancel" 0 \
        "已由 ctest scenario-ctx-cancel 覆盖（RUN_SCENARIOS_STANDALONE=1 强制独立跑）"
    mark_skip "scenario-deadlock-routes" 0 \
        "已由 ctest scenario-deadlock-routes 覆盖"
    mark_skip "scenario-full-trace" 0 \
        "已由 ctest scenario-full-trace 覆盖"
    mark_skip "scenario-channel-deadlock" 0 \
        "已由 ctest scenario-channel-deadlock 覆盖"
else
    # 包装脚本自己解析构建目录（GO2CPP_SCENARIO_BUILD_DIR > 当前构建 >
    # build-doctor-full > ...）；缺依赖时 exit 77 → 本入口记为 SKIP。
    run_scenario "scenario-ctx-cancel" 300 \
        "$ROOT/tests/scenarios/run_ctx_cancel_ctest.sh"
    run_scenario "scenario-deadlock-routes" 600 \
        "$ROOT/tests/scenarios/run_deadlock_routes_ctest.sh"
    run_scenario "scenario-full-trace" 300 \
        "$ROOT/tests/scenarios/run_full_trace_ctest.sh"
    run_scenario "scenario-channel-deadlock" 300 \
        "$ROOT/tests/scenarios/channel_deadlock/run_channel_deadlock.sh"
fi

# ---------------------------------------------------------------------------
# 4) tools 测试（ctest 已覆盖时只做兜底）
# ---------------------------------------------------------------------------
if [ "$TOOLS_IN_CTEST" -eq 1 ] && [ "${RUN_TOOLS_STANDALONE:-0}" != "1" ]; then
    mark_skip "tools-migration" 0 "已由 ctest tools-migration 覆盖（RUN_TOOLS_STANDALONE=1 强制独立跑）"
    mark_skip "tools-cli" 0 "已由 ctest tools-cli 覆盖"
    mark_skip "tools-replay-unit" 0 "已由 ctest tools-replay 覆盖"
    mark_skip "tools-replay-scenarios" 0 "已由 ctest tools-replay 覆盖"
else
MIG_DIR_RESOLVED="$(pick "${MIG_DIR:-}" "$ROOT/build-migration")" || true
if [ -x "$MIG_DIR_RESOLVED/migrate" ]; then
    run_step "tools-migration" 900 \
        bash "$ROOT/tools/migration/tests/run_tests.sh" "$MIG_DIR_RESOLVED"
else
    mark_skip "tools-migration" 0 \
        "缺少 $MIG_DIR_RESOLVED/migrate（cmake -S tools/migration -B build-migration && cmake --build build-migration -j）"
fi

if [ -x "$CLI_BIN_RESOLVED" ]; then
    run_step "tools-cli" 900 \
        env BIN="$CLI_BIN_RESOLVED" bash "$ROOT/tools/cli/tests/run_all.sh"
else
    mark_skip "tools-cli" 0 "缺少 CLI（$CLI_BIN_RESOLVED）"
fi

REPLAY_DIR_RESOLVED="$(pick "${REPLAY_DIR:-}" "$ROOT/build-replay" \
    "$ROOT/build-enhanced")" || true
if [ -x "$REPLAY_DIR_RESOLVED/go2cpp_replay_tests" ]; then
    run_step "tools-replay-unit" 300 "$REPLAY_DIR_RESOLVED/go2cpp_replay_tests"
else
    mark_skip "tools-replay-unit" 0 \
        "缺少 $REPLAY_DIR_RESOLVED/go2cpp_replay_tests（tools/replay/build.sh）"
fi
if [ -x "$REPLAY_DIR_RESOLVED/replay" ]; then
    run_step "tools-replay-scenarios" 900 \
        bash "$ROOT/tools/replay/scripts/run_all.sh" "$REPLAY_DIR_RESOLVED"
else
    mark_skip "tools-replay-scenarios" 0 \
        "缺少 $REPLAY_DIR_RESOLVED/replay（tools/replay/build.sh）"
fi

fi

# ---------------------------------------------------------------------------
# 5) 内存基准（OPTIONAL）—— 默认 SKIP，不拖慢常规 CI。
#
# 为什么默认不跑：完整矩阵（5 模式 × 3 栈 × 3 数量）会创建最多 5000 个线程/
# 协程并触页数 GB，耗时数分钟且受机器内存影响；常规回归只需要功能测试通过。
# RUN_MEMORY_BENCH=1 时跑 QUICK 小矩阵（2 模式 × 64KB × 100/1000），约 10 秒。
# ---------------------------------------------------------------------------
if [ "${RUN_MEMORY_BENCH:-0}" = "1" ]; then
    run_step "bench-memory" 600 \
        env QUICK=1 OUT_DIR="$OUT_DIR/bench-memory" \
        bash "$ROOT/tests/bench/run_memory_bench.sh"
else
    mark_skip "bench-memory" 0 \
        "OPTIONAL：RUN_MEMORY_BENCH=1 运行（QUICK 小矩阵，约 10s）"
fi

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
printf '\n===== Go2Cpp 全量测试汇总 =====\n'
{
    printf '%-34s %-5s %8s  %s\n' STEP STATUS MS DETAIL
    awk -F'\t' 'NR > 1 {
        printf "%-34s %-5s %8s  %s\n", $1, $2, $3, $4
    }' "$SUMMARY"
} | sed 's/[[:space:]]*$//'
printf '\n'
printf 'ctest 用例数: %s\n' "${CTEST_TOTAL:-?}"
printf 'PASS=%d FAIL=%d SKIP=%d\n' "$PASS" "$FAIL" "$SKIP"
printf '汇总 TSV: %s\n' "$SUMMARY"

if [ "$FAIL" -ne 0 ]; then
    printf 'FAILED STEPS: %s\n' "${FAILED[*]}" >&2
    exit 1
fi
if [ "$PASS" -eq 0 ]; then
    echo "全部步骤 SKIP：没有任何测试真正执行（拒绝空跑当通过）" >&2
    exit 1
fi
exit 0
