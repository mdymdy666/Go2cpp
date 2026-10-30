#!/usr/bin/env bash
# 场景验收：channel 互等死锁 → select 语义恢复（不依赖 Doctor）。
#
# 四个子场景：
#   1) baseline 纯阻塞：A <-chX、B <-chY 互等；进程永久卡住，作为死锁对照；
#      脚本确认 stuck=2、1.5s 内无进展，然后 SIGTERM 结束。
#   2) timeout    ：select { case <-ch: ...; case <-time.After(200ms): ... }；
#      双方超时退出，stuck_after=0，记录超时耗时。
#   3) multi      ：select { case <-自己的入站: ...; case 对端的入站 <- token: ... }；
#      两个 select 的 send/recv 配对，双方完成，不互等。
#   4) default    ：select { case <-ch: ...; default: 降级 }；
#      非阻塞尝试记录 no_ready，然后走降级路径，不 park。
#
# 另外用 --repro N（默认 10）跑 timeout / multi / default 的重复率。
#
# 用法：
#   bash tests/scenarios/channel_deadlock/run_channel_deadlock.sh [BUILD_DIR] \
#        [--repro N] [--quick] [--ctest]
#
# 构建目录解析顺序：
#   BUILD_DIR 参数 > GO2CPP_SCENARIO_BUILD_DIR > 当前目录（含 CMakeCache.txt）
#   > build-doctor-full > build-enhanced > build-debug-full > build-work
# ctest 模式下缺 demo 可执行文件按**失败**上报（FAIL，exit 1）；
# 只有手工运行且未构建时才 exit 77（环境缺件，SKIP）。
#
# ctest 包装：cmake 以 `--ctest` 调用；此时当前工作目录就是构建目录。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR=""
REPRO=10
CTEST_MODE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --ctest) CTEST_MODE=1; shift ;;
        --repro) REPRO="$2"; shift 2 ;;
        --repro=*) REPRO="${1#*=}"; shift ;;
        --quick) REPRO=1; shift ;;
        -*) echo "未知选项 $1" >&2; exit 2 ;;
        *) BUILD_DIR="$1"; shift ;;
    esac
done

if [ -z "${BUILD_DIR}" ]; then
    if [ -n "${GO2CPP_SCENARIO_BUILD_DIR:-}" ]; then
        BUILD_DIR="${GO2CPP_SCENARIO_BUILD_DIR}"
    elif [ -f "${PWD}/CMakeCache.txt" ]; then
        BUILD_DIR="${PWD}"
    else
        for candidate in \
            "${ROOT}/build-doctor-full" \
            "${ROOT}/build-enhanced" \
            "${ROOT}/build-debug-full" \
            "${ROOT}/build-work"; do
            if [ -f "${candidate}/CMakeCache.txt" ]; then
                BUILD_DIR="${candidate}"
                break
            fi
        done
    fi
fi

BIN="${BUILD_DIR}/go2cpp_channel_deadlock"
if [ -z "${BUILD_DIR}" ] || [ ! -x "${BIN}" ]; then
    # audit-base-channel.md C5：缺 demo 原先一律 exit 77，ctest 记 SKIP，
    # 构建/配置回归永远看不到红灯。ctest 模式下 demo 是本构建的产物，
    # 缺件必须按失败上报；只有手工运行（尚未构建）才保留 77 表示缺件。
    echo "FAIL: 缺少 ${BIN}" >&2
    echo "  先构建：flock /tmp/go2cpp_build.lock cmake --build ${BUILD_DIR:-build-doctor-full} --target go2cpp_channel_deadlock -j" >&2
    if [ "${CTEST_MODE}" -eq 1 ]; then
        exit 1
    fi
    echo "SKIP: 手工运行且未构建 demo（ctest 模式下会按失败处理）" >&2
    exit 77
