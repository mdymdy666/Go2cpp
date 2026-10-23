#!/usr/bin/env bash
# go2cpp-cli graph 验收：五种图 × 三种格式、死锁环高亮、--replay、
# 数据源降级、参数错误、DOT 渲染。
#
# 用法: BIN=/path/to/go2cpp-cli bash run_graph.sh
set -u

BIN="${BIN:-/tmp/go2cpp-cli-build/tools/cli/go2cpp-cli}"
TEST_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d /tmp/graph-test.XXXXXX)"
DEBUG_SOCK="$WORK/debug.sock"
DOCTOR_SOCK="$WORK/doctor.sock"
PASS=0
FAIL=0
FAILED=()
LAST_OUTPUT=""
SERVER_PID=""

cleanup() {
    stop_server
    rm -rf "$WORK"
}
trap cleanup EXIT

say() { printf '\n==== %s ====\n' "$1"; }
pass() { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
fail() {
    FAIL=$((FAIL + 1))
    FAILED+=("$1")
    printf 'FAIL  %s\n' "$1"
}

check() { # name expected actual output
    local name="$1" expected="$2" actual="$3" output="$4"
    if [ "$actual" -eq "$expected" ]; then
        pass "$name (exit $actual)"
    else
        fail "$name (expected exit $expected, actual $actual)"
        printf '%s\n' "$output" | sed 's/^/      | /'
    fi
}

run_expect() { # name expected args...
    local name="$1" expected="$2"
    shift 2
    LAST_OUTPUT="$("$BIN" "$@" 2>&1)"
    local code=$?
    check "$name" "$expected" "$code" "$LAST_OUTPUT"
}

expect_contains() { # name needle output
    local name="$1" needle="$2" output="$3"
    if printf '%s' "$output" | grep -qF -- "$needle"; then
        pass "$name"
    else
        fail "$name (missing: $needle)"
        printf '%s\n' "$output" | sed 's/^/      | /'
    fi
}

expect_missing() { # name needle output
    local name="$1" needle="$2" output="$3"
    if printf '%s' "$output" | grep -qF -- "$needle"; then
        fail "$name (unexpected: $needle)"
        printf '%s\n' "$output" | sed 's/^/      | /'
    else
        pass "$name"
    fi
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

start_server() { # [--only debug|doctor]
    local only="${1:-}"
    stop_server
    rm -f "$DEBUG_SOCK" "$DEBUG_SOCK.ready" "$DOCTOR_SOCK" "$DOCTOR_SOCK.ready"
    local args=(--debug-socket "$DEBUG_SOCK" --doctor-socket "$DOCTOR_SOCK")
    if [ -n "$only" ]; then
        args+=(--only "$only")
    fi
    python3 "$WORK/graph_fake_server.py" "${args[@]}" &
    SERVER_PID=$!
    local i
    for ((i = 0; i < 300; i++)); do
        local debug_ready=0 doctor_ready=0
        [ -e "$DEBUG_SOCK.ready" ] && debug_ready=1
        [ -e "$DOCTOR_SOCK.ready" ] && doctor_ready=1
        if [ "$only" = "debug" ] && [ "$debug_ready" -eq 1 ]; then
            return 0
        fi
        if [ "$only" = "doctor" ] && [ "$doctor_ready" -eq 1 ]; then
            return 0
        fi
        if [ -z "$only" ] && [ "$debug_ready" -eq 1 ] && [ "$doctor_ready" -eq 1 ]; then
            return 0
        fi
        sleep 0.01
    done
    printf 'fake server 未就绪\n' >&2
    return 1
}

graph() { # type format [extra...]
    local type="$1" format="$2"
    shift 2
    "$BIN" graph --pid $$ --debug-socket "$DEBUG_SOCK" \
        --doctor-socket "$DOCTOR_SOCK" --type "$type" --format "$format" "$@"
}

if [ ! -x "$BIN" ]; then
    printf 'go2cpp-cli 不存在或不可执行: %s\n' "$BIN" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    printf '需要 python3\n' >&2
    exit 2
fi

cp "$TEST_SRC/graph_fake_server.py" "$WORK/graph_fake_server.py"
python3 "$WORK/graph_fake_server.py" --dump-fixtures "$WORK/fixtures"

# ---------------- 1. 帮助与参数 ----------------
say "帮助与参数"
run_expect "graph --help" 0 graph --help
expect_contains "帮助含用法" "用法:" "$LAST_OUTPUT"
expect_contains "帮助含五种图" "wait-for" "$LAST_OUTPUT"
run_expect "help graph" 0 help graph
expect_contains "help graph 含降级说明" "降级" "$LAST_OUTPUT"
run_expect "缺少 --type" 2 graph --pid $$
expect_contains "缺少 --type 提示" "缺少必需选项 '--type'" "$LAST_OUTPUT"
run_expect "缺少 pid/replay" 2 graph --type wait-for
expect_contains "缺少 pid/replay 提示" "需要 --pid" "$LAST_OUTPUT"
run_expect "pid+replay 互斥" 2 graph --pid $$ --replay /tmp/x --type wait-for
expect_contains "pid+replay 提示" "只能二选一" "$LAST_OUTPUT"
run_expect "未知 type" 2 graph --pid $$ --type nope
expect_contains "未知 type 提示" "只接受 wait-for" "$LAST_OUTPUT"
run_expect "未知 format" 2 graph --pid $$ --type wait-for --format svg
expect_contains "未知 format 提示" "只接受 mermaid|dot|json" "$LAST_OUTPUT"
run_expect "多余位置参数" 2 graph --pid $$ --type wait-for extra
expect_contains "多余参数提示" "多余" "$LAST_OUTPUT"

# ---------------- 2. wait-for ----------------
say "wait-for（死锁环高亮）"
if start_server; then
    LAST_OUTPUT="$(graph wait-for mermaid 2>&1)"
    check "wait-for mermaid" 0 $? "$LAST_OUTPUT"
    expect_contains "wait-for 含 Doctor 资源名" "res-a" "$LAST_OUTPUT"
    expect_contains "wait-for 含等待者 G12" "G12" "$LAST_OUTPUT"
    expect_contains "wait-for 死锁 class" "deadlock" "$LAST_OUTPUT"
    expect_contains "wait-for 死锁 linkStyle" "linkStyle" "$LAST_OUTPUT"
    expect_contains "wait-for 环说明" "等待环" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph wait-for dot 2>&1)"
    check "wait-for dot" 0 $? "$LAST_OUTPUT"
    expect_contains "dot 有向图" "digraph go2cpp_graph" "$LAST_OUTPUT"
    expect_contains "dot 死锁颜色" "color=\"#c00000\"" "$LAST_OUTPUT"
    printf '%s\n' "$LAST_OUTPUT" > "$WORK/wait-for.dot"
    if command -v dot >/dev/null 2>&1; then
        if dot -Tsvg "$WORK/wait-for.dot" > "$WORK/wait-for.svg" 2> "$WORK/dot.err"; then
            pass "wait-for.dot 可被 dot -Tsvg 渲染"
            expect_contains "SVG 含图内容" "<svg" "$(cat "$WORK/wait-for.svg")"
        else
            fail "wait-for.dot 可被 dot -Tsvg 渲染"
            head -3 "$WORK/dot.err"
        fi
    else
        printf 'SKIP  wait-for.dot（本机无 dot）\n'
    fi

    LAST_OUTPUT="$(graph wait-for json 2>&1)"
    check "wait-for json" 0 $? "$LAST_OUTPUT"
    if printf '%s' "$LAST_OUTPUT" | python3 -c '
import json, sys
data = json.load(sys.stdin)
assert data["schema"] == "go2cpp.graph.wait-for/1"
assert data["nodes"] and data["edges"]
assert data["deadlock_cycles"], "no cycle"
assert any(edge["deadlock"] for edge in data["edges"])
'; then
        pass "wait-for json 结构（nodes/edges/环/deadlock 标记）"
    else
        fail "wait-for json 结构（nodes/edges/环/deadlock 标记）"
        printf '%s\n' "$LAST_OUTPUT" | head -20
    fi

    # ---------------- 3. resource ----------------
    say "resource"
    LAST_OUTPUT="$(graph resource mermaid 2>&1)"
    check "resource mermaid" 0 $? "$LAST_OUTPUT"
    expect_contains "resource 含空闲 mutex #4" "kMutex-4" "$LAST_OUTPUT"
    expect_contains "resource 含 PC" "kParkingCondition-5" "$LAST_OUTPUT"
    expect_contains "resource 含等待者计数" "waiters=1" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph resource dot 2>&1)"
    check "resource dot" 0 $? "$LAST_OUTPUT"
    expect_contains "resource dot 含节点" "kMutex#4" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph resource json 2>&1)"
    check "resource json" 0 $? "$LAST_OUTPUT"
    if printf '%s' "$LAST_OUTPUT" | python3 -c '
