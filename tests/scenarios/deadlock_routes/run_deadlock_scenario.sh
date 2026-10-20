#!/usr/bin/env bash
# 真实场景端到端验收：100+ HTTP client → 锁顺序死锁 → Debug dump →
# deadlock-report → Doctor force-wake（无效）→ mutex-unlock（refused）→
# mutex-unlock --force（受控解锁）→ 全部 client 恢复。
#
# 用法：
#   bash tests/scenarios/deadlock_routes/run_deadlock_scenario.sh [BUILD_DIR] [选项]
#
# 选项：
#   --repro N     重复触发 N 次（每次新进程），统计死锁触发率与恢复率
#   --clients N   加载 client 数（默认 120；服务端下限 100）
#   --load-ms N   加载阶段时长（默认 1500；--repro 模式默认 800）
#
# 依赖：BUILD_DIR 已构建 go2cpp_deadlock_routes 与 tools/cli/go2cpp-cli，
#       且构建时 GO2CPP_DEBUG_DUMP=ON、GO2CPP_DIAGNOSTIC=ON、GO2CPP_WITH_CLI=ON。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT}/build-doctor-full"
REPRO=1
CLIENTS=120
LOAD_MS=1500
LOAD_MS_SET=0
MODE="full"

while [ $# -gt 0 ]; do
    case "$1" in
        --repro) REPRO="$2"; MODE="repro"; shift 2 ;;
        --clients) CLIENTS="$2"; shift 2 ;;
        --load-ms) LOAD_MS="$2"; LOAD_MS_SET=1; shift 2 ;;
        -*) echo "未知选项 $1" >&2; exit 2 ;;
        *) BUILD_DIR="$1"; shift ;;
    esac
done
if [ "${MODE}" = "repro" ] && [ "${LOAD_MS_SET}" -eq 0 ]; then
    LOAD_MS=800
fi

SERVER="${BUILD_DIR}/go2cpp_deadlock_routes"
CLI="${BUILD_DIR}/tools/cli/go2cpp-cli"
for f in "${SERVER}" "${CLI}"; do
    if [ ! -x "${f}" ]; then
        echo "SKIP: 缺少可执行文件 ${f}"
        echo "  先构建：cmake -S . -B ${BUILD_DIR} -DGO2CPP_DEBUG_DUMP=ON -DGO2CPP_DIAGNOSTIC=ON -DGO2CPP_WITH_CLI=ON"
        echo "          cmake --build ${BUILD_DIR} -j"
        exit 77
    fi
done

TAG="$$-$(date +%s)"
LOG="/tmp/deadlock-scenario-${TAG}.log"
DUMP="/tmp/scenario-deadlock-dump.txt"
REPORT="/tmp/deadlock_scenario_evidence.txt"
TRIGGER_LOG="/tmp/deadlock-trigger-rate.txt"
REPORT_FILE="/tmp/deadlock-report.txt"
FORCE_WAKE_FILE="/tmp/deadlock-force-wake.txt"
REFUSED_FILE="/tmp/deadlock-unlock-refused.txt"
NORESASON_FILE="/tmp/deadlock-unlock-noreason.txt"
FORCED_FILE="/tmp/deadlock-unlock-forced.txt"
PASS=0
FAIL=0
SERVER_PID=""
DEBUG_SOCK=""
DOCTOR_SOCK=""
PORT=""
CURRENT_LOG=""
STUCK_BEFORE="?"

pass() { echo "PASS $*"; PASS=$((PASS + 1)); }
fail() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

cleanup() {
    if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    [ -n "${DEBUG_SOCK}" ] && rm -f "${DEBUG_SOCK}"
    [ -n "${DOCTOR_SOCK}" ] && rm -f "${DOCTOR_SOCK}"
}
trap cleanup EXIT

