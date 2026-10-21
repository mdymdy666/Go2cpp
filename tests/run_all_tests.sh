#!/usr/bin/env bash
# Go2Cpp 全量测试统一入口（可一键复现，不依赖 /tmp 固定构建目录）。
#
# 顺序：
#   1) ctest          —— 构建目录（默认 build-work）全部用例；
#   2) enhanced       —— 五能力增强矩阵（tests/enhanced/run_all.sh）；
#   3) scenarios      —— ctx_cancel_rollback / deadlock_routes / full_trace；
#   4) tools          —— migration（独立工程）/ cli / replay（单测 + 6 场景）。
#
# 设计：
# - 每步独立计时并写 TSV 汇总；某一步失败不会中断后续步骤（一次拿到全貌）；
# - 依赖缺失（未构建的目录）记 SKIP 并打印构建命令，不伪装成通过；
# - 最终任一 FAIL → exit 1；全部 SKIP（等于空跑）也 → exit 1。
#
# 环境变量：
#   BUILD_DIR       ctest 构建目录（默认 build-work → build-enhanced）
#   SCENARIO_BUILD  场景/demo 构建目录（默认 build-enhanced → build-doctor-full）
#   CLI_BIN         go2cpp-cli 路径（默认随场景构建目录解析）
#   MIG_DIR         tools/migration 构建目录（默认 build-migration）
#   REPLAY_DIR      tools/replay 构建目录（默认 build-replay）
#   JOBS            ctest 并行度（默认 nproc）
#   OUT_DIR         日志与汇总输出目录（默认 /tmp/go2cpp-run-all-<pid>）
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
# 3) 场景测试
# ---------------------------------------------------------------------------
SCENARIO_BUILD="$(pick "${SCENARIO_BUILD_DIR:-}" "$ROOT/build-enhanced" \
    "$ROOT/build-doctor-full")" || true
CLI_BIN_RESOLVED="$(pick "${CLI_BIN:-}" \
    "$SCENARIO_BUILD/tools/cli/go2cpp-cli" \
    "$ROOT/build-cli/tools/cli/go2cpp-cli" \
    "$ROOT/build-doctor-full/tools/cli/go2cpp-cli")" || true

if [ -x "$SCENARIO_BUILD/go2cpp_ctx_cancel_rollback" ]; then
    run_step "scenario-ctx-cancel" 600 \
        bash "$ROOT/tests/scenarios/ctx_cancel_rollback/run_ctx_cancel_rollback.sh" \
        "$SCENARIO_BUILD"
else
    mark_skip "scenario-ctx-cancel" 0 \
        "缺少 $SCENARIO_BUILD/go2cpp_ctx_cancel_rollback（-DGO2CPP_DIAGNOSTIC=ON）"
fi

if [ -x "$SCENARIO_BUILD/go2cpp_deadlock_routes" ] && [ -x "$CLI_BIN_RESOLVED" ]; then
    run_step "scenario-deadlock-routes" 900 \
        bash "$ROOT/tests/scenarios/deadlock_routes/run_deadlock_scenario.sh" \
        "$SCENARIO_BUILD" --clients 120 --load-ms 600
else
    mark_skip "scenario-deadlock-routes" 0 \
        "缺少 deadlock_routes 或 CLI（$SCENARIO_BUILD / $CLI_BIN_RESOLVED）"
fi

if [ -x "$SCENARIO_BUILD/go2cpp_full_trace_scenario" ] && [ -x "$CLI_BIN_RESOLVED" ]; then
    run_step "scenario-full-trace" 600 \
        env SCENARIO="$SCENARIO_BUILD/go2cpp_full_trace_scenario" \
        CLI="$CLI_BIN_RESOLVED" OUT="$OUT_DIR/full-trace" \
        bash "$ROOT/tests/scenarios/full_trace/run_full_trace.sh" 10 8
else
    mark_skip "scenario-full-trace" 0 \
        "缺少 full_trace 或 CLI（$SCENARIO_BUILD）"
fi

# ---------------------------------------------------------------------------
# 4) tools 测试
# ---------------------------------------------------------------------------
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
