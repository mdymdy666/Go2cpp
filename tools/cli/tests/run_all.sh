#!/usr/bin/env bash
# go2cpp-cli 验收测试：帮助、错误路径、假 DebugServer（GO2CPP-DEBUG/1）集成、
# replay/migrate 后端分派、debug 离线分析、随机参数与压测。
#
# 用法: BIN=/path/to/go2cpp-cli bash run_all.sh
set -u

BIN="${BIN:-/tmp/go2cpp-cli-build/tools/cli/go2cpp-cli}"
TEST_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d /tmp/cli-test.XXXXXX)"
SOCKET="$WORK/fake.sock"
PASS=0
FAIL=0
FAILED=()
LAST_OUTPUT=""
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
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

start_server() { # [scenario] [delay_ms] [max_connections] [extra args...]
    local scenario="${1:-default}" delay="${2:-0}" count="${3:-16}"
    shift 3
    stop_server
    rm -f "$SOCKET" "$SOCKET.ready"
    python3 "$WORK/fake_debug_server.py" --socket "$SOCKET" --scenario "$scenario" \
        --delay-ms "$delay" --max-connections "$count" "$@" &
    SERVER_PID=$!
    local i
    for ((i = 0; i < 300; i++)); do
        if [ -e "$SOCKET.ready" ]; then
            return 0
        fi
        sleep 0.01
    done
    printf 'fake server 未就绪（scenario=%s）\n' "$scenario" >&2
    return 1
}

if [ ! -x "$BIN" ]; then
    printf 'go2cpp-cli 不存在或不可执行: %s\n' "$BIN" >&2
    exit 2
fi

cp "$TEST_SRC/fake_debug_server.py" "$WORK/fake_debug_server.py"
cp "$TEST_SRC/stress.py" "$WORK/stress.py"
cp "$TEST_SRC/make_binaries.py" "$WORK/make_binaries.py"
python3 "$WORK/make_binaries.py" "$WORK"

# ---------------- fixtures ----------------
cat > "$WORK/fake_app.sh" <<'APP'
#!/usr/bin/env bash
printf 'ENV_REPLAY=%s\n' "${GO2CPP_REPLAY:-}"
printf 'ENV_RECORD=%s\n' "${GO2CPP_REPLAY_RECORD:-}"
printf 'ARGS=%s\n' "$*"
if [ -n "${GO2CPP_REPLAY_RECORD:-}" ]; then
    printf '# fake record\n#1 event\n#2 event\n' > "$GO2CPP_REPLAY_RECORD"
fi
exit "${FAKE_APP_EXIT:-0}"
APP

cat > "$WORK/fake_norecord.sh" <<'APP'
#!/usr/bin/env bash
printf 'no record written\n'
exit "${FAKE_APP_EXIT:-0}"
APP

cat > "$WORK/fake_binary.sh" <<'APP'
#!/usr/bin/env bash
if [ -n "${GO2CPP_REPLAY_RECORD:-}" ]; then
    printf 'BIN\000ARY' > "$GO2CPP_REPLAY_RECORD"
fi
exit 0
APP

cat > "$WORK/fake_signal.sh" <<'APP'
#!/usr/bin/env bash
kill -TERM $$
sleep 5
APP

cat > "$WORK/fake_replay.sh" <<'APP'
#!/usr/bin/env bash
# 引擎契约：`play <file>`；stdout 只输出 JSON 报告（CLI 会解析 replay_executed），
# 人类可读信息一律走 stderr。
if [ "${1:-}" = "play" ]; then
    shift
fi
printf '{"schema":"go2cpp.replay.report/1","status":"ok","replay_executed":true}\n'
printf 'FAKE-REPLAY:%s\n' "${1:-}" >&2
printf 'FAKE-REPLAY-COUNT:%s\n' "$#" >&2
exit "${FAKE_BACKEND_EXIT:-0}"
APP