# ---------------------------------------------------------------- 启动 / 等待
start_server() {
    local load_ms="$1" attempt=0 waited=0
    while [ "${attempt}" -lt 4 ]; do
        attempt=$((attempt + 1))
        # 端口取在 Linux 临时端口范围（32768-60999）之外：压测会在客户端
        # 侧留下大量 TIME_WAIT 端口，落在临时端口段里会与监听 bind 冲突。
        PORT=$((18000 + RANDOM % 8000))
        DEBUG_SOCK="/tmp/go2cpp-scenario-${TAG}-${PORT}.sock"
        DOCTOR_SOCK="/tmp/go2cpp-scenario-${TAG}-${PORT}-doctor.sock"
        CURRENT_LOG="${LOG}.${PORT}"
        : >"${CURRENT_LOG}"
        "${SERVER}" --port "${PORT}" --clients "${CLIENTS}" \
            --load-ms "${load_ms}" \
            --debug-socket "${DEBUG_SOCK}" \
            --doctor-socket "${DOCTOR_SOCK}" >"${CURRENT_LOG}" 2>&1 &
        SERVER_PID=$!
        waited=0
        while [ "${waited}" -lt 20 ]; do
            if grep -q '^READY ' "${CURRENT_LOG}" 2>/dev/null; then
                return 0
            fi
            if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
                break
            fi
            sleep 0.05
            waited=$((waited + 1))
        done
        stop_server >/dev/null
    done
    return 1
}

stop_server() { # 最多等 5s 自然退出，再 kill；返回真实退出码（自然退出时）
    local rc=0 n=0
    if [ -n "${SERVER_PID}" ]; then
        while [ "${n}" -lt 100 ] && kill -0 "${SERVER_PID}" 2>/dev/null; do
            sleep 0.05
            n=$((n + 1))
        done
        if kill -0 "${SERVER_PID}" 2>/dev/null; then
            kill "${SERVER_PID}" 2>/dev/null || true
        fi
        wait "${SERVER_PID}" 2>/dev/null || rc=$?
        SERVER_PID=""
    fi
    rm -f "${DEBUG_SOCK}" "${DOCTOR_SOCK}"
    return "${rc}"
}

wait_log() { # pattern timeout_seconds
    local pattern="$1" timeout="$2" waited=0
    local limit=$((timeout * 20))
    while [ "${waited}" -lt "${limit}" ]; do
        if grep -q -- "${pattern}" "${CURRENT_LOG}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            return 1
        fi
        sleep 0.05
        waited=$((waited + 1))
    done
    return 1
}

server_pid_from_log() {
    sed -n 's/^READY pid=\([0-9]*\).*/\1/p' "${CURRENT_LOG}" | head -1
}

doctor() { "${CLI}" doctor --socket "${DOCTOR_SOCK}" "$@" 2>&1; }

# status 的值里 '=' 会被 dump 层转义成 '\='；先去掉反斜杠再解析。
mutex_waiters() {
    doctor status 2>/dev/null | tr -d '\\' |
        sed -n "s/^waits\.$1=.*waiters=\([0-9]*\).*/\1/p" | head -1
}

goroutines_waiting() {
    doctor status 2>/dev/null | tr -d '\\' |
        sed -n 's/^goroutines\.waiting=\([0-9]*\).*/\1/p' | head -1
}

verdict_now() {
    doctor status 2>/dev/null |
        sed -n 's/^doctor\.deadlock_verdict=//p' | head -1
}

# 从 deadlock-report 里按 from 名取字段（grep -A 覆盖一条 mutex 边 7 行）。
edge_field() { # name field
    grep -A12 -E "^edge\.[0-9]+\.from=$1$" "${REPORT_FILE}" |
        sed -n "s/^edge\.[0-9]*\.$2=\([0-9]*\)/\1/p" | head -1
}