fi
if ! command -v timeout >/dev/null 2>&1; then
    # coreutils 缺失属于环境依赖：保留 77，cmake 侧显式声明
    # SKIP_RETURN_CODE=77，ctest 记 SKIPPED 而不是 PASS。
    echo "SKIP: 需要 coreutils timeout" >&2
    exit 77
fi

TAG="$$-$(date +%s)"
LOG_DIR="/tmp/go2cpp-channel-deadlock-${TAG}"
mkdir -p "${LOG_DIR}"
PASS=0
FAIL=0
DEMO_PID=""

pass() { echo "PASS $*"; PASS=$((PASS + 1)); }
fail() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

cleanup() {
    if [ -n "${DEMO_PID}" ] && kill -0 "${DEMO_PID}" 2>/dev/null; then
        kill -TERM "${DEMO_PID}" 2>/dev/null || true
        sleep 0.2
        kill -KILL "${DEMO_PID}" 2>/dev/null || true
        wait "${DEMO_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

expect_contains() { # <file> <needle> <name>
    if grep -q -- "$2" "$1" 2>/dev/null; then
        pass "$3"
    else
        fail "$3（缺 $(printf '%q' "$2")）"
    fi
}

expect_count() { # <file> <pattern> <min> <name>
    local count
    count="$(grep -c -- "$2" "$1" 2>/dev/null || true)"
    if [ "${count:-0}" -ge "$3" ]; then
        pass "$4（${count} 行）"
    else
        fail "$4（期望 >=${3} 行，实际 ${count:-0}）"
    fi
}

get_field() { # <line> <key>
    printf '%s\n' "$1" | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2-
}

# ---------------------------------------------------------------------------
# 1) baseline：纯阻塞互等 → 永久 stuck（对照组）
# ---------------------------------------------------------------------------
echo "=== baseline：纯阻塞互等（死锁对照）==="
BASELINE_OUT="${LOG_DIR}/baseline.out"
BASELINE_ERR="${LOG_DIR}/baseline.err"
"${BIN}" --mode=baseline --hold-ms=60000 \
    >"${BASELINE_OUT}" 2>"${BASELINE_ERR}" &
DEMO_PID=$!
READY=0
for _ in $(seq 1 200); do
    if grep -q '^DEADLOCK_READY ' "${BASELINE_OUT}" 2>/dev/null; then
        READY=1
        break
    fi
    kill -0 "${DEMO_PID}" 2>/dev/null || break
    sleep 0.05
done
if [ "${READY}" -eq 1 ]; then
    pass "baseline 进入 DEADLOCK_READY"
else
    fail "baseline 未进入 DEADLOCK_READY：$(tail -3 "${BASELINE_OUT}" 2>/dev/null)"
fi

if [ "${READY}" -eq 1 ]; then
    sleep 1.5
    if kill -0 "${DEMO_PID}" 2>/dev/null; then
        pass "baseline 1.5s 后进程仍存活（未自行恢复）"
    else
        fail "baseline 在 1.5s 内退出（不应发生）"
    fi
    expect_count "${BASELINE_OUT}" '^STILL_STUCK ' 2 "baseline 持续输出 STILL_STUCK 心跳"
    expect_contains "${BASELINE_OUT}" 'stuck=2' "baseline stuck=2（双方都 park 在 channel 上）"
    if grep -q '^RESULT ' "${BASELINE_OUT}" 2>/dev/null; then
        fail "baseline 出现了 RESULT（应永久卡住）"
    else
        pass "baseline 没有 RESULT（无进展）"
    fi

    kill -TERM "${DEMO_PID}" 2>/dev/null || true
    for _ in $(seq 1 100); do
        kill -0 "${DEMO_PID}" 2>/dev/null || break
        sleep 0.05
    done
    if kill -0 "${DEMO_PID}" 2>/dev/null; then
        kill -KILL "${DEMO_PID}" 2>/dev/null || true
        fail "baseline SIGTERM 后未退出，已 SIGKILL"
    else
        pass "baseline SIGTERM 后退出"
    fi
    wait "${DEMO_PID}" 2>/dev/null
    BASELINE_RC=$?
    DEMO_PID=""
    if [ "${BASELINE_RC}" -eq 0 ]; then
        pass "baseline 退出码 0（demo 自行报告死锁）"
    else
        fail "baseline 退出码 ${BASELINE_RC}"
    fi
    expect_contains "${BASELINE_OUT}" 'BASELINE_RESULT mode=baseline deadlock=1 stuck=2 recovered=0' \
        "baseline 最终报告 deadlock=1 stuck=2 recovered=0"
    expect_contains "${BASELINE_OUT}" 'stopped=1' "baseline 由外部信号停止"
fi

BASELINE_HELD="$(grep -m1 '^BASELINE_RESULT ' "${BASELINE_OUT}" 2>/dev/null | sed -n 's/.*held_ms=\([0-9]*\).*/\1/p')"
BASELINE_STUCK="$(grep -m1 '^BASELINE_RESULT ' "${BASELINE_OUT}" 2>/dev/null | sed -n 's/.* stuck=\([0-9]*\).*/\1/p')"

# ---------------------------------------------------------------------------
# 2) timeout：select + AfterCase(200ms) → 超时退出，stuck=0
# ---------------------------------------------------------------------------
echo "=== timeout：select + 200ms 定时器 ==="
TIMEOUT_OUT="${LOG_DIR}/timeout.out"
TIMEOUT_ERR="${LOG_DIR}/timeout.err"
timeout 60 "${BIN}" --mode=timeout --repro=1 --timeout-ms=200 \
    >"${TIMEOUT_OUT}" 2>"${TIMEOUT_ERR}"
TIMEOUT_RC=$?
if [ "${TIMEOUT_RC}" -eq 0 ]; then
    pass "timeout 退出码 0"
else
    fail "timeout 退出码 ${TIMEOUT_RC}"
fi
TIMEOUT_RESULT="$(grep -m1 '^RESULT ' "${TIMEOUT_OUT}" || true)"
echo "  ${TIMEOUT_RESULT}"
expect_contains "${TIMEOUT_OUT}" '^STUCK mode=timeout .*stuck=2' \
    "timeout 观察到互等窗口 stuck=2"
expect_contains "${TIMEOUT_OUT}" 'pass=1' "timeout RESULT pass=1"
expect_contains "${TIMEOUT_OUT}" 'stuck_after=0' "timeout stuck_after=0"
expect_contains "${TIMEOUT_OUT}" 'a_timed_out=1' "timeout A 从定时器分支退出"
expect_contains "${TIMEOUT_OUT}" 'b_timed_out=1' "timeout B 从定时器分支退出"
expect_contains "${TIMEOUT_OUT}" 'a_completed=1' "timeout A completed=1"
expect_contains "${TIMEOUT_OUT}" 'b_completed=1' "timeout B completed=1"
TIMEOUT_A_MS="$(get_field "${TIMEOUT_RESULT}" a_elapsed_ms)"
TIMEOUT_B_MS="$(get_field "${TIMEOUT_RESULT}" b_elapsed_ms)"
echo "  timeout 耗时：A=${TIMEOUT_A_MS}ms B=${TIMEOUT_B_MS}ms"
if [ -n "${TIMEOUT_A_MS}" ] && [ "${TIMEOUT_A_MS}" -ge 180 ] && [ "${TIMEOUT_A_MS}" -le 3000 ]; then
    pass "timeout A 耗时在 [180,3000]ms：${TIMEOUT_A_MS}ms"
else
    fail "timeout A 耗时异常：${TIMEOUT_A_MS}ms"
fi
if [ -n "${TIMEOUT_B_MS}" ] && [ "${TIMEOUT_B_MS}" -ge 180 ] && [ "${TIMEOUT_B_MS}" -le 3000 ]; then
    pass "timeout B 耗时在 [180,3000]ms：${TIMEOUT_B_MS}ms"
else
    fail "timeout B 耗时异常：${TIMEOUT_B_MS}ms"
fi
expect_contains "${TIMEOUT_ERR}" '\[A\] TIMEOUT ' "timeout A 打印 TIMEOUT 分支"
expect_contains "${TIMEOUT_ERR}" '\[B\] TIMEOUT ' "timeout B 打印 TIMEOUT 分支"

# ---------------------------------------------------------------------------
# 3) multi：两路 select（收 + 发）→ send/recv 配对，双方完成
# ---------------------------------------------------------------------------
echo "=== multi：两路 select（收 + 发）==="
MULTI_OUT="${LOG_DIR}/multi.out"
MULTI_ERR="${LOG_DIR}/multi.err"
timeout 60 "${BIN}" --mode=multi --repro=1 \
    >"${MULTI_OUT}" 2>"${MULTI_ERR}"
MULTI_RC=$?
if [ "${MULTI_RC}" -eq 0 ]; then
    pass "multi 退出码 0"
else
    fail "multi 退出码 ${MULTI_RC}"
fi
MULTI_RESULT="$(grep -m1 '^RESULT ' "${MULTI_OUT}" || true)"
echo "  ${MULTI_RESULT}"
expect_contains "${MULTI_OUT}" 'pass=1' "multi RESULT pass=1"
expect_contains "${MULTI_OUT}" 'a_completed=1' "multi A completed=1"
expect_contains "${MULTI_OUT}" 'b_completed=1' "multi B completed=1"
expect_contains "${MULTI_OUT}" 'stuck_after=0' "multi stuck_after=0"
MULTI_A_KIND="$(get_field "${MULTI_RESULT}" a_kind)"
MULTI_B_KIND="$(get_field "${MULTI_RESULT}" b_kind)"
echo "  multi 角色：A kind=${MULTI_A_KIND} B kind=${MULTI_B_KIND}（1=recv 3=send）"
if { [ "${MULTI_A_KIND}" = "1" ] && [ "${MULTI_B_KIND}" = "3" ]; } ||
   { [ "${MULTI_A_KIND}" = "3" ] && [ "${MULTI_B_KIND}" = "1" ]; }; then
    pass "multi 一个 send 与一个 recv 配对完成"
else
    fail "multi 角色不是一 send 一 recv：A=${MULTI_A_KIND} B=${MULTI_B_KIND}"
fi
expect_contains "${MULTI_ERR}" 'role=send' "multi 日志出现 send 角色"
expect_contains "${MULTI_ERR}" 'role=recv' "multi 日志出现 recv 角色"

# ---------------------------------------------------------------------------
# 4) default：select + default → 非阻塞降级，no_ready 计数
# ---------------------------------------------------------------------------
echo "=== default：select + default 非阻塞降级 ==="
DEFAULT_OUT="${LOG_DIR}/default.out"
DEFAULT_ERR="${LOG_DIR}/default.err"
timeout 60 "${BIN}" --mode=default --repro=1 --default-retries=3 \
    --default-interval-ms=20 >"${DEFAULT_OUT}" 2>"${DEFAULT_ERR}"
DEFAULT_RC=$?
if [ "${DEFAULT_RC}" -eq 0 ]; then
    pass "default 退出码 0"
else
    fail "default 退出码 ${DEFAULT_RC}"
fi
DEFAULT_RESULT="$(grep -m1 '^RESULT ' "${DEFAULT_OUT}" || true)"
echo "  ${DEFAULT_RESULT}"
expect_contains "${DEFAULT_OUT}" 'pass=1' "default RESULT pass=1"
expect_contains "${DEFAULT_OUT}" 'a_fallback=1' "default A 走降级路径"
expect_contains "${DEFAULT_OUT}" 'b_fallback=1' "default B 走降级路径"
expect_contains "${DEFAULT_OUT}" 'stuck_after=0' "default stuck_after=0"
DEFAULT_A_NR="$(get_field "${DEFAULT_RESULT}" a_no_ready)"
DEFAULT_B_NR="$(get_field "${DEFAULT_RESULT}" b_no_ready)"
echo "  default no_ready：A=${DEFAULT_A_NR} B=${DEFAULT_B_NR}"
if [ "${DEFAULT_A_NR:-0}" -eq 3 ] && [ "${DEFAULT_B_NR:-0}" -eq 3 ]; then
    pass "default no_ready=3/3（三次尝试均无就绪）"
else
    fail "default no_ready 计数异常：A=${DEFAULT_A_NR} B=${DEFAULT_B_NR}"
fi
expect_count "${DEFAULT_ERR}" 'NO_READY ' 6 "default 日志出现 6 次 NO_READY"

# ---------------------------------------------------------------------------
# 5) --repro N：timeout / multi / default 重复率
# ---------------------------------------------------------------------------
echo "=== repro ${REPRO}：三种 select 写法的重复率 ==="
for mode in timeout multi default; do
    REPRO_OUT="${LOG_DIR}/repro-${mode}.out"
    REPRO_ERR="${LOG_DIR}/repro-${mode}.err"
    timeout 180 "${BIN}" --mode="${mode}" --repro="${REPRO}" \
        --default-retries=3 --default-interval-ms=20 \
        >"${REPRO_OUT}" 2>"${REPRO_ERR}"
    REPRO_RC=$?
    if [ "${REPRO_RC}" -eq 0 ]; then
        pass "repro-${mode} 退出码 0"
    else
        fail "repro-${mode} 退出码 ${REPRO_RC}"
    fi
    REPRO_LINE="$(grep -m1 '^REPRO ' "${REPRO_OUT}" || true)"
    echo "  ${REPRO_LINE}"
    ROUNDS="$(get_field "${REPRO_LINE}" rounds)"
    PASSED="$(get_field "${REPRO_LINE}" passed)"
    FAILED="$(get_field "${REPRO_LINE}" failed)"
    if [ "${ROUNDS:-0}" -eq "${REPRO}" ] && [ "${PASSED:-0}" -eq "${REPRO}" ] &&
       [ "${FAILED:-0}" -eq 0 ]; then
        pass "repro-${mode} ${PASSED}/${ROUNDS} 通过"
    else
        fail "repro-${mode} 汇总异常：rounds=${ROUNDS} passed=${PASSED} failed=${FAILED}"
    fi
done

# ---------------------------------------------------------------------------
# 对比表 + 汇总
# ---------------------------------------------------------------------------
echo
echo "===== channel 互等：四种写法对比 ====="
printf '%-10s %-12s %-32s %s\n' "模式" "是否死锁" "恢复方式" "实测耗时"
printf '%-10s %-12s %-32s %s\n' "baseline" "是(stuck=2)" "未恢复（外部 SIGTERM）" "held_ms=${BASELINE_HELD:-?}"
printf '%-10s %-12s %-32s %s\n' "timeout" "否" "AfterCase(200ms) 超时退出" "A=${TIMEOUT_A_MS:-?}ms B=${TIMEOUT_B_MS:-?}ms"
printf '%-10s %-12s %-32s %s\n' "multi" "否" "select send/recv 配对" "A/B 均完成（kind ${MULTI_A_KIND:-?}/${MULTI_B_KIND:-?}）"
printf '%-10s %-12s %-32s %s\n' "default" "否" "DefaultCase 降级" "no_ready A=${DEFAULT_A_NR:-?} B=${DEFAULT_B_NR:-?}"

echo
echo "PASS=${PASS} FAIL=${FAIL} logs=${LOG_DIR}"
if [ "${FAIL}" -ne 0 ]; then
    exit 1
fi
exit 0