cat > "$WORK/fake_no_replay.sh" <<'APP'
#!/usr/bin/env bash
# 退化引擎：退出码 0 但声称没有执行重放（P1-2 的缺陷形态）。
printf '{"schema":"go2cpp.replay.report/1","status":"ok","replay_executed":false}\n'
exit 0
APP
chmod +x "$WORK/fake_no_replay.sh"

cat > "$WORK/fake_migrate.sh" <<'APP'
#!/usr/bin/env bash
mode="${1:-}"
path="${2:-}"
if [ "$mode" = "dump" ]; then
    cp "${FIXTURE_DIR}/good-v3.mig" "$path" || exit 1
    printf 'FAKE-MIGRATE dump %s\n' "$path"
    printf 'FAKE-MIGRATE pid=%s gid=%s\n' "${MIGRATION_PID:-}" "${MIGRATION_GID:-}"
    exit 0
fi
if [ "$mode" = "restore" ]; then
    printf 'FAKE-MIGRATE restore %s\n' "$path"
    exit "${FAKE_BACKEND_EXIT:-0}"
fi
printf 'FAKE-MIGRATE unknown mode %s\n' "$mode" >&2
exit 2
APP

chmod +x "$WORK/fake_app.sh" "$WORK/fake_norecord.sh" "$WORK/fake_binary.sh" \
    "$WORK/fake_signal.sh" "$WORK/fake_replay.sh" "$WORK/fake_migrate.sh"

export FIXTURE_DIR="$WORK"
unset GO2CPP_DEBUG_SOCKET GO2CPP_REPLAY_TOOL GO2CPP_MIGRATE_TOOL
unset FAKE_APP_EXIT FAKE_BACKEND_EXIT

# ---------------- 1. 帮助与全局 ----------------
say "帮助与全局入口"
run_expect "无参数显示主帮助" 0
expect_contains "主帮助标题" "go2cpp-cli — Go2Cpp 运行时统一命令行入口" "$LAST_OUTPUT"
expect_contains "主帮助含 dump" "dump      从运行中的进程抓取" "$LAST_OUTPUT"
run_expect "--help" 0 --help
run_expect "-h" 0 -h
run_expect "help" 0 help
run_expect "help dump" 0 help dump
expect_contains "help dump 用法" "用法: go2cpp-cli dump --pid <pid>" "$LAST_OUTPUT"
run_expect "help status" 0 help status
run_expect "help replay" 0 help replay
run_expect "help migrate" 0 help migrate
run_expect "help debug" 0 help debug
run_expect "help 未知命令" 2 help nope
run_expect "--version" 0 --version
expect_contains "版本号" "go2cpp-cli 0.1.0" "$LAST_OUTPUT"
run_expect "未知命令" 2 frobnicate
expect_contains "未知命令提示" "未知命令 'frobnicate'" "$LAST_OUTPUT"
run_expect "dump --help" 0 dump --help
expect_contains "dump help 选项" "--sections <LIST>" "$LAST_OUTPUT"
expect_contains "dump help 协议" "GO2CPP-DEBUG/1" "$LAST_OUTPUT"
run_expect "status --help" 0 status --help
expect_contains "status help 选项" "--json" "$LAST_OUTPUT"
run_expect "replay --help" 0 replay --help
expect_contains "replay help record" "replay record <exe>" "$LAST_OUTPUT"
run_expect "replay record --help" 0 replay record --help
run_expect "replay play --help" 0 replay play --help
run_expect "migrate --help" 0 migrate --help
expect_contains "migrate help dump" "migrate dump --pid <pid> --gid <gid>" "$LAST_OUTPUT"
run_expect "migrate dump --help" 0 migrate dump --help
run_expect "migrate restore --help" 0 migrate restore --help
run_expect "debug --help" 0 debug --help
expect_contains "debug help 选项" "--top <N>" "$LAST_OUTPUT"

