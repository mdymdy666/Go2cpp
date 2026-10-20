#!/usr/bin/env bash
# 场景验收：锁顺序死锁 → Doctor `ctx.cancel` → ContextRollback undo 解死锁。
#
# 三个子场景：
#   1) --mode=cancel   进程内 Doctor 客户端：deadlock-report → ctx.cancel <A>；
#                      断言 A 的 Lock 返回 false、ctx.Err 非空、undo 释放 L1、
#                      B 拿到 L1 完成、锁收支平衡、无泄漏。
#   2) --mode=force    对照组：mutex-unlock --force；断言 stale-unlock
#                      （undo 未执行、A 失去 L1 后跳过 Unlock、锁仍平衡）。
#   3) --mode=external 真实 Unix socket + doctor_client.py：
#                      deadlock-report、ctx.cancel 的拒绝路径与成功路径。
#
# 用法：bash tests/scenarios/ctx_cancel_rollback/run_ctx_cancel_rollback.sh [build-dir]
# 依赖：build-dir 里已构建 go2cpp_ctx_cancel_rollback（GO2CPP_DIAGNOSTIC=ON
#       且 GO2CPP_DEBUG_DUMP=ON），以及 python3。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build-doctor-full}"
DEMO="${BUILD_DIR}/go2cpp_ctx_cancel_rollback"
CLIENT="${ROOT}/tests/debug/doctor_client.py"
TAG="$$-$(date +%s)"
LOG_DIR="/tmp/go2cpp-ctx-cancel-${TAG}"
PASS=0
FAIL=0
DEMO_PID=""

pass() { echo "PASS $*"; PASS=$((PASS + 1)); }
fail() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

cleanup() {
    if [ -n "${DEMO_PID}" ] && kill -0 "${DEMO_PID}" 2>/dev/null; then
        kill "${DEMO_PID}" 2>/dev/null || true
        wait "${DEMO_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

if [ ! -x "${DEMO}" ]; then
    echo "SKIP: ${DEMO} 不存在（先构建 -DGO2CPP_DIAGNOSTIC=ON -DGO2CPP_DEBUG_DUMP=ON）"
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: 需要 python3"
    exit 77
fi
mkdir -p "${LOG_DIR}"

expect_result() {  # <name> <result-line> <key=value>...
    local name="$1"
    local line="$2"
    shift 2
    local key
    for key in "$@"; do
        case "${line}" in
            *"${key}"*) pass "${name}: ${key}" ;;
            *) fail "${name}: 缺 ${key}" ;;
        esac
    done
}

expect_contains() {  # <name> <file> <needle>
    if grep -q -- "$3" "$2"; then
        pass "$1"
    else
        fail "$1（缺 $(printf '%q' "$3")）"
    fi
}

# ---------------------------------------------------------------------------
# 1) cancel 模式：Doctor 只取消，A 回滚释放 L1，B 继续完成
# ---------------------------------------------------------------------------
echo "=== cancel 模式 ==="
"${DEMO}" --mode=cancel --socket "${LOG_DIR}/cancel.sock" \
    >"${LOG_DIR}/cancel.out" 2>"${LOG_DIR}/cancel.err"
CANCEL_EXIT=$?
if [ "${CANCEL_EXIT}" -eq 0 ]; then
    pass "cancel 模式退出码 0"
else
    fail "cancel 模式退出码 ${CANCEL_EXIT}"
fi
CANCEL_RESULT="$(grep -m1 '^RESULT ' "${LOG_DIR}/cancel.out" || true)"
echo "  ${CANCEL_RESULT}"
expect_result "cancel" "${CANCEL_RESULT}" \
    "pass=1" "a_completed=1" "b_completed=1" "a_lock2_false=1" \
    "ctx_err=1" "undo_executed=1" "rollback_status=rolled_back" \
    "pending_undo=0" "rollback_failures=0" "stale=0" \
    "locks_acquired=3" "locks_released=3" "balanced=1" \
    "l1_held=0" "l1_waiters=0" "l2_held=0" "l2_waiters=0" \
    "shared_work=1"
