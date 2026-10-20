#!/usr/bin/env bash
# Doctor 能力增强测试矩阵（真实 diagnostic demo + G2CD/1）。
#
# 覆盖：status/dump 7 组件/deadlock-report/force-wake/mutex-unlock/
#       inject-fiber/stats、并发 10/50/100、协议坏校验/截断/超长、
#       CLI doctor、SIGTERM。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=doctor
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work doctor)"
DIAG_PID=""
CLIENT="$ROOT/tests/debug/doctor_client.py"
trap 'stop_pid "${DIAG_PID:-}"; rm -rf "$WORK"' EXIT

if [ ! -x "$DIAG_DEMO" ]; then
    fail "fixture:diagnostic-demo" normal "缺少 $DIAG_DEMO"
    summary
    exit 1
fi
if ! start_doctor_server 180; then
    fail "fixture:doctor-server" normal "demo 未 READY：$(tail -5 "$DIAG_LOG" | tr '\n' '|')"
    summary
    exit 1
fi

CLI_DOCTOR=(env GO2CPP_DOCTOR_SOCKET="$DIAG_SOCK" "$CLI" doctor)

expect_ok "status:协议/schema" normal "schema=go2cpp.diagnostic.status/1" \
    python3 "$CLIENT" "$DIAG_SOCK" status
expect_ok "status:调度器 processors=2" normal "scheduler.0.processors=2" \
    python3 "$CLIENT" "$DIAG_SOCK" status
expect_ok "status:模块 build-id" normal "module.0.build_id=" \
    python3 "$CLIENT" "$DIAG_SOCK" status

expect_ok "dump:all schema" normal "schema=go2cpp.dump.all/1" \
    python3 "$CLIENT" "$DIAG_SOCK" "dump sections=all"
DUMP_OUT="$(timeout 20 python3 "$CLIENT" "$DIAG_SOCK" "dump sections=all" 2>&1)"
for component in scheduler demo-cv demo-channel demo-mutex channel-waiter runtime events; do
    if printf '%s' "$DUMP_OUT" | grep -qF "[component ${component}]"; then
        pass "dump:组件 ${component}" normal
    else
        fail "dump:组件 ${component}" normal "缺少 [component ${component}]"
    fi
done

# deadlock-report：等待检测阈值。
DEADLOCK_OUT=""
for _ in $(seq 1 100); do
    DEADLOCK_OUT="$(timeout 20 python3 "$CLIENT" "$DIAG_SOCK" deadlock-report 2>&1)"
    printf '%s' "$DEADLOCK_OUT" | grep -q "verdict=deadlock" && break
    sleep 0.1
done
expect_contains "deadlock-report:verdict=deadlock" normal "verdict=deadlock" "$DEADLOCK_OUT"
expect_contains "deadlock-report:等待图边" normal "edge.0.from=scheduler:scheduler" "$DEADLOCK_OUT"
expect_contains "deadlock-report:parking-condition 边" normal "kind=parking-condition" "$DEADLOCK_OUT"

# 白名单负路径。
run_expect "force-wake:scope=mutex refused(4)" 4 error \
    python3 "$CLIENT" "$DIAG_SOCK" "force-wake scope=mutex"
run_expect "force-wake:非法 scope(3)" 3 error \
    python3 "$CLIENT" "$DIAG_SOCK" "force-wake scope=bogus"
run_expect "mutex-unlock:owner 不可证明(3)" 3 error \
    python3 "$CLIENT" "$DIAG_SOCK" "mutex-unlock name=demo-mutex"
run_expect "inject-fiber:未注册函数(3)" 3 error \
    python3 "$CLIENT" "$DIAG_SOCK" "inject-fiber fn=no-such-function"
run_expect "unknown:未知命令(3)" 3 error \
    python3 "$CLIENT" "$DIAG_SOCK" bogus-command

# force-wake all 恢复丢唤醒。
expect_ok "force-wake:scope=all" normal "verdict=ok" \
    python3 "$CLIENT" "$DIAG_SOCK" "force-wake scope=all"