# ---------------- 2. 参数错误 ----------------
say "参数错误（退出码 2）"
run_expect "dump 缺 --pid" 2 dump
expect_contains "dump 缺 pid 提示" "缺少必需选项 '--pid'" "$LAST_OUTPUT"
run_expect "dump --pid 非整数" 2 dump --pid abc
expect_contains "非整数提示" "不是合法整数" "$LAST_OUTPUT"
run_expect "dump --pid 0" 2 dump --pid 0
expect_contains "范围提示" "超出范围" "$LAST_OUTPUT"
run_expect "dump 未知选项" 2 dump --pid 1 --bogus
expect_contains "未知选项提示" "未知选项 '--bogus'" "$LAST_OUTPUT"
run_expect "dump 重复选项" 2 dump --pid 1 --pid 2
expect_contains "重复选项提示" "重复出现" "$LAST_OUTPUT"
run_expect "dump --pid 缺值" 2 dump --pid
run_expect "dump --pid 后接选项" 2 dump --pid --out /tmp/x
run_expect "dump 非法 format" 2 dump --pid 1 --format xml
expect_contains "非法 format 提示" "只接受 json 或 text" "$LAST_OUTPUT"
run_expect "dump 非法 section" 2 dump --pid 1 --sections foo
expect_contains "非法 section 提示" "未知 section 'foo'" "$LAST_OUTPUT"
run_expect "dump 多余位置参数" 2 dump --pid 1 extra
run_expect "status 多余位置参数" 2 status --pid 1 extra
run_expect "migrate dump 缺 --gid" 2 migrate dump --pid 1 --out /tmp/x
run_expect "migrate dump 缺 --out" 2 migrate dump --pid 1 --gid 1
run_expect "migrate 未知子命令" 2 migrate bogus
run_expect "replay 未知子命令" 2 replay bogus
run_expect "debug 缺文件" 2 debug
run_expect "debug 多余参数" 2 debug /tmp/x /tmp/y
run_expect "replay record 缺 --out" 2 replay record "$WORK/fake_app.sh"
expect_contains "缺 out 提示" "缺少必需选项 '--out <FILE>'" "$LAST_OUTPUT"
run_expect "replay record 未知选项" 2 replay record --bogus "$WORK/fake_app.sh" --out "$WORK/x.g2cr"
run_expect "replay record 缺 exe" 2 replay record --out "$WORK/x.g2cr"
run_expect "replay record --out -" 2 replay record "$WORK/fake_app.sh" --out -

# ---------------- 3. DebugServer 未启用 ----------------
say "DebugServer 未启用（退出码 3）"
run_expect "status 进程不存在" 3 status --pid 999999
expect_contains "进程不存在提示" "进程 999999 不存在" "$LAST_OUTPUT"
run_expect "status socket 不存在" 3 status --pid $$
expect_contains "status 未启用提示" "DebugServer 未启用" "$LAST_OUTPUT"
expect_contains "status 未启用含 socket 路径" "/tmp/go2cpp-$$.sock" "$LAST_OUTPUT"
run_expect "dump socket 不存在" 3 dump --pid $$
expect_contains "dump 未启用提示" "DebugServer 未启用" "$LAST_OUTPUT"
GO2CPP_DEBUG_SOCKET="$WORK/missing.sock"
export GO2CPP_DEBUG_SOCKET
run_expect "覆盖 socket 不存在" 3 dump --pid 12345
expect_contains "覆盖 socket 提示" "$WORK/missing.sock" "$LAST_OUTPUT"
unset GO2CPP_DEBUG_SOCKET

# ---------------- 4. 假 DebugServer（GO2CPP-DEBUG/1） ----------------
say "假 DebugServer：STATUS / JSON scheduler / JSON ALL / DUMP ALL"
export GO2CPP_DEBUG_SOCKET="$SOCKET"