expect_contains "cancel: A 的 Lock(L2) 返回 false 且 ctx.Err 非空" \
    "${LOG_DIR}/cancel.err" "\[A\] Lock(L2)=false ctx.Err=1"
expect_contains "cancel: undo 执行（L1 由回滚释放）" \
    "${LOG_DIR}/cancel.err" "\[undo\] A: L1 released by ContextRollback undo"
expect_contains "cancel: B 在 A 回滚后拿到 L1 并完成" \
    "${LOG_DIR}/cancel.err" "\[B\] COMPLETED"
expect_contains "cancel: Doctor 审计行（谁/何时/哪个 ctx）" \
    "${LOG_DIR}/cancel.err" "\[go2cpp\]\[doctor\]\[audit\] ctx.cancel name=worker-A"
expect_contains "cancel: deadlock-report verdict=deadlock" \
    "${LOG_DIR}/cancel.err" "^verdict=deadlock"
expect_contains "cancel: 等待图 owner_alive=true（有主的锁）" \
    "${LOG_DIR}/cancel.err" "owner_alive=true"

# ---------------------------------------------------------------------------
# 2) force 对照：stale-unlock（强制解锁不通知 owner）
# ---------------------------------------------------------------------------
echo "=== force 对照模式 ==="
"${DEMO}" --mode=force --socket "${LOG_DIR}/force.sock" \
    >"${LOG_DIR}/force.out" 2>"${LOG_DIR}/force.err"
FORCE_EXIT=$?
if [ "${FORCE_EXIT}" -eq 0 ]; then
    pass "force 对照退出码 0"
else
    fail "force 对照退出码 ${FORCE_EXIT}"
fi
FORCE_RESULT="$(grep -m1 '^RESULT ' "${LOG_DIR}/force.out" || true)"
echo "  ${FORCE_RESULT}"
expect_result "force" "${FORCE_RESULT}" \
    "pass=1" "stale=1" "undo_executed=0" "rollback_status=committed" \
    "a_still_owns_l1=0" "locks_acquired=4" "locks_released=3" \
    "force_unlocks=1" "balanced=1" "l1_held=0" "l2_held=0" "shared_work=1"
expect_contains "force: A 发现 L1 已被夺走（stale）" \
    "${LOG_DIR}/force.err" "\[A\] STALE: L1 no longer owned"
expect_contains "force: Doctor 审计（forced_unlock）" \
    "${LOG_DIR}/force.err" "mutex-unlock --force name=L1"

# ---------------------------------------------------------------------------
# 3) external 模式：真实 socket + 真实验收拒绝/成功路径
# ---------------------------------------------------------------------------
echo "=== external 模式（真实 socket）==="
SOCK="${LOG_DIR}/external.sock"
"${DEMO}" --mode=external --socket "${SOCK}" \
    >"${LOG_DIR}/external.out" 2>"${LOG_DIR}/external.err" &
DEMO_PID=$!

GID=""
for _ in $(seq 1 400); do
    if grep -q '^DEADLOCK_READY ' "${LOG_DIR}/external.out" 2>/dev/null; then
        GID="$(grep -m1 '^DEADLOCK_READY ' "${LOG_DIR}/external.out" |
            sed -n 's/.*gid_a=\([0-9][0-9]*\).*/\1/p')"
        break
    fi
    kill -0 "${DEMO_PID}" 2>/dev/null || break
    sleep 0.05
done
if [ -n "${GID}" ]; then
    pass "external DEADLOCK_READY gid_a=${GID}"
else
    fail "external 未进入 DEADLOCK_READY：$(tail -3 "${LOG_DIR}/external.out" 2>/dev/null)"
fi

