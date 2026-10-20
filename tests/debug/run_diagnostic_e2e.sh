#!/usr/bin/env bash
# Doctor 端到端验收：真实进程 + 真实 Unix socket + 丢唤醒恢复。
#
# 用法：bash tests/debug/run_diagnostic_e2e.sh [build-dir]
# 依赖：build 目录里已构建 go2cpp_diagnostic_demo（GO2CPP_DIAGNOSTIC=ON）。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build-diag}"
DEMO="${BUILD_DIR}/go2cpp_diagnostic_demo"
CLIENT="${ROOT}/tests/debug/doctor_client.py"
TAG="$$-$(date +%s)"
SOCK="/tmp/go2cpp-doctor-e2e-${TAG}.sock"
LOG="/tmp/go2cpp-doctor-e2e-${TAG}.log"
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
    rm -f "${SOCK}" "${LOG}"
}
trap cleanup EXIT

if [ ! -x "${DEMO}" ]; then
    echo "SKIP: ${DEMO} 不存在（先构建 -DGO2CPP_DIAGNOSTIC=ON）"
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: 需要 python3"
    exit 77
fi

"${DEMO}" "${SOCK}" >"${LOG}" 2>&1 &
DEMO_PID=$!

READY=0
for _ in $(seq 1 200); do
    if grep -q 'READY' "${LOG}" 2>/dev/null; then READY=1; break; fi
    if ! kill -0 "${DEMO_PID}" 2>/dev/null; then break; fi
    sleep 0.05
done
if [ "${READY}" -ne 1 ]; then
    fail "demo 未进入 READY：$(tail -5 "${LOG}" 2>/dev/null)"
    echo "PASS=${PASS} FAIL=${FAIL}"
    exit 1
fi
pass "demo READY $(grep -m1 READY "${LOG}")"

# 1) status：卡死期间仍能返回（Doctor 线程不经过 Scheduler）。
OUT="$(python3 "${CLIENT}" "${SOCK}" status 2>&1)"
if echo "${OUT}" | grep -q 'schema=go2cpp.diagnostic.status/1' &&
   echo "${OUT}" | grep -q 'scheduler.0.processors=2'; then
    pass "status 在卡死现场返回（$(echo "${OUT}" | grep -c '=') 行）"
else
    fail "status 输出异常：${OUT}"
fi
if echo "${OUT}" | grep -q 'module.0.build_id='; then
    pass "status 含模块基址/build-id"
else
    fail "status 缺 build-id"
fi

# 2) dump：复用 Debug 组件 dump（demo 已注册 scheduler/channel/parking/mutex/
#    fiber + 内置 runtime/events；io 没有 IOManager，允许 unavailable）。
OUT="$(python3 "${CLIENT}" "${SOCK}" "dump sections=all" 2>&1)"
OK=1
for section in scheduler demo-cv demo-channel demo-mutex channel-waiter runtime events; do
    echo "${OUT}" | grep -q "\[component ${section}\]" || OK=0
done
if [ "${OK}" -eq 1 ]; then
    pass "dump 已注册组件全部返回"
else
    fail "dump 缺组件：$(echo "${OUT}" | grep 'component .* =====' | wc -l)"
fi
if echo "${OUT}" | grep -q 'duration_us='; then
    pass "dump 响应标注 duration/queue_wait"
else
    fail "dump 响应缺 duration 元数据"
fi

# 3) deadlock-report：等到检测阈值后必须给出 deadlock 判定与等待图。
DEADLOCK_OUT=""
for _ in $(seq 1 100); do
    DEADLOCK_OUT="$(python3 "${CLIENT}" "${SOCK}" deadlock-report 2>&1)"
    if echo "${DEADLOCK_OUT}" | grep -q 'verdict=deadlock'; then break; fi
    sleep 0.1
done
if echo "${DEADLOCK_OUT}" | grep -q 'verdict=deadlock'; then
    pass "deadlock-report verdict=deadlock"
else
    fail "deadlock-report 未判定 deadlock：$(echo "${DEADLOCK_OUT}" | head -5)"
fi
if echo "${DEADLOCK_OUT}" | grep -q 'edge\.0\.from=scheduler:scheduler' &&
   echo "${DEADLOCK_OUT}" | grep -q 'edge\..*kind=parking-condition'; then
    pass "等待图含 scheduler/parking-condition 边"
else
    fail "等待图边不足：$(echo "${DEADLOCK_OUT}" | grep -c 'edge\.')"
fi

# 4) 白名单负路径：mutex scope 必须 refused（退出码 4）。
python3 "${CLIENT}" "${SOCK}" "force-wake scope=mutex" >/dev/null 2>&1
if [ $? -eq 4 ]; then
    pass "force-wake scope=mutex refused（退出码 4）"
else
    fail "force-wake scope=mutex 未返回 refused"
fi

# 5) force-wake all：恢复丢唤醒。
OUT="$(python3 "${CLIENT}" "${SOCK}" "force-wake scope=all" 2>&1)"
if echo "${OUT}" | grep -q 'verdict=ok'; then
    pass "force-wake all verdict=ok $(echo "${OUT}" | grep woken_total || true)"
else
    fail "force-wake all 失败：${OUT}"
fi
RECOVERED=0
for _ in $(seq 1 100); do
    if grep -q 'RECOVERED' "${LOG}" 2>/dev/null; then RECOVERED=1; break; fi
    sleep 0.05
done
if [ "${RECOVERED}" -eq 1 ]; then
    pass "丢唤醒进程恢复（$(grep -m1 RECOVERED "${LOG}")）"
else
    fail "进程未恢复：$(tail -5 "${LOG}")"
fi

# 6) 恢复后 status 可再次返回，runqueue 归零。
sleep 0.1
OUT="$(python3 "${CLIENT}" "${SOCK}" status 2>&1)"
if echo "${OUT}" | grep -q 'goroutines.waiting='; then
    pass "恢复后 status 仍可用（$(echo "${OUT}" | grep -m1 'goroutines.waiting=')）"
else
    fail "恢复后 status 异常"
fi

# 7) 并发 10 条命令：单线程串行执行、响应按 stream_id 匹配。
OUT="$(python3 "${CLIENT}" "${SOCK}" --concurrency 10 --command status 2>&1)"
if echo "${OUT}" | grep -q 'PASS concurrency=10'; then
    pass "并发 10 命令串行执行且 stream_id 匹配"
else
    fail "并发 10 命令失败：${OUT}"
fi

# 8) stats：concurrent_peak 必须为 1（单线程命令平面）。
OUT="$(python3 "${CLIENT}" "${SOCK}" stats 2>&1)"
if echo "${OUT}" | grep -q 'concurrent_peak=1'; then
    pass "stats concurrent_peak=1"
else
    fail "stats concurrent_peak 不为 1：$(echo "${OUT}" | grep concurrent_peak)"
fi
if echo "${OUT}" | grep -q 'deadlock_events=[1-9]'; then
    pass "死锁事件已计数"
else
    fail "死锁事件未计数：$(echo "${OUT}" | grep deadlock_events)"
fi

echo "PASS=${PASS} FAIL=${FAIL}"
[ "${FAIL}" -eq 0 ]