start_server default 0 16
run_expect "status 正常响应" 0 status --pid 12345
expect_contains "status 协议" "协议: GO2CPP-DEBUG/1" "$LAST_OUTPUT"
expect_contains "status 服务信息" "DebugServer: true，uptime 1.23 s，组件 3 个" "$LAST_OUTPUT"
expect_contains "status 调度器" "调度器: processors=4 workers=6 running=true" "$LAST_OUTPUT"
expect_contains "status 任务" "任务: 3 个（kRunning=1, kWaiting=2）" "$LAST_OUTPUT"
expect_contains "status runqueue" "runqueue: local=9 global=3 per_p=[3, 0, 2, 4]" "$LAST_OUTPUT"
expect_contains "status 调度指标" "task_runs=100000 completions=99900" "$LAST_OUTPUT"
expect_contains "status avg resume" "avg 8.8 us/run" "$LAST_OUTPUT"
expect_contains "status 内存说明" "DebugServer 未提供内存统计" "$LAST_OUTPUT"

start_server default 0 16
run_expect "status --json" 0 status --pid 12345 --json
if printf '%s' "$LAST_OUTPUT" | python3 -m json.tool >/dev/null 2>&1; then
    pass "status --json 输出合法 JSON"
else
    fail "status --json 输出合法 JSON"
fi
expect_contains "status --json 含 status" '"status"' "$LAST_OUTPUT"
expect_contains "status --json 含 scheduler" '"scheduler"' "$LAST_OUTPUT"

start_server partial 0 16
run_expect "status 部分字段缺失" 0 status --pid 12345
expect_contains "status 缺失 tasks" "任务: (scheduler 响应缺少 tasks 数组)" "$LAST_OUTPUT"
expect_contains "status 缺失计数" "注: 有" "$LAST_OUTPUT"

start_server no-scheduler 0 16
run_expect "status 未注册 scheduler" 0 status --pid 12345
expect_contains "未注册提示" "未注册 scheduler 组件" "$LAST_OUTPUT"

start_server default 0 16
run_expect "dump JSON ALL" 0 dump --pid 12345
expect_contains "dump all schema" '"schema": "go2cpp.dump.all/1"' "$LAST_OUTPUT"
expect_contains "dump components" '"components"' "$LAST_OUTPUT"
expect_contains "dump scheduler" '"processor_count": 4' "$LAST_OUTPUT"

start_server default 0 16
run_expect "dump 写文件" 0 dump --pid 12345 --out "$WORK/dump.json"
expect_contains "dump 写入提示" "已写入 $WORK/dump.json" "$LAST_OUTPUT"
if grep -q '"schema"' "$WORK/dump.json"; then
    pass "dump 文件内容为 JSON"
else
    fail "dump 文件内容为 JSON"
fi

start_server default 0 16
run_expect "dump text 格式" 0 dump --pid 12345 --format text
expect_contains "dump text schema" "schema=go2cpp.dump.all/1" "$LAST_OUTPUT"
expect_contains "dump text 组件段" "[component scheduler]" "$LAST_OUTPUT"

start_server default 0 16
run_expect "dump 指定组件 scheduler" 0 dump --pid 12345 --sections scheduler
expect_contains "dump selection schema" '"schema": "go2cpp-cli.dump.selection/1"' "$LAST_OUTPUT"
expect_contains "dump selection 组件" '"scheduler"' "$LAST_OUTPUT"

start_server default 0 16
run_expect "dump 指定 runtime text" 0 dump --pid 12345 --sections runtime --format text
expect_contains "dump runtime 段" "[component runtime]" "$LAST_OUTPUT"

start_server default 0 16
run_expect "dump 未注册组件 fiber" 3 dump --pid 12345 --sections goroutines
expect_contains "未注册 fiber 提示" "未注册组件 'fiber'" "$LAST_OUTPUT"

start_server default 0 16
export GO2CPP_CLI_TRACE=1
run_expect "协议 trace" 0 dump --pid 12345
expect_contains "trace 请求 JSON ALL" "-> JSON ALL" "$LAST_OUTPUT"
expect_contains "trace 响应头" "<- GO2CPP-DEBUG/1 OK" "$LAST_OUTPUT"
unset GO2CPP_CLI_TRACE