if [ -n "${GID}" ]; then
    REPORT=""
    for _ in $(seq 1 100); do
        REPORT="$(python3 "${CLIENT}" "${SOCK}" deadlock-report 2>&1 || true)"
        case "${REPORT}" in
            *"verdict=deadlock"*) break ;;
        esac
        sleep 0.1
    done
    case "${REPORT}" in
        *"verdict=deadlock"*) pass "external deadlock-report verdict=deadlock" ;;
        *) fail "external deadlock-report 未判定 deadlock：$(printf '%s' "${REPORT}" | head -3)" ;;
    esac
    case "${REPORT}" in
        *"owner_alive=true"*) pass "external 等待图 owner_alive=true" ;;
        *) fail "external 等待图缺 owner_alive=true" ;;
    esac

    run_reject() {  # <name> <command> <needle>
        local name="$1"
        local command="$2"
        local needle="$3"
        local out code
        out="$(python3 "${CLIENT}" "${SOCK}" "${command}" 2>&1)"
        code=$?
        if [ "${code}" -ne 0 ] && printf '%s' "${out}" | grep -q -- "${needle}"; then
            pass "${name}（退出码 ${code}）"
        else
            fail "${name}：code=${code} out=$(printf '%s' "${out}" | head -2)"
        fi
    }
    run_reject "拒绝: 未注册 gid" "ctx.cancel gid=999999" "not-registered"
    run_reject "拒绝: 非法 gid" "ctx.cancel gid=abc" "bad-argument"
    run_reject "拒绝: 未注册地址" "ctx.cancel --address 0x1" "not-registered"
    run_reject "拒绝: 缺少定位参数" "ctx.cancel" "bad-argument"

    CANCEL_OUT="$(python3 "${CLIENT}" "${SOCK}" "ctx.cancel gid=${GID}" 2>&1)"
    CANCEL_CODE=$?
    if [ "${CANCEL_CODE}" -eq 0 ]; then
        pass "external ctx.cancel gid=${GID} 成功（退出码 0）"
    else
        fail "external ctx.cancel 退出码 ${CANCEL_CODE}"
    fi
    case "${CANCEL_OUT}" in
        *"verdict=cancelled"*) pass "external ctx.cancel verdict=cancelled" ;;
        *) fail "external ctx.cancel 缺 verdict=cancelled" ;;
    esac
    case "${CANCEL_OUT}" in
        *"context.matched_by=gid"*) pass "external ctx.cancel matched_by=gid" ;;
        *) fail "external ctx.cancel 缺 matched_by=gid" ;;
    esac
    case "${CANCEL_OUT}" in
        *"audit.actor=doctor"*) pass "external ctx.cancel 审计字段完整" ;;
        *) fail "external ctx.cancel 缺审计字段" ;;
    esac

    for _ in $(seq 1 400); do
        kill -0 "${DEMO_PID}" 2>/dev/null || break
        sleep 0.05
    done
    wait "${DEMO_PID}"
    DEMO_EXIT=$?
    DEMO_PID=""
    if [ "${DEMO_EXIT}" -eq 0 ]; then
        pass "external 进程退出码 0"
    else
        fail "external 进程退出码 ${DEMO_EXIT}"
    fi
    EXTERNAL_RESULT="$(grep -m1 '^RESULT ' "${LOG_DIR}/external.out" || true)"
    echo "  ${EXTERNAL_RESULT}"
    expect_result "external" "${EXTERNAL_RESULT}" \
        "pass=1" "a_completed=1" "b_completed=1" "a_lock2_false=1" \
        "ctx_err=1" "undo_executed=1" "balanced=1" "force_unlocks=0" \
        "l1_held=0" "l2_held=0" "shared_work=1"
    expect_contains "external: undo 执行（L1 由回滚释放）" \
        "${LOG_DIR}/external.err" "\[undo\] A: L1 released by ContextRollback undo"
    expect_contains "external: Doctor 审计行" \
        "${LOG_DIR}/external.err" "\[go2cpp\]\[doctor\]\[audit\] ctx.cancel name=worker-A"
fi

echo "PASS=${PASS} FAIL=${FAIL} logs=${LOG_DIR}"
if [ "${FAIL}" -ne 0 ]; then
    exit 1
fi
exit 0
