#!/usr/bin/env bash
# CLI 能力增强测试矩阵（202 断言的补充：真实 socket、协议负路径、后端分派）。
# 随机 1000 组与并发 10/50/100 见 stress_cli.sh。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=cli
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work cli)"
SOCK="$WORK/fake.sock"
SERVER_PID=""
trap 'stop_pid "${SERVER_PID:-}"; rm -rf "$WORK"' EXIT

if [ ! -x "$CLI" ]; then
    fail "fixture:cli" normal "缺少 $CLI"
    summary
    exit 1
fi

cp "$ROOT/tools/cli/tests/fake_debug_server.py" "$WORK/fake_debug_server.py"
cp "$ROOT/tools/cli/tests/make_binaries.py" "$WORK/make_binaries.py"
python3 "$WORK/make_binaries.py" "$WORK" >/dev/null 2>&1
export FIXTURE_DIR="$WORK"

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
cat > "$WORK/fake_signal.sh" <<'APP'
#!/usr/bin/env bash
kill -TERM $$
sleep 5
APP
cat > "$WORK/fake_norecord.sh" <<'APP'
#!/usr/bin/env bash
printf 'no record written\n'
exit 0
APP
cat > "$WORK/fake_replay.sh" <<'APP'
#!/usr/bin/env bash
printf '{"status":"ok","replay_executed":true,"path":"%s"}\n' "${2:-}"
exit "${FAKE_BACKEND_EXIT:-0}"
APP
cat > "$WORK/fake_migrate.sh" <<'APP'
#!/usr/bin/env bash
if [ "${1:-}" = "dump" ]; then
    cp "${FIXTURE_DIR}/good.mig" "${2:-/tmp/x.mig}" || exit 1
    printf 'FAKE-MIGRATE dump %s\n' "${2:-}"
    exit 0
fi
if [ "${1:-}" = "restore" ]; then
    printf 'FAKE-MIGRATE restore %s\n' "${2:-}"
    exit "${FAKE_BACKEND_EXIT:-0}"