start_server default 0 16 --error-mode
run_expect "远端 ERR" 5 status --pid 12345
expect_contains "远端错误码" "[internal]" "$LAST_OUTPUT"
expect_contains "远端错误消息" "test-error" "$LAST_OUTPUT"

start_server default 0 16 --bad-protocol
run_expect "响应头协议不匹配" 1 dump --pid 12345
expect_contains "协议不匹配提示" "响应头不是 GO2CPP-DEBUG/1" "$LAST_OUTPUT"

start_server default 0 16 --truncate-payload
run_expect "payload 截断" 1 dump --pid 12345
expect_contains "截断提示" "提前关闭连接" "$LAST_OUTPUT"

start_server default 800 16
run_expect "响应超时" 3 status --pid 12345 --timeout-ms 300
expect_contains "超时提示" "等待 DebugServer 响应超时（300ms）" "$LAST_OUTPUT"

unset GO2CPP_DEBUG_SOCKET

# ---------------- 5. replay record ----------------
say "replay record"
run_expect "record 成功" 0 replay record "$WORK/fake_app.sh" --out "$WORK/rec.g2cr" a b
expect_contains "record 环境 GO2CPP_REPLAY" "ENV_REPLAY=ON" "$LAST_OUTPUT"
expect_contains "record 环境记录路径" "ENV_RECORD=$WORK/rec.g2cr" "$LAST_OUTPUT"
expect_contains "record 参数透传" "ARGS=a b" "$LAST_OUTPUT"
expect_contains "record 摘要" "记录文件: $WORK/rec.g2cr" "$LAST_OUTPUT"
expect_contains "record 行数" "3 行" "$LAST_OUTPUT"

run_expect "record 透传以 - 开头的参数" 0 replay record "$WORK/fake_app.sh" --out "$WORK/rec_flag.g2cr" --port 8080 -x
expect_contains "record flag 参数" "ARGS=--port 8080 -x" "$LAST_OUTPUT"

run_expect "record -- 分隔" 0 replay record --out "$WORK/rec_dd.g2cr" -- "$WORK/fake_app.sh" -x
expect_contains "record -- 后参数" "ARGS=-x" "$LAST_OUTPUT"

printf 'OLD-CONTENT\n' > "$WORK/replace.g2cr"
run_expect "record 拒绝覆盖已存在文件" 2 replay record "$WORK/fake_app.sh" --out "$WORK/replace.g2cr"
expect_contains "拒绝覆盖提示" "记录文件已存在，拒绝覆盖" "$LAST_OUTPUT"
if grep -q 'OLD-CONTENT' "$WORK/replace.g2cr"; then
    pass "record 旧文件未被删除"
else
    fail "record 旧文件未被删除"
fi

export FAKE_APP_EXIT=7
run_expect "record 透传子进程退出码" 7 replay record "$WORK/fake_app.sh" --out "$WORK/rec7.g2cr"
expect_contains "record 退出码输出" "程序退出码: 7" "$LAST_OUTPUT"
unset FAKE_APP_EXIT

run_expect "record 未生成记录文件" 3 replay record "$WORK/fake_norecord.sh" --out "$WORK/none.g2cr"
expect_contains "未生成记录提示" "没有生成重放记录文件" "$LAST_OUTPUT"
expect_contains "未生成记录待对齐" "GO2CPP_REPLAY 注入" "$LAST_OUTPUT"

run_expect "record 子进程被信号" 143 replay record "$WORK/fake_signal.sh" --out "$WORK/sig.g2cr"
expect_contains "信号提示" "被信号 15" "$LAST_OUTPUT"

run_expect "record 二进制记录" 0 replay record "$WORK/fake_binary.sh" --out "$WORK/bin.g2cr"
expect_contains "二进制提示" "二进制" "$LAST_OUTPUT"

run_expect "record exe 不存在" 1 replay record "$WORK/no-such-exe" --out "$WORK/x.g2cr"
expect_contains "exe 不存在提示" "找不到可执行文件" "$LAST_OUTPUT"