import json, sys
data = json.load(sys.stdin)
assert data["schema"] == "go2cpp.graph.resource/1"
assert len(data["nodes"]) >= 5
assert any(edge["kind"] == "wait" for edge in data["edges"])
'; then
        pass "resource json 结构"
    else
        fail "resource json 结构"
    fi

    # ---------------- 4. fiber-state ----------------
    say "fiber-state"
    LAST_OUTPUT="$(graph fiber-state mermaid 2>&1)"
    check "fiber-state mermaid" 0 $? "$LAST_OUTPUT"
    expect_contains "fiber-state created" "created" "$LAST_OUTPUT"
    expect_contains "fiber-state parked" "parked" "$LAST_OUTPUT"
    expect_contains "fiber-state ready" "ready" "$LAST_OUTPUT"
    expect_contains "fiber-state completed" "completed" "$LAST_OUTPUT"
    expect_contains "fiber-state 含 G11" "G11" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph fiber-state dot 2>&1)"
    check "fiber-state dot" 0 $? "$LAST_OUTPUT"
    expect_contains "fiber-state dot 节点" "G11" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph fiber-state json 2>&1)"
    check "fiber-state json" 0 $? "$LAST_OUTPUT"
    if printf '%s' "$LAST_OUTPUT" | python3 -c '