RECOVERED=0
for _ in $(seq 1 100); do
    grep -q "RECOVERED" "$DIAG_LOG" 2>/dev/null && RECOVERED=1 && break
    sleep 0.05
done
if [ "$RECOVERED" -eq 1 ]; then
    pass "force-wake:丢唤醒进程恢复" normal
else
    fail "force-wake:丢唤醒进程恢复" normal "日志未出现 RECOVERED"
fi
expect_ok "stats:concurrent_peak=1" normal "concurrent_peak=1" \
    python3 "$CLIENT" "$DIAG_SOCK" stats
expect_ok "stats:deadlock_events 已计数" normal "deadlock_events=" \
    python3 "$CLIENT" "$DIAG_SOCK" stats

# 并发（doctor_client 自带线程探针，响应按 stream_id 匹配）。
for n in 10 50 100; do
    OUT="$(timeout 60 python3 "$CLIENT" "$DIAG_SOCK" --concurrency "$n" --command status 2>&1)"; CODE=$?
    if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -q "PASS concurrency=$n"; then
        pass "concurrency:${n} 条命令串行执行" concurrency "$(echo "$OUT" | tail -1)"
    else
        fail "concurrency:${n} 条命令串行执行" concurrency "exit=$CODE $(echo "$OUT" | tail -1)"
    fi
done
expect_ok "stats:并发后仍 concurrent_peak=1" normal "concurrent_peak=1" \
    python3 "$CLIENT" "$DIAG_SOCK" stats

# 协议负路径：坏校验/截断/超长；之后 status 必须仍可用。
python3 "$HERE/doctor_frames.py" "$DIAG_SOCK" bad-checksum >/dev/null 2>&1
expect_ok "protocol:坏校验后服务仍可用" error "schema=go2cpp.diagnostic.status/1" \
    python3 "$CLIENT" "$DIAG_SOCK" status
python3 "$HERE/doctor_frames.py" "$DIAG_SOCK" truncated >/dev/null 2>&1
expect_ok "protocol:截断帧后服务仍可用" error "schema=go2cpp.diagnostic.status/1" \
    python3 "$CLIENT" "$DIAG_SOCK" status
OVER_OUT="$(timeout 10 python3 "$HERE/doctor_frames.py" "$DIAG_SOCK" oversized 2>&1)"
expect_contains "protocol:超长命令返回错误帧" error "command-too-long" "$OVER_OUT"
expect_ok "protocol:超长命令后服务仍可用" error "schema=go2cpp.diagnostic.status/1" \
    python3 "$CLIENT" "$DIAG_SOCK" status
python3 "$HERE/doctor_frames.py" "$DIAG_SOCK" bad-magic >/dev/null 2>&1
expect_ok "protocol:坏 magic 后服务仍可用" error "schema=go2cpp.diagnostic.status/1" \
    python3 "$CLIENT" "$DIAG_SOCK" status

# CLI doctor 客户端（同一 G2CD/1 协议）。
expect_ok "cli-doctor:status" normal "schema=go2cpp.diagnostic.status/1" \
    "${CLI_DOCTOR[@]}" status
expect_ok "cli-doctor:并发 10" normal "PASS concurrency=10" \
    "${CLI_DOCTOR[@]}" --concurrency 10 --command status

# SIGTERM：进程应响应并退出（socket 清理由 demo 决定，不在此断言）。
kill -TERM "$DIAG_PID" 2>/dev/null || true
EXITED=0
for _ in $(seq 1 100); do
    if ! kill -0 "$DIAG_PID" 2>/dev/null; then EXITED=1; break; fi
    sleep 0.02
done
if [ "$EXITED" -eq 1 ]; then
    pass "sigterm:Doctor demo 响应 SIGTERM" inject
else
    fail "sigterm:Doctor demo 响应 SIGTERM" inject "3s 内未退出"
    stop_pid "$DIAG_PID"
fi
DIAG_PID=""

summary