# ---------------------------------------------------------------- 完整验收
run_full() {
    echo "== 完整场景：启动服务器（clients=${CLIENTS} load_ms=${LOAD_MS}）"
    if ! start_server "${LOAD_MS}"; then
        fail "服务器启动失败（连续 bind 失败）：$(tail -3 "${CURRENT_LOG}" 2>/dev/null)"
        return 1
    fi
    if ! wait_log '^READY ' 20; then
        fail "服务器未进入 READY：$(tail -5 "${CURRENT_LOG}" 2>/dev/null)"
        stop_server >/dev/null
        return 1
    fi
    local pid
    pid="$(server_pid_from_log)"
    pass "服务器 READY $(grep -m1 '^READY ' "${CURRENT_LOG}")"

    # 1) 100+ client 压测数据
    if ! wait_log '^LOAD_DONE ' 30; then
        fail "未看到 LOAD_DONE"
        stop_server >/dev/null
        return 1
    fi
    local load_line
    load_line="$(grep -m1 '^LOAD_DONE ' "${CURRENT_LOG}")"
    echo "  ${load_line}"
    if echo "${load_line}" | grep -qE 'load_requests=[1-9][0-9]{2,}'; then
        pass "加载阶段完成：$(echo "${load_line}" | sed 's/^LOAD_DONE //')"
    else
        fail "加载请求数不足：${load_line}"
    fi

    # 2) 死锁建立
    if ! wait_log '^DEADLOCK_READY ' 20; then
        fail "未建立死锁：$(tail -5 "${CURRENT_LOG}")"
        stop_server >/dev/null
        return 1
    fi
    local deadlock_line gid_a gid_b
    deadlock_line="$(grep -m1 '^DEADLOCK_READY ' "${CURRENT_LOG}")"
    echo "  ${deadlock_line}"
    gid_a="$(echo "${deadlock_line}" | sed -n 's/.*gid_a=\([0-9]*\).*/\1/p')"
    gid_b="$(echo "${deadlock_line}" | sed -n 's/.*gid_b=\([0-9]*\).*/\1/p')"
    pass "死锁建立 gid_a=${gid_a} gid_b=${gid_b}"

    if ! wait_log '^QUIESCENT ' 20; then
        fail "未进入静默态"
        stop_server >/dev/null
        return 1
    fi
    local quiescent_line
    quiescent_line="$(grep -m1 '^QUIESCENT ' "${CURRENT_LOG}")"
    echo "  ${quiescent_line}"
    STUCK_BEFORE="$(echo "${quiescent_line}" | sed -n 's/.*stuck=\([0-9]*\).*/\1/p')"

    # 3) Debug dump（DebugServer / CLI）
    export GO2CPP_DEBUG_SOCKET="${DEBUG_SOCK}"
    if "${CLI}" dump --pid "${pid}" --sections scheduler,fiber,mutex,events \
            --format text --out "${DUMP}" >/tmp/deadlock-dump-cmd.log 2>&1; then
        pass "Debug dump 已写入 ${DUMP}（$(wc -c <"${DUMP}") 字节）"
    else
        fail "Debug dump 失败：$(cat /tmp/deadlock-dump-cmd.log)"
    fi
    local dump_ok=1
    grep -q '^schema=go2cpp.dump.scheduler/1' "${DUMP}" || dump_ok=0
    grep -q '^fiber\.[0-9]*\.state=kWaiting' "${DUMP}" || dump_ok=0
    grep -q '^mutex\.res-a\.held=true' "${DUMP}" || dump_ok=0
    grep -q '^mutex\.res-b\.held=true' "${DUMP}" || dump_ok=0
    if [ "${dump_ok}" -eq 1 ]; then
        pass "dump 含 scheduler/fiber/mutex 关键段"
    else
        fail "dump 缺关键段"
    fi
    local cycle_ok=1
    grep -q "^fiber\.[0-9]*\.gid=${gid_a}$" "${DUMP}" || cycle_ok=0
    grep -q "^fiber\.[0-9]*\.gid=${gid_b}$" "${DUMP}" || cycle_ok=0
    grep -q '^fiber\.[0-9]*\.waiting_on=res-b$' "${DUMP}" || cycle_ok=0
    grep -q '^fiber\.[0-9]*\.waiting_on=res-a$' "${DUMP}" || cycle_ok=0
    if [ "${cycle_ok}" -eq 1 ]; then
        pass "dump 抓到交叉等待：$(grep -E '^fiber\.[0-9]+\.(gid|role|state|waiting_on|held)=' "${DUMP}" | head -5 | tr '\n' ' ')"
    else
        fail "dump 未体现 gid_a/gid_b 的交叉等待"
    fi
    grep -q 'kParkEnter\|kMutexLock\|kMutexUnlock' "${DUMP}" &&
        pass "dump 含事件环（park/mutex 事件）" || fail "dump 缺事件环"

    # 4) deadlock-report：等待图 + verdict
    # 全局无进展判定需要超过 deadlock_threshold（服务端设 600ms）才会从
    # stalled 变成 deadlock，因此这里轮询到判定稳定为止。
    local tries=0
    while [ "${tries}" -lt 80 ]; do
        doctor deadlock-report >"${REPORT_FILE}" 2>&1
        if grep -q '^verdict=deadlock$' "${REPORT_FILE}"; then
            break
        fi
        sleep 0.1
        tries=$((tries + 1))
    done
    if grep -q '^verdict=deadlock$' "${REPORT_FILE}"; then
        pass "deadlock-report verdict=deadlock（$(grep -m1 '^no_progress_ms=' "${REPORT_FILE}")）"
    else
        fail "deadlock-report 未判定 deadlock：$(head -3 "${REPORT_FILE}")"
    fi
    if grep -qE '^edge\.[0-9]+\.from=res-a$' "${REPORT_FILE}" &&
       grep -qE '^edge\.[0-9]+\.from=res-b$' "${REPORT_FILE}" &&
       grep -qE '^edge\.[0-9]+\.owner_alive=true$' "${REPORT_FILE}"; then
        pass "等待图含 res-a/res-b mutex 边（owner_alive=true）"
    else
        fail "等待图缺 mutex 边"
    fi
    local owner_a owner_b waiters_a waiters_b
    owner_a="$(edge_field res-a owner_gid)"
    owner_b="$(edge_field res-b owner_gid)"
    waiters_a="$(edge_field res-a waiters)"
    waiters_b="$(edge_field res-b waiters)"
    echo "  cycle: ${gid_a}(res-a owner) --wait res-b--> ${gid_b}(res-b owner) --wait res-a--> ${gid_a}"
    echo "  graph: res-a owner=${owner_a} waiters=${waiters_a}; res-b owner=${owner_b} waiters=${waiters_b}"
    if [ "${owner_a}" = "${gid_a}" ] && [ "${owner_b}" = "${gid_b}" ]; then
        pass "等待图 owner 与死锁 G 对应（${gid_a}/${gid_b}）"
    else
        fail "等待图 owner 不匹配：res-a=${owner_a} res-b=${owner_b}"
    fi

    # 5) force-wake 对锁顺序死锁无效
    doctor "force-wake scope=all" >"${FORCE_WAKE_FILE}" 2>&1
    echo "  force-wake: $(grep -m1 '^verdict=' "${FORCE_WAKE_FILE}") $(grep -m1 '^woken_total=' "${FORCE_WAKE_FILE}")"
    sleep 0.3
    local w_a w_b g_waiting verdict_after
    w_a="$(mutex_waiters res-a)"
    w_b="$(mutex_waiters res-b)"
    g_waiting="$(goroutines_waiting)"
    verdict_after="$(verdict_now)"
    echo "  after force-wake: waits.res-a=${w_a} waits.res-b=${w_b} goroutines.waiting=${g_waiting} verdict=${verdict_after}"
    if [ "${w_a}" = "${waiters_a}" ] && [ "${w_b}" = "${waiters_b}" ] &&
       [ "${verdict_after}" = "deadlock" ]; then
        pass "force-wake 无效：waiters/verdict 均未变化（锁等待不可重试）"
    else
        fail "force-wake 意外改变了现场"
    fi

    # 6) 默认 mutex-unlock：owner 存活 → refused
    doctor "mutex-unlock name=res-a" >"${REFUSED_FILE}" 2>&1
    local refused_exit=$?
    if [ "${refused_exit}" -eq 5 ] && grep -q 'owner-alive' "${REFUSED_FILE}"; then
        pass "mutex-unlock 默认拒绝：$(head -1 "${REFUSED_FILE}")"
    else
        fail "mutex-unlock 默认路径未 refused（exit=${refused_exit}）"
    fi
    doctor "mutex-unlock name=res-a --force" >"${NORESASON_FILE}" 2>&1
    if [ $? -eq 5 ] && grep -q 'requires reason' "${NORESASON_FILE}"; then
        pass "mutex-unlock --force 缺少 reason 被拒绝"
    else
        fail "mutex-unlock --force 缺少 reason 未被拒绝"
    fi

    # 7) 受控强制解锁
    doctor "mutex-unlock name=res-a --force reason=deadlock-scenario-e2e" \
        >"${FORCED_FILE}" 2>&1
    if grep -q '^verdict=forced$' "${FORCED_FILE}" &&
       grep -q '^audit=true$' "${FORCED_FILE}" &&
       grep -q '^held_after=false$' "${FORCED_FILE}"; then
        pass "mutex-unlock --force 执行：$(grep -E '^(waiters_before|waiters_after|held_after|owner_gid)=' "${FORCED_FILE}" | tr '\n' ' ')"
    else
        fail "mutex-unlock --force 失败：$(cat "${FORCED_FILE}")"
    fi

    # 8) 恢复
    if wait_log '^RECOVERED ' 30; then
        local recovered_line stuck_after
        recovered_line="$(grep -m1 '^RECOVERED ' "${CURRENT_LOG}")"
        echo "  ${recovered_line}"
        stuck_after="$(echo "${recovered_line}" | sed -n 's/.*stuck_after=\([0-9]*\).*/\1/p')"
        if [ "${stuck_after}" = "0" ]; then
            pass "全部卡住请求恢复：stuck ${STUCK_BEFORE} → 0"
        else
            fail "仍有卡住请求：stuck_after=${stuck_after}"
        fi
        grep -q '^FINAL ' "${CURRENT_LOG}" &&
            pass "最终统计：$(grep -m1 '^FINAL ' "${CURRENT_LOG}" | sed 's/^FINAL //')"
        grep -q '^FINAL_ERRORS ' "${CURRENT_LOG}" &&
            echo "  $(grep -m1 '^FINAL_ERRORS ' "${CURRENT_LOG}")"
        grep -q '^FINAL_ROUTES ' "${CURRENT_LOG}" &&
            echo "  $(grep -m1 '^FINAL_ROUTES ' "${CURRENT_LOG}")"
        grep -q 'stale-unlock-skip' "${CURRENT_LOG}" &&
            echo "  $(grep -m1 'stale-unlock-skip' "${CURRENT_LOG}")"
        grep -q 'doctor\]\[audit\] mutex-unlock --force' "${CURRENT_LOG}" &&
            echo "  $(grep -m1 '\[audit\] mutex-unlock --force' "${CURRENT_LOG}")"
    else
        fail "未在 30s 内恢复：$(tail -5 "${CURRENT_LOG}")"
    fi

    # 9) 进程自然退出
    local rc=0
    stop_server >/dev/null
    rc=$?
    if [ "${rc}" -eq 0 ]; then
        pass "场景进程正常退出（exit=0）"
    else
        fail "场景进程退出码 ${rc}"
    fi
}