import json, sys
data = json.load(sys.stdin)
assert data["schema"] == "go2cpp.graph.fiber-state/1"
assert data["nodes"] and data["edges"]
assert all(edge["kind"] == "transition" for edge in data["edges"])
'; then
        pass "fiber-state json 结构"
    else
        fail "fiber-state json 结构"
    fi

    # ---------------- 5. gmp ----------------
    say "gmp"
    LAST_OUTPUT="$(graph gmp mermaid 2>&1)"
    check "gmp mermaid" 0 $? "$LAST_OUTPUT"
    expect_contains "gmp 含 runqueue" "global runqueue" "$LAST_OUTPUT"
    expect_contains "gmp 含 P0" "P0" "$LAST_OUTPUT"
    expect_contains "gmp 含 M0" "M0" "$LAST_OUTPUT"
    expect_contains "gmp 含 blocked G" "blocked" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph gmp dot 2>&1)"
    check "gmp dot" 0 $? "$LAST_OUTPUT"
    expect_contains "gmp dot 含 M" "M0" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph gmp json 2>&1)"
    check "gmp json" 0 $? "$LAST_OUTPUT"
    if printf '%s' "$LAST_OUTPUT" | python3 -c '
import json, sys
data = json.load(sys.stdin)
assert data["schema"] == "go2cpp.graph.gmp/1"
assert any(node["kind"] == "processor" for node in data["nodes"])
assert any(node["kind"] == "machine" for node in data["nodes"])
assert any(edge["kind"] == "runqueue" for edge in data["edges"])
'; then
        pass "gmp json 结构"
    else
        fail "gmp json 结构"
    fi

    # ---------------- 6. sequence ----------------
    say "sequence"
    LAST_OUTPUT="$(graph sequence mermaid 2>&1)"
    check "sequence mermaid" 0 $? "$LAST_OUTPUT"
    expect_contains "sequenceDiagram" "sequenceDiagram" "$LAST_OUTPUT"
    expect_contains "sequence participant" "participant R1" "$LAST_OUTPUT"
    expect_contains "sequence 资源事件" "lock" "$LAST_OUTPUT"
    expect_contains "sequence blocked" "blocked" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph sequence dot 2>&1)"
    check "sequence dot（时间线）" 0 $? "$LAST_OUTPUT"
    expect_contains "sequence dot 时间线" "m0" "$LAST_OUTPUT"

    LAST_OUTPUT="$(graph sequence json 2>&1)"
    check "sequence json" 0 $? "$LAST_OUTPUT"
    if printf '%s' "$LAST_OUTPUT" | python3 -c '