fi
exit 2
APP
chmod +x "$WORK"/*.sh

start_server() { # scenario max-connections [extra...]
    local scenario="$1" maximum="$2"
    shift 2
    stop_pid "${SERVER_PID:-}"
    SERVER_PID=""
    rm -f "$SOCK" "$SOCK.ready"
    python3 "$WORK/fake_debug_server.py" --socket "$SOCK" --scenario "$scenario" \
        --max-connections "$maximum" "$@" >"$WORK/server.log" 2>&1 &
    SERVER_PID=$!
    wait_for_path "$SOCK.ready" 5000
}

# ---------------- 帮助/参数 ----------------
run_expect "help:无参数" 0 normal "$CLI"
run_expect "help:--version" 0 normal "$CLI" --version
run_expect "error:未知命令" 2 error "$CLI" frobnicate
run_expect "error:dump 缺 --pid" 2 error "$CLI" dump
run_expect "error:pid 非整数" 2 error "$CLI" dump --pid abc
run_expect "error:pid=0" 2 error "$CLI" dump --pid 0
run_expect "error:未知选项" 2 error "$CLI" dump --pid 1 --bogus
run_expect "error:非法 format" 2 error "$CLI" dump --pid 1 --format xml
run_expect "error:非法 section" 2 error "$CLI" dump --pid 1 --sections foo

# ---------------- 真实 fake DebugServer ----------------
if start_server default 32; then
    expect_ok "status:协议与组件" normal "协议: GO2CPP-DEBUG/1" \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" status --pid 12345
    expect_ok "dump:JSON ALL" normal '"schema": "go2cpp.dump.all/1"' \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" dump --pid 12345
    expect_ok "dump:text ALL" normal "[component scheduler]" \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" dump --pid 12345 --format text
    expect_ok "dump:selection" normal '"schema": "go2cpp-cli.dump.selection/1"' \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" dump --pid 12345 --sections scheduler
    expect_ok "dump:写文件" normal "已写入" \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" dump --pid 12345 --out "$WORK/dump.json"
    stop_pid "$SERVER_PID"; SERVER_PID=""
else
    fail "fixture:fake-debug-server" normal "server 未就绪"
fi

# 未启用 DebugServer
run_expect "error:socket 不存在" 3 error \
    env GO2CPP_DEBUG_SOCKET="$WORK/missing.sock" "$CLI" dump --pid 1

# 远端错误 / 协议错误 / 截断 / 超时
if start_server default 4 --error-mode; then
    run_expect "error:远端 ERR 映射退出码 5" 5 error \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" status --pid 1
    stop_pid "$SERVER_PID"; SERVER_PID=""
fi
if start_server default 4 --bad-protocol; then
    run_expect "error:响应协议不匹配" 1 error \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" dump --pid 1
    stop_pid "$SERVER_PID"; SERVER_PID=""
fi
if start_server default 4 --truncate-payload; then
    run_expect "error:payload 截断" 1 error \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" dump --pid 1
    stop_pid "$SERVER_PID"; SERVER_PID=""
fi
if start_server default 4 --delay-ms 800; then
    run_expect "error:响应超时" 3 error \
        env GO2CPP_DEBUG_SOCKET="$SOCK" "$CLI" status --pid 1 --timeout-ms 300
    stop_pid "$SERVER_PID"; SERVER_PID=""
fi

# ---------------- replay record / play ----------------
run_expect "replay record:成功" 0 normal "$CLI" replay record "$WORK/fake_app.sh" \
    --out "$WORK/rec.g2cr"
expect_ok "replay record:记录文件" normal "程序退出码: 0" \
    "$CLI" replay record "$WORK/fake_app.sh" --out "$WORK/rec2.g2cr"
run_expect "replay record:子进程被信号 143" 143 error \
    "$CLI" replay record "$WORK/fake_signal.sh" --out "$WORK/sig.g2cr"
run_expect "replay record:未生成记录(3)" 3 error \
    "$CLI" replay record "$WORK/fake_norecord.sh" --out "$WORK/none.g2cr"
expect_ok "replay play:后端分派" normal '"replay_executed":true' \
    env GO2CPP_REPLAY_TOOL="$WORK/fake_replay.sh" "$CLI" replay play "$WORK/good.g2cr"
run_expect "replay play:无后端(3)" 3 error \
    env -u GO2CPP_REPLAY_TOOL "$CLI" replay play "$WORK/good.g2cr"

# ---------------- migrate 后端分派 ----------------
export GO2CPP_MIGRATE_TOOL="$WORK/fake_migrate.sh"
run_expect "migrate dump:后端成功" 0 normal "$CLI" migrate dump --pid "$$" --gid 1 \
    --out "$WORK/out.mig"
expect_ok "migrate dump:快照摘要" normal "迁移快照:" \
    "$CLI" migrate dump --pid "$$" --gid 1 --out "$WORK/out2.mig"
run_expect "migrate restore:后端成功" 0 normal "$CLI" migrate restore "$WORK/good.mig"
unset GO2CPP_MIGRATE_TOOL

# ---------------- debug 离线分析 ----------------
cat > "$WORK/dump.json" <<'JSON'
{"schema":"go2cpp.dump.scheduler/1","processor_count":4,
 "tasks":[{"id":1,"state":"kRunning"},{"id":2,"state":"kWaiting"}],
 "events":[{"seq":1,"type":"SPAWN"}]}
JSON
expect_ok "debug:JSON dump" normal "schema: go2cpp.dump.scheduler/1" \
    "$CLI" debug "$WORK/dump.json"
expect_ok "debug:G2CRPLY 记录" normal "记录头" "$CLI" debug "$WORK/good.g2cr"
: > "$WORK/empty.bin"
run_expect "debug:未知格式(4)" 4 error "$CLI" debug "$WORK/empty.bin"

summary