run_expect "record 输出目录不存在" 1 replay record "$WORK/fake_app.sh" --out "$WORK/no-dir/x.g2cr"
expect_contains "输出目录提示" "输出目录不存在" "$LAST_OUTPUT"

# ---------------- 6. replay play ----------------
say "replay play"
run_expect "play 无后端" 3 replay play "$WORK/good.g2cr"
expect_contains "play 无后端提示" "重放引擎未就绪" "$LAST_OUTPUT"
expect_contains "play 已校验说明" "已校验记录文件" "$LAST_OUTPUT"

GO2CPP_REPLAY_TOOL="$WORK/fake_replay.sh"
export GO2CPP_REPLAY_TOOL
run_expect "play 后端成功" 0 replay play "$WORK/good.g2cr"
expect_contains "play 后端参数" "FAKE-REPLAY:$WORK/good.g2cr" "$LAST_OUTPUT"
expect_contains "play 后端只收一个参数" "FAKE-REPLAY-COUNT:1" "$LAST_OUTPUT"
export FAKE_BACKEND_EXIT=9
run_expect "play 后端退出码透传" 9 replay play "$WORK/good.g2cr"
unset FAKE_BACKEND_EXIT GO2CPP_REPLAY_TOOL

GO2CPP_REPLAY_TOOL="$WORK/fake_no_replay.sh"
export GO2CPP_REPLAY_TOOL
run_expect "play 引擎未执行重放 → 3" 3 replay play "$WORK/good.g2cr"
expect_contains "play 未执行提示" "没有执行重放" "$LAST_OUTPUT"
unset GO2CPP_REPLAY_TOOL

run_expect "play 文件不存在" 1 replay play "$WORK/nope.g2cr"
printf '' > "$WORK/empty.g2cr"
run_expect "play 空文件" 4 replay play "$WORK/empty.g2cr"
expect_contains "play 空文件提示" "文件为空" "$LAST_OUTPUT"
printf 'hello world\n' > "$WORK/junk.g2cr"
run_expect "play 未知格式" 4 replay play "$WORK/junk.g2cr"
expect_contains "play 格式提示" "记录文件格式不匹配" "$LAST_OUTPUT"
printf '{"schema":"go2cpp.replay/1"}\n' > "$WORK/json.g2cr"
run_expect "play 拒绝 JSON" 4 replay play "$WORK/json.g2cr"
expect_contains "play JSON 提示" "期望重放记录（魔数 G2CRPLY" "$LAST_OUTPUT"

# ---------------- 7. migrate dump / restore ----------------
say "migrate dump / restore"
GO2CPP_MIGRATE_TOOL="$WORK/fake_migrate.sh"
export GO2CPP_MIGRATE_TOOL
run_expect "migrate dump 成功" 0 migrate dump --pid $$ --gid 42 --out "$WORK/out.mig"
expect_contains "migrate dump 快照摘要" "迁移快照:" "$LAST_OUTPUT"
expect_contains "migrate dump 后端输出" "FAKE-MIGRATE dump" "$LAST_OUTPUT"
expect_contains "migrate dump pid 透传" "FAKE-MIGRATE pid=$$ gid=42" "$LAST_OUTPUT"
expect_contains "migrate dump demo 提示" "不支持按 pid/gid" "$LAST_OUTPUT"
if head -c 8 "$WORK/out.mig" | grep -q 'G2CMIG03'; then
    pass "migrate dump 生成 G2CMIG03"
else
    fail "migrate dump 生成 G2CMIG03"
fi

run_expect "migrate dump 进程不存在" 3 migrate dump --pid 999999 --gid 1 --out "$WORK/x.mig"
expect_contains "migrate dump 进程提示" "进程 999999 不存在" "$LAST_OUTPUT"

run_expect "migrate dump --out -" 2 migrate dump --pid $$ --gid 1 --out -
unset GO2CPP_MIGRATE_TOOL