import json, sys
data = json.load(sys.stdin)
assert data["schema"] == "go2cpp.graph.sequence/1"
assert data["sequence"]["participants"]
assert data["sequence"]["messages"]
assert any("lock" in message["label"] for message in data["sequence"]["messages"])
'; then
        pass "sequence json 结构"
    else
        fail "sequence json 结构"
    fi

    # ---------------- 7. --out ----------------
    say "--out"
    LAST_OUTPUT="$("$BIN" graph --pid $$ --debug-socket "$DEBUG_SOCK" \
        --doctor-socket "$DOCTOR_SOCK" --type wait-for --format mermaid \
        --out "$WORK/wait-for.mmd" 2>&1)"
    check "--out 写文件" 0 $? "$LAST_OUTPUT"
    if [ -s "$WORK/wait-for.mmd" ] && grep -q "graph LR" "$WORK/wait-for.mmd"; then
        pass "--out 文件内容正确"
    else
        fail "--out 文件内容正确"
    fi
    stop_server
else
    fail "假数据源启动失败"
fi

# ---------------- 8. --replay ----------------
say "--replay"
run_expect "replay sched -> fiber-state" 0 graph --replay "$WORK/fixtures/sched.json" \
    --type fiber-state --format mermaid
expect_contains "replay fiber-state 内容" "created" "$LAST_OUTPUT"
run_expect "replay history -> sequence" 0 graph --replay "$WORK/fixtures/history1.json" \
    --type sequence --format mermaid
expect_contains "replay sequence 内容" "lock" "$LAST_OUTPUT"
run_expect "replay gmp -> gmp" 0 graph --replay "$WORK/fixtures/gmp.json" \
    --type gmp --format mermaid
expect_contains "replay gmp 内容" "P0" "$LAST_OUTPUT"
run_expect "replay resources -> resource" 0 graph --replay "$WORK/fixtures/resources.json" \
    --type resource --format mermaid
expect_contains "replay resource 内容" "kMutex-4" "$LAST_OUTPUT"
run_expect "replay trace -> fiber-state" 0 graph --replay "$WORK/fixtures/run.trace" \
    --type fiber-state --format mermaid
expect_contains "replay trace 内容" "G11" "$LAST_OUTPUT"
run_expect "replay 二进制记录拒绝" 4 graph --replay "$WORK/fixtures/record.g2cr" \
    --type sequence
expect_contains "replay 二进制提示" "只支持 JSON" "$LAST_OUTPUT"
run_expect "replay 文件不存在" 1 graph --replay "$WORK/nope.json" --type fiber-state
run_expect "replay trace 作 wait-for" 3 graph --replay "$WORK/fixtures/run.trace" \
    --type wait-for
expect_contains "trace wait-for 提示" "需要" "$LAST_OUTPUT"

# ---------------- 9. 降级 ----------------
say "降级"
if start_server doctor; then
    LAST_OUTPUT="$(graph wait-for mermaid 2>&1)"
    check "仅 Doctor 时 wait-for 仍输出" 0 $? "$LAST_OUTPUT"
    expect_contains "仅 Doctor 提示" "Doctor: ok" "$LAST_OUTPUT"
    expect_contains "仅 Doctor 含 owner 边" "held" "$LAST_OUTPUT"
    stop_server
else
    fail "仅 Doctor 假数据源启动失败"
fi
if start_server debug; then
    LAST_OUTPUT="$(graph wait-for mermaid 2>&1)"
    check "仅 DebugServer 时 wait-for 仍输出" 0 $? "$LAST_OUTPUT"
    expect_contains "仅 Debug 环（事件推导）" "deadlock" "$LAST_OUTPUT"
    expect_contains "仅 Debug 提示" "Doctor: 不可用" "$LAST_OUTPUT"
    stop_server
else
    fail "仅 DebugServer 假数据源启动失败"
fi
LAST_OUTPUT="$("$BIN" graph --pid $$ --debug-socket "$WORK/absent.sock" \
    --doctor-socket "$WORK/absent-doctor.sock" --type wait-for --format mermaid 2>&1)"
check "无数据源仍输出图并退 3" 3 $? "$LAST_OUTPUT"
expect_contains "无数据源提示" "需要 Doctor 或 DebugServer" "$LAST_OUTPUT"

# ---------------- 汇总 ----------------
say "汇总"
printf 'PASS=%d FAIL=%d\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
    printf '失败用例:\n'
    printf '  - %s\n' "${FAILED[@]}"
    exit 1
fi
exit 0