# ---------------------------------------------------------------- 触发率复现
run_repro() {
    local runs="$1" ok=0 recovered=0 i
    : >"${TRIGGER_LOG}"
    for i in $(seq 1 "${runs}"); do
        if ! start_server "${LOAD_MS}"; then
            echo "RUN ${i}: deadlock=0 recovered=0 (start-failed)" |
                tee -a "${TRIGGER_LOG}"
            continue
        fi
        local got=0 rec=0
        if wait_log '^DEADLOCK_READY ' 15; then
            got=1
            ok=$((ok + 1))
            doctor "mutex-unlock name=res-a --force reason=repro-${i}" \
                >/dev/null 2>&1
            if wait_log '^RECOVERED ' 20; then
                rec=1
                recovered=$((recovered + 1))
            fi
        fi
        stop_server >/dev/null
        echo "RUN ${i}: deadlock=${got} recovered=${rec}" | tee -a "${TRIGGER_LOG}"
        rm -f "${DEBUG_SOCK}" "${DOCTOR_SOCK}"
    done
    echo "TRIGGER_RATE ${ok}/${runs}" | tee -a "${TRIGGER_LOG}"
    echo "RECOVERY_RATE ${recovered}/${runs}" | tee -a "${TRIGGER_LOG}"
    if [ "${ok}" -ge 1 ] && [ $((ok * 10)) -ge "${runs}" ]; then
        pass "死锁触发率 ${ok}/${runs}（≥1/10）"
    else
        fail "死锁触发率 ${ok}/${runs} 低于 1/10"
    fi
    if [ "${recovered}" -eq "${ok}" ]; then
        pass "恢复率 ${recovered}/${ok}"
    else
        fail "恢复率 ${recovered}/${ok}"
    fi
}

# ---------------------------------------------------------------- main
echo "build_dir=${BUILD_DIR}"
echo "server=${SERVER}"
echo "cli=${CLI}"

if [ "${MODE}" = "repro" ]; then
    run_repro "${REPRO}"
else
    run_full
fi

echo "PASS=${PASS} FAIL=${FAIL}"
{
    echo "mode=${MODE} clients=${CLIENTS} load_ms=${LOAD_MS}"
    echo "PASS=${PASS} FAIL=${FAIL}"
    echo "log=${CURRENT_LOG}"
    echo "dump=${DUMP}"
    echo "deadlock_report=${REPORT_FILE}"
    echo "force_wake=${FORCE_WAKE_FILE}"
    echo "unlock_refused=${REFUSED_FILE}"
    echo "unlock_forced=${FORCED_FILE}"
} >"${REPORT}"
[ "${FAIL}" -eq 0 ]
