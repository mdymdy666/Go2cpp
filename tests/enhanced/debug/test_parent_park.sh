#!/usr/bin/env bash
# P0 场景：父 Fiber park 后全量 dump 的可见性/状态/父链。
#
# 构造：父 G spawn 子 G 后 park 在无缓冲 channel Recv；子 G 也 park。
# 检查：
#   1. dump --sections scheduler 的 tasks 中父/子是否仍在；
#   2. 父 G 状态（GState 合法值是 kWaiting；出现 kDead/kFailed 即 P0）；
#   3. tasks 是否携带 parent_id/stack（缺 → P2 能力缺口）；
#   4. 按 gid 请求 DumpFiber 是否有入口（缺 → P2 能力缺口）。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=debug
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work parent-park)"
SOCK="$WORK/parent.sock"
LOG="$WORK/parent.log"
BIN="$WORK/fixture_parent_park"
FIXTURE_PID=""
trap 'stop_pid "${FIXTURE_PID:-}"; rm -rf "$WORK"' EXIT

# 编译测试夹具（只编译测试代码，不修改核心源码）。
if ! g++ -std=c++17 -O1 -g -pthread \
    -DGO2CPP_DEBUG_DUMP=1 \
    -DGO2CPP_DEBUG_EVENT_CAPACITY=256 \
    -DGO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY=64 \
    -I"$ROOT/include" -I"$ROOT/third_party" \
    "$HERE/fixture_parent_park.cpp" -o "$BIN" \
    -Wl,--no-as-needed -Wl,-rpath,"$ROOT/build-debug" \
    "$ROOT"/build-debug/libgo2cpp_*.so -ldl -lm -lz \
    >"$WORK/compile.log" 2>&1; then
    fail "fixture:compile" normal "g++ 失败：$(tail -3 "$WORK/compile.log" | tr '\n' '|')"
    summary
    exit 1
fi

"$BIN" "$SOCK" 120 >"$LOG" 2>&1 &
FIXTURE_PID=$!
if ! wait_for_line "$LOG" "READY" 6000; then
    fail "fixture:ready" normal "夹具未 READY：$(tail -5 "$LOG" | tr '\n' '|')"
    summary
    exit 1
fi
READY_LINE="$(grep -m1 'READY' "$LOG")"
PARENT_GID="$(printf '%s' "$READY_LINE" | sed -n 's/.*parent_gid=\([0-9][0-9]*\).*/\1/p')"
CHILD_GID="$(printf '%s' "$READY_LINE" | sed -n 's/.*child_gid=\([0-9][0-9]*\).*/\1/p')"
if [ -n "$PARENT_GID" ] && [ -n "$CHILD_GID" ] && [ "$CHILD_GID" != "0" ]; then
    pass "fixture:父/子 G 已启动" normal "parent=$PARENT_GID child=$CHILD_GID"
else
    fail "fixture:父/子 G 已启动" normal "READY=$READY_LINE"
    summary
    exit 1
fi

# 全量 dump（CLI 走 DebugServer，内部 DumpScheduler(include_fibers=true)）。
DUMP_JSON="$WORK/scheduler.json"
OUT="$(env GO2CPP_DEBUG_SOCKET="$SOCK" timeout 10 "$CLI" dump --pid "$FIXTURE_PID" \
    --sections scheduler 2>&1)"; CODE=$?
printf '%s' "$OUT" > "$DUMP_JSON"
if [ "$CODE" -ne 0 ]; then
    fail "dump:全量 scheduler" normal "exit=$CODE $(printf '%s' "$OUT" | head -3 | tr '\n' '|')"
    summary
    exit 1
fi
pass "dump:全量 scheduler" normal

CHECK_OUT="$(python3 "$HERE/check_parent_dump.py" "$DUMP_JSON" "$PARENT_GID" "$CHILD_GID" 2>&1)"
CHECK_CODE=$?
echo "$CHECK_OUT"
if printf '%s' "$CHECK_OUT" | grep -q "parent_present=1"; then
    pass "P0:父 G 仍在 tasks" normal
else
    fail "P0:父 G 仍在 tasks" concurrency "父 G 从 DebugSnapshotTasks 消失（期望可见）"
fi
if printf '%s' "$CHECK_OUT" | grep -q "child_present=1"; then
    pass "P0:子 G 仍在 tasks" normal
else
    fail "P0:子 G 仍在 tasks" concurrency "子 G 从 tasks 消失"
fi
PARENT_STATE="$(printf '%s' "$CHECK_OUT" | sed -n 's/.*parent_state=\([^ ]*\).*/\1/p')"
if [ "$PARENT_STATE" = "kWaiting" ]; then
    pass "P0:父 G park 后状态 kWaiting" concurrency
else
    fail "P0:父 G park 后状态 kWaiting" concurrency \
        "实际 state=${PARENT_STATE:-missing}；park 不应是 kDead/kCompleted"
fi
if printf '%s' "$CHECK_OUT" | grep -q "tasks_have_parent_id=1"; then
    pass "P1:tasks 携带 parent_id" normal
else
    gap "P2:tasks 缺少 parent_id" concurrency \
        "DumpScheduler.tasks schema 只有 id/state/queued/started/last_processor_id/task_class/failed/cancellation_requested；无父链"
fi
if printf '%s' "$CHECK_OUT" | grep -q "tasks_have_stack=1"; then
    pass "P1:tasks 携带栈信息" normal
else
    gap "P2:tasks 缺少栈信息" concurrency \
        "全量 scheduler dump 无 fiber 栈；DumpFiber 才能读栈"
fi
if [ "$CHECK_CODE" -eq 3 ]; then
    fail "P0:父/子可见性与状态" concurrency "checker 判定 P0（见上）"
fi

# 按父 gid 请求 DumpFiber：DebugServer 只有 RegisterFiber(name, Fiber&)，
# 没有 gid -> Fiber 的查询入口；CLI 也没有 --gid/--fiber 选项。
OUT="$(env GO2CPP_DEBUG_SOCKET="$SOCK" timeout 10 "$CLI" dump --pid "$FIXTURE_PID" \
    --sections fiber 2>&1)"; CODE=$?
if printf '%s' "$OUT" | grep -qF "未注册组件 'fiber'"; then
    gap "P2:按父 gid DumpFiber 无入口" concurrency \
        "dump --sections fiber 返回未注册；DumpFiber 只能由 RegisterFiber(name, Fiber&) 提供，无 gid 查询"
else
    fail "P2:按父 gid DumpFiber 无入口" concurrency \
        "exit=$CODE $(printf '%s' "$OUT" | head -2 | tr '\n' '|')"
fi

summary
