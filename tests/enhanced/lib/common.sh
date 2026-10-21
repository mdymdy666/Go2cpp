#!/usr/bin/env bash
# Go2Cpp 五能力增强测试的共用库：断言、矩阵记录、真实进程夹具。
#
# 设计约束：
# - 测试失败不得中断整个脚本（不用 set -e）；
# - 每个用例都记录到 MATRIX_FILE（TSV），由 report.py 汇总；
# - 所有外部进程都带 timeout，避免偶发卡死拖垮一键测试；
# - 只读仓库核心代码，不修改核心源码。
set -u

# ---------------------------------------------------------------------------
# 构建产物定位（不依赖 /tmp 固定路径）
#
# 顺序：显式环境变量（GO2CPP_CLI / GO2CPP_REPLAY_DIR / ...）优先，其次按
# 「一键 enhanced 构建 → 各能力专项构建 → 默认构建」探测工作树内的构建目录。
# 换机器、清理 /tmp、或只构建了某个能力目录时仍可复现；全部候选都不存在时
# 返回第一个候选，让调用方报错时能看到期望路径。
# ---------------------------------------------------------------------------

# find_exe <候选...>：输出第一个可执行文件。
find_exe() {
    local first="" candidate
    for candidate in "$@"; do
        [ -n "$candidate" ] || continue
        [ -z "$first" ] && first="$candidate"
        if [ -x "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    printf '%s' "$first"
    return 1
}

# find_build_dir <目录内标记文件> <候选目录...>：输出第一个含标记文件的目录。
find_build_dir() {
    local marker="$1" first="" candidate
    shift
    for candidate in "$@"; do
        [ -n "$candidate" ] || continue
        [ -z "$first" ] && first="$candidate"
        if [ -x "$candidate/$marker" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    printf '%s' "$first"
    return 1
}

common_init() {
    ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
    # 一键增强构建（GO2CPP_BUILD_ENHANCED=ON）优先：DEBUG_DUMP/DIAGNOSTIC/
    # REPLAY/MIGRATION_API 同时打开时，debug demo / doctor demo / replay /
    # CLI 都在这一个目录里。
    local enhanced_build="$ROOT/build-enhanced"
    CLI="$(find_exe "${GO2CPP_CLI:-}" \
        "$enhanced_build/tools/cli/go2cpp-cli" \
        "$ROOT/build-cli/tools/cli/go2cpp-cli" \
        "$ROOT/build-doctor-full/tools/cli/go2cpp-cli")" || true
    MIGRATE="${GO2CPP_MIGRATE:-$ROOT/build-migration/migrate}"
    MIGRATE_ALT="${GO2CPP_MIGRATE_ALT:-$ROOT/build-migration/migrate_alt}"
    MIGRATE_CONFLICT="${GO2CPP_MIGRATE_CONFLICT:-$ROOT/build-migration/migrate_conflict}"
    MIG_DIR="${GO2CPP_MIG_DIR:-$ROOT/build-migration}"
    REPLAY_DIR="$(find_build_dir replay "${GO2CPP_REPLAY_DIR:-}" \
        "$enhanced_build" "$ROOT/build-replay")" || true
    REPLAY_BIN="${GO2CPP_REPLAY_BIN:-$REPLAY_DIR/replay}"
    RECORD_BIN="${GO2CPP_RECORD_BIN:-$REPLAY_DIR/go2cpp_replay_record}"
    SELECT_BIN="${GO2CPP_SELECT_BIN:-$REPLAY_DIR/go2cpp_replay_select}"
    CONTEXT_BIN="${GO2CPP_CONTEXT_BIN:-$REPLAY_DIR/go2cpp_replay_context}"
    VALUES_BIN="${GO2CPP_VALUES_BIN:-$REPLAY_DIR/go2cpp_replay_values}"
    # debug demo：build-debug 是审计基线（仅 DEBUG_DUMP，3 组件）；
    # build-enhanced / build-*-full 会多注册组件，test_debug.sh 已按
    # 「>=3 个」断言，因此这些构建也能跑。
    DEBUG_DEMO="$(find_exe "${GO2CPP_DEBUG_DEMO:-}" \
        "$ROOT/build-debug/go2cpp_debug_subscribe_demo" \
        "$enhanced_build/go2cpp_debug_subscribe_demo" \
        "$ROOT/build-debug-full/go2cpp_debug_subscribe_demo" \
        "$ROOT/build-doctor-full/go2cpp_debug_subscribe_demo")" || true
    DIAG_DEMO="$(find_exe "${GO2CPP_DIAG_DEMO:-}" \
        "$enhanced_build/go2cpp_diagnostic_demo" \
        "$ROOT/build-doctor-full/go2cpp_diagnostic_demo" \
        "$ROOT/build-diag/go2cpp_diagnostic_demo")" || true
    MATRIX_FILE="${MATRIX_FILE:-/tmp/test_enhance_matrix.tsv}"
    STATS_FILE="${STATS_FILE:-/tmp/test_enhance_stats.tsv}"
    PASS=0
    FAIL=0
    SKIP=0
    GAP=0
    FAILED=()
    LAST_OUTPUT=""
    CASE_TIMEOUT="${CASE_TIMEOUT:-20}"

    mkdir -p "$(dirname "$MATRIX_FILE")"
    if [ ! -f "$MATRIX_FILE" ]; then
        printf 'capability\tcase\tkind\tstatus\tdetail\n' > "$MATRIX_FILE"
    fi
    if [ ! -f "$STATS_FILE" ]; then
        printf 'capability\tname\tvalue\tdetail\n' > "$STATS_FILE"
    fi
    # 固定测试密钥（不是生产密钥）。
    export MIGRATION_SIGNING_KEY="${MIGRATION_SIGNING_KEY:-hex:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff}"
}

_sanitize() {
    printf '%s' "$1" | tr '\t\n\r' '   ' | cut -c1-360
}

record() { # case kind status detail
    printf '%s\t%s\t%s\t%s\t%s\n' "${CAPABILITY:-unknown}" "$1" "$2" "$3" "$(_sanitize "${4:-}")" \
        >> "$MATRIX_FILE"
}

pass() { # case [kind] [detail]
    PASS=$((PASS + 1))
    record "$1" "${2:-normal}" PASS "${3:-}"
}

fail() { # case [kind] [detail]
    FAIL=$((FAIL + 1))
    FAILED+=("$1")
    record "$1" "${2:-normal}" FAIL "${3:-}"
}

skip() { # case [kind] [detail]
    SKIP=$((SKIP + 1))
    record "$1" "${2:-normal}" SKIP "${3:-}"
}

# 能力缺口：不是通过，也不是失败；单独计数并写入矩阵（供 P2 缺陷清单）。
gap() { # case [kind] [detail]
    GAP=$((GAP + 1))
    record "$1" "${2:-normal}" GAP "${3:-}"
}

add_stat() { # name value detail
    printf '%s\t%s\t%s\t%s\n' "${CAPABILITY:-unknown}" "$1" "$2" "$(_sanitize "${3:-}")" \
        >> "$STATS_FILE"
}

expect_ok() { # name kind pattern cmd...
    local name="$1" kind="$2" pattern="$3"
    shift 3
    local out code
    out="$(timeout "$CASE_TIMEOUT" "$@" 2>&1)"
    code=$?
    LAST_OUTPUT="$out"
    if [ "$code" -eq 0 ] && printf '%s' "$out" | grep -qF -- "$pattern"; then
        pass "$name" "$kind"
    else
        fail "$name" "$kind" "exit=$code missing='$pattern' head=$(printf '%s' "$out" | head -3 | tr '\n' '|')"
    fi
}

expect_fail() { # name kind pattern cmd...
    local name="$1" kind="$2" pattern="$3"
    shift 3
    local out code
    out="$(timeout "$CASE_TIMEOUT" "$@" 2>&1)"
    code=$?
    LAST_OUTPUT="$out"
    if [ "$code" -ne 0 ] && printf '%s' "$out" | grep -qF -- "$pattern"; then
        pass "$name" "$kind"
    else
        fail "$name" "$kind" "exit=$code (want nonzero) missing='$pattern' head=$(printf '%s' "$out" | head -3 | tr '\n' '|')"
    fi
}

run_expect() { # name expected kind cmd...
    local name="$1" expected="$2" kind="$3"
    shift 3
    local out code
    out="$(timeout "$CASE_TIMEOUT" "$@" 2>&1)"
    code=$?
    LAST_OUTPUT="$out"
    if [ "$code" -eq "$expected" ]; then
        pass "$name" "$kind"
    else
        fail "$name" "$kind" "exit=$code want=$expected head=$(printf '%s' "$out" | head -3 | tr '\n' '|')"
    fi
}

expect_contains() { # name kind needle output
    local name="$1" kind="$2" needle="$3" output="$4"
    if printf '%s' "$output" | grep -qF -- "$needle"; then
        pass "$name" "$kind"
    else
        fail "$name" "$kind" "missing='$needle' head=$(printf '%s' "$output" | head -3 | tr '\n' '|')"
    fi
}

expect_contains_re() { # name kind regex output
    local name="$1" kind="$2" pattern="$3" output="$4"
    if printf '%s' "$output" | grep -qE -- "$pattern"; then
        pass "$name" "$kind"
    else
        fail "$name" "$kind" "regex miss='$pattern' head=$(printf '%s' "$output" | head -3 | tr '\n' '|')"
    fi
}

check_true() { # name kind condition-desc result(0/1) [detail]
    local name="$1" kind="$2" desc="$3" result="$4" detail="${5:-}"
    if [ "$result" -eq 0 ]; then
        pass "$name" "$kind"
    else
        fail "$name" "$kind" "$desc ${detail}"
    fi
}

make_work() { # prefix
    mktemp -d "/tmp/go2cpp-enhanced-${1:-work}.XXXXXX"
}

wait_for_path() { # path timeout_ms
    local path="$1" deadline=$(( $(date +%s%N) / 1000000 + ${2:-5000} ))
    while [ $(( $(date +%s%N) / 1000000 )) -lt "$deadline" ]; do
        [ -e "$path" ] && return 0
        sleep 0.02
    done
    return 1
}

wait_for_line() { # file pattern timeout_ms
    local file="$1" pattern="$2" deadline=$(( $(date +%s%N) / 1000000 + ${3:-5000} ))
    while [ $(( $(date +%s%N) / 1000000 )) -lt "$deadline" ]; do
        if [ -f "$file" ] && grep -qF -- "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.02
    done
    return 1
}

stop_pid() { # pid
    local pid="${1:-}"
    [ -n "$pid" ] || return 0
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.02
        done
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

start_debug_server() { # [seconds] ; sets DEBUG_PID DEBUG_SOCK DEBUG_LOG
    local seconds="${1:-60}"
    DEBUG_SOCK="$WORK/debug-server-$$.sock"
    DEBUG_LOG="$WORK/debug-server.log"
    rm -f "$DEBUG_SOCK"
    # 用 exec -a 改进程名：共享工作区里其它 agent 可能 pkill 同名 demo，
    # 不隔离会把外部清理误判成被测服务崩溃。
    ( exec -a "g2c-dbg-fixture-$$-$RANDOM" "$DEBUG_DEMO" "$DEBUG_SOCK" "$seconds" ) \
        >"$DEBUG_LOG" 2>&1 &
    DEBUG_PID=$!
    if ! wait_for_path "$DEBUG_SOCK" 5000; then
        return 1
    fi
    return 0
}

start_doctor_server() { # [timeout_seconds] ; sets DIAG_PID DIAG_SOCK DIAG_LOG
    local seconds="${1:-120}"
    DIAG_SOCK="$WORK/doctor-$$.sock"
    DIAG_LOG="$WORK/doctor.log"
    rm -f "$DIAG_SOCK"
    ( exec -a "g2c-doctor-fixture-$$-$RANDOM" "$DIAG_DEMO" "$DIAG_SOCK" ) \
        >"$DIAG_LOG" 2>&1 &
    DIAG_PID=$!
    if ! wait_for_line "$DIAG_LOG" "READY" 8000; then
        return 1
    fi
    return 0
}

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

summary() {
    echo
    echo "==== ${CAPABILITY:-unknown}: PASS=$PASS FAIL=$FAIL SKIP=$SKIP GAP=$GAP ===="
    if [ "$FAIL" -gt 0 ]; then
        printf 'FAILED: %s\n' "${FAILED[*]}"
        return 1
    fi
    return 0
}
