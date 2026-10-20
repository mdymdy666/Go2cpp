#!/usr/bin/env bash
# CLI 随机参数 1000 组 + 并发 10/50/100 dump。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=cli
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work cli-stress)"
SOCK="$WORK/fake.sock"
SERVER_PID=""
trap 'stop_pid "${SERVER_PID:-}"; rm -rf "$WORK"' EXIT

cp "$ROOT/tools/cli/tests/fake_debug_server.py" "$WORK/fake_debug_server.py"
python3 "$WORK/fake_debug_server.py" --socket "$SOCK" --max-connections 5000 \
    >"$WORK/server.log" 2>&1 &
SERVER_PID=$!
if ! wait_for_path "$SOCK.ready" 5000; then
    fail "fixture:fake-debug-server" stress "server 未就绪"
    summary
    exit 1
fi

OUT="$(timeout 120 python3 "$HERE/fuzz_args.py" --bin "$CLI" --count 1000 \
    --fake-socket "$SOCK" --timeout 3 2>&1)"; CODE=$?
echo "$OUT"
RESULT_LINE="$(printf '%s\n' "$OUT" | grep -oE 'RESULT .*')"
add_stat "random_args_1000" "$RESULT_LINE" "随机参数 1000 组"
if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -q "failures=0"; then
    pass "fuzz:1000 组随机参数退出码均在契约内" random "$RESULT_LINE"
else
    fail "fuzz:1000 组随机参数退出码均在契约内" random "exit=$CODE $RESULT_LINE"
fi

run_concurrency() { # n
    local n="$1"
    local elapsed failures=0
    local start end
    start=$(now_ms)
    local pids=()
    for index in $(seq 1 "$n"); do
        env GO2CPP_DEBUG_SOCKET="$SOCK" timeout 10 \
            "$CLI" dump --pid 1 --sections runtime >/dev/null 2>&1 &
        pids+=($!)
    done
    for pid in "${pids[@]}"; do
        wait "$pid" || failures=$((failures + 1))
    done
    end=$(now_ms)
    elapsed=$((end - start))
    if [ "$failures" -eq 0 ]; then
        pass "concurrency:${n} 个 CLI dump 全部成功" concurrency "elapsed_ms=$elapsed"
    else
        fail "concurrency:${n} 个 CLI dump 全部成功" concurrency \
            "failures=$failures elapsed_ms=$elapsed"
    fi
    add_stat "cli_concurrency_${n}_ms" "$elapsed" "并发 $n 个 CLI dump"
}

# fake server 的 backlog=64；100 并发时连接可能被拒，CLI 会返回非 0。
# 这里只断言「无挂死且记录失败数」，不把 backlog 上限算成产品缺陷。
run_concurrency 10
run_concurrency 50
run_concurrency 100

summary