GO2CPP_MIGRATE_TOOL="$WORK/no-such-backend"
export GO2CPP_MIGRATE_TOOL
run_expect "migrate 后端不可执行" 3 migrate restore "$WORK/good.mig"
expect_contains "后端不可执行提示" "指向的后端不存在或不可执行" "$LAST_OUTPUT"
unset GO2CPP_MIGRATE_TOOL

run_expect "restore 无后端" 3 migrate restore "$WORK/good.mig"
expect_contains "restore 无后端提示" "迁移工具未就绪" "$LAST_OUTPUT"

GO2CPP_MIGRATE_TOOL="$WORK/fake_migrate.sh"
export GO2CPP_MIGRATE_TOOL
run_expect "restore 后端成功（v2 兼容读）" 0 migrate restore "$WORK/good.mig"
expect_contains "restore 后端参数" "FAKE-MIGRATE restore $WORK/good.mig" "$LAST_OUTPUT"
run_expect "restore 后端成功（v3）" 0 migrate restore "$WORK/good-v3.mig"
expect_contains "restore v3 后端参数" "FAKE-MIGRATE restore $WORK/good-v3.mig" "$LAST_OUTPUT"
export FAKE_BACKEND_EXIT=9
run_expect "restore 后端退出码透传" 9 migrate restore "$WORK/good.mig"
unset FAKE_BACKEND_EXIT GO2CPP_MIGRATE_TOOL

run_expect "restore 文件不存在" 1 migrate restore "$WORK/nope.mig"
run_expect "restore 未知格式" 4 migrate restore "$WORK/junk.g2cr"
expect_contains "restore 格式提示" "迁移文件格式不匹配" "$LAST_OUTPUT"
run_expect "restore 拒绝 v1" 4 migrate restore "$WORK/legacy_v1.mig"
expect_contains "restore v1 提示" "迁移文件格式不匹配" "$LAST_OUTPUT"
printf '' > "$WORK/empty.mig"
run_expect "restore 空文件" 4 migrate restore "$WORK/empty.mig"
run_expect "restore 拒绝不支持的版本" 4 migrate restore "$WORK/unsupported.mig"
expect_contains "不支持版本提示" "版本不支持" "$LAST_OUTPUT"

# ---------------- 8. debug ----------------
say "debug 离线分析"
cat > "$WORK/dump.json" <<'JSON'
{
  "schema": "go2cpp.dump.scheduler/1",
  "processor_count": 4,
  "tasks": [
    {"id": 1, "state": "kRunning"},
    {"id": 2, "state": "kWaiting"},
    {"id": 3, "state": "kWaiting"}
  ],
  "events": [
    {"seq": 1, "type": "SPAWN"},
    {"seq": 2, "type": "PARK"}
  ]
}
JSON
run_expect "debug JSON dump" 0 debug "$WORK/dump.json" --top 1
expect_contains "debug schema" "schema: go2cpp.dump.scheduler/1" "$LAST_OUTPUT"
expect_contains "debug 任务统计" "协程/任务: 3 条；状态: kRunning=1, kWaiting=2" "$LAST_OUTPUT"
expect_contains "debug 事件统计" "事件: 2 条；类型: PARK=1, SPAWN=1" "$LAST_OUTPUT"
expect_contains "debug 时间线" "时间线（前 1 条）" "$LAST_OUTPUT"

run_expect "debug --json" 0 debug "$WORK/dump.json" --json
if printf '%s' "$LAST_OUTPUT" | python3 -m json.tool >/dev/null 2>&1; then
    pass "debug --json 输出合法 JSON"
else
    fail "debug --json 输出合法 JSON"
fi

printf '# go2cpp-trace v1\n# seed=0x5f3a91\n# BEGIN\n#1  INPUT  SEL   gid=-  cand=[3,7,9]  pick=7\n#2  OUTPUT SPAWN gid=7  task=0x7f3a\n#3  OUTPUT PARK  gid=3  reason=chan_wait\n# END\n' > "$WORK/run.trace"
run_expect "debug trace 文本" 0 debug "$WORK/run.trace"
expect_contains "trace 记录数" "记录: 3 条（INPUT 1 / OUTPUT 2）" "$LAST_OUTPUT"
expect_contains "trace 事件类型" "PARK=1, SEL=1, SPAWN=1" "$LAST_OUTPUT"
expect_contains "trace 时间线" "#1  INPUT  SEL" "$LAST_OUTPUT"

printf '[scheduler]\nprocessor_count = 4\n[fiber.1]\nid = 1\nstate = Running\n' > "$WORK/dump.ini"
run_expect "debug ini" 0 debug "$WORK/dump.ini"
expect_contains "ini 统计" "节: 2 个；键值行: 3 行" "$LAST_OUTPUT"

run_expect "debug G2CRPLY" 0 debug "$WORK/good.g2cr"
expect_contains "replay 记录头" "记录头: version=1 header_size=32" "$LAST_OUTPUT"

run_expect "debug G2CMIG02（v2 兼容读）" 0 debug "$WORK/good.mig"
expect_contains "migration 快照头" "快照头: version=2 sections=2 fiber_id=42 stack_size=64.0 KiB" "$LAST_OUTPUT"
expect_contains "migration v2 格式" "G2CMIG02，版本 2" "$LAST_OUTPUT"
run_expect "debug G2CMIG03" 0 debug "$WORK/good-v3.mig"
expect_contains "migration v3 快照头" "快照头: version=3 sections=2 fiber_id=42 stack_size=64.0 KiB" "$LAST_OUTPUT"
expect_contains "migration v3 格式" "G2CMIG03，版本 3" "$LAST_OUTPUT"
run_expect "debug 拒绝 v1" 4 debug "$WORK/legacy_v1.mig"
expect_contains "debug v1 提示" "无法识别的文件格式" "$LAST_OUTPUT"

printf '\x00\x01\x02binary' > "$WORK/blob.bin"
run_expect "debug 未知格式" 4 debug "$WORK/blob.bin"
expect_contains "未知格式提示" "无法识别的文件格式" "$LAST_OUTPUT"
run_expect "debug 文件不存在" 1 debug "$WORK/nope.json"
printf '' > "$WORK/empty.bin"
run_expect "debug 空文件" 4 debug "$WORK/empty.bin"

# ---------------- 9. 随机参数与压测 ----------------
say "随机参数（500 组）与压测（各 200 次）"
unset GO2CPP_DEBUG_SOCKET GO2CPP_REPLAY_TOOL GO2CPP_MIGRATE_TOOL
if start_server default 0 500; then
    if python3 "$WORK/stress.py" "$BIN" "$WORK" "$SOCKET" > "$WORK/stress.json"; then
        pass "随机参数 500 组退出码均在契约内"
    else
        fail "随机参数出现契约外退出码或超时"
    fi
    stop_server
    cat "$WORK/stress.json"
    cp "$WORK/stress.json" /tmp/cli_stress_evidence.json 2>/dev/null || true
else
    fail "压测用假 server 启动失败"
fi

# ---------------- 10. graph（并发结构状态图） ----------------
# graph 有自己的假数据源（DebugServer VIEW + Doctor G2CD）与 91 项断言；
# 独立脚本运行，失败计入本脚本汇总。
say "graph（五图 × 三格式 / 环高亮 / --replay / 降级）"
unset GO2CPP_DEBUG_SOCKET GO2CPP_DOCTOR_SOCKET
if BIN="$BIN" bash "$TEST_SRC/run_graph.sh" > "$WORK/graph.log" 2>&1; then
    pass "graph 验收（$(grep -c '^PASS  ' "$WORK/graph.log") 项）"
else
    fail "graph 验收失败"
    tail -40 "$WORK/graph.log" | sed 's/^/      | /'
fi
grep '^PASS=' "$WORK/graph.log" | tail -1 | sed 's/^/      graph /'

# ---------------- 汇总 ----------------
say "汇总"
printf 'PASS=%d FAIL=%d\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
    printf '失败用例:\n'
    printf '  - %s\n' "${FAILED[@]}"
    exit 1
fi
exit 0
