#!/usr/bin/env bash
# Replay 能力增强测试矩阵（真实 tools/replay 二进制）。
#
# 覆盖：record/play/info/until/split、select、context 虚拟时钟、values、
#       截断/空文件/错格式/未知选项、丢唤醒注入、CLI 集成、资源限制。
# 500 轮随机交错与 100 轮压测见 stress_replay.sh。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=replay
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work replay)"
FANIN="$WORK/fanin.g2cr"
PARTS="$WORK/parts"
STUCK="$WORK/stuck.g2cr"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$REPLAY_BIN" ] || [ ! -x "$RECORD_BIN" ]; then
    fail "fixture:replay" normal "缺少 $REPLAY_BIN 或 $RECORD_BIN"
    summary
    exit 1
fi

# ---------------- 正常路径 ----------------
run_expect "fanin:record 96 次交接" 0 normal "$RECORD_BIN" "$FANIN"
if [ -s "$FANIN" ]; then
    pass "fanin:记录文件非空" normal "$(stat -c%s "$FANIN") bytes"
else
    fail "fanin:记录文件非空" normal "文件为空"
fi
expect_ok "fanin:info 静态校验" normal '"checksum_ok":true' "$REPLAY_BIN" "$FANIN"
expect_ok "fanin:info 事件数" normal '"file_events":394' "$REPLAY_BIN" "$FANIN"
expect_ok "fanin:play 完整重放" normal '"replay_executed":true' "$REPLAY_BIN" play "$FANIN"
expect_ok "fanin:play 状态 ok" normal '"status":"ok"' "$REPLAY_BIN" play "$FANIN"
expect_ok "fanin:play checksum 一致" normal '"replayed_message_checksum"' "$REPLAY_BIN" play "$FANIN"

expect_ok "until:partial 状态" normal '"status":"partial"' "$REPLAY_BIN" play "$FANIN" --until=100
expect_ok "until:停在 seq 100" normal '"stopped_at_seq":100' "$REPLAY_BIN" play "$FANIN" --until=100
expect_ok "until:stopped_early" normal '"stopped_early":true' "$REPLAY_BIN" play "$FANIN" --until=100

mkdir -p "$PARTS"
expect_ok "split:生成前缀分片" normal '"ok":true' \
    "$REPLAY_BIN" split "$FANIN" --every-events=100 --out-dir="$PARTS"
PART_COUNT="$(find "$PARTS" -name '*.g2cr' -type f | wc -l)"
if [ "$PART_COUNT" -eq 4 ]; then
    pass "split:4 个分片" normal
else
    fail "split:4 个分片" normal "实际 $PART_COUNT"
fi
expect_ok "split:分片可独立重放" normal '"replay_executed":true' \
    "$REPLAY_BIN" play "$PARTS/fanin.part0001.g2cr"

# ---------------- select / context / values ----------------
OUT="$(timeout 30 "$SELECT_BIN" record "$WORK/select.g2cr" 2>&1)"; CODE=$?
expect_contains "select:record 事件数" normal "received=96" "$OUT"
if [ "$CODE" -eq 0 ]; then pass "select:record 退出码" normal; else fail "select:record 退出码" normal "exit=$CODE"; fi
OUT="$(timeout 30 "$SELECT_BIN" replay "$WORK/select.g2cr" 2>&1)"; CODE=$?
expect_contains "select:replay 一致性" normal "consistency=MATCH" "$OUT"
if [ "$CODE" -eq 0 ]; then pass "select:replay 退出码" normal; else fail "select:replay 退出码" normal "exit=$CODE"; fi

OUT="$(timeout 30 "$CONTEXT_BIN" record "$WORK/context.g2cr" 2>&1)"; CODE=$?
expect_contains "context:record 期望匹配" normal "expected=MATCH" "$OUT"
OUT="$(timeout 30 "$CONTEXT_BIN" replay "$WORK/context.g2cr" 2>&1)"; CODE=$?
expect_contains "context:虚拟时钟" normal "virtual_clock=OK" "$OUT"
expect_contains "context:重放一致性" normal "consistency=MATCH" "$OUT"

OUT="$(timeout 30 "$VALUES_BIN" record "$WORK/values.g2cr" 2>&1)"; CODE=$?
expect_contains "values:record 期望匹配" normal "expected=MATCH" "$OUT"
OUT="$(timeout 30 "$VALUES_BIN" replay "$WORK/values.g2cr" 2>&1)"; CODE=$?
expect_contains "values:重放 checksum" normal "consistency=MATCH" "$OUT"

# ---------------- 负路径 ----------------
: > "$WORK/empty.g2cr"
expect_fail "negative:空文件 format-error(exit2)" error '"status":"format-error"' \
    "$REPLAY_BIN" "$WORK/empty.g2cr"
printf 'hello world\n' > "$WORK/junk.g2cr"
expect_fail "negative:错误格式(exit2)" error '"status":"format-error"' \
    "$REPLAY_BIN" "$WORK/junk.g2cr"
head -c 300 "$FANIN" > "$WORK/trunc.g2cr"
TRUNC_OUT="$(timeout 20 "$REPLAY_BIN" play "$WORK/trunc.g2cr" 2>&1)"; TRUNC_CODE=$?
if [ "$TRUNC_CODE" -ne 0 ] && printf '%s' "$TRUNC_OUT" | grep -qF "complete=false"; then
    pass "negative:截断记录诊断 complete=false(exit!=0)" boundary "exit=$TRUNC_CODE"
else
    fail "negative:截断记录诊断 complete=false(exit!=0)" boundary \
        "exit=$TRUNC_CODE out=$(echo "$TRUNC_OUT" | head -3 | tr '\n' '|')"
fi
expect_fail "negative:未知选项(exit2)" error "unknown option" \
    "$REPLAY_BIN" play "$FANIN" --bogus

# ---------------- 失败注入：丢唤醒 ----------------
run_expect "fault:录制端 drop wake #20（看门狗收尾）" 0 inject \
    env GO2CPP_REPLAY_DROP_WAKE=20 GO2CPP_REPLAY_WATCHDOG_SEC=2 \
    timeout 20 "$RECORD_BIN" "$STUCK"
expect_fail "fault:坏记录重放判定 stuck" inject '"status":"stuck"' \
    "$REPLAY_BIN" "$STUCK"
expect_fail "fault:重放端注入 drop wake #20（fanin demo）" inject "status=stuck" \
    env GO2CPP_REPLAY_DROP_WAKE=20 "$REPLAY_DIR/go2cpp_replay_replay" "$FANIN"
# CLI `replay play` 的 ReplayOptions 没有从环境变量读取 drop_wake_at：
# 这是能力缺口，单独作为失败用例记录。
OUT="$(timeout 20 env GO2CPP_REPLAY_DROP_WAKE=20 "$REPLAY_BIN" play "$FANIN" 2>&1)"; CODE=$?
if [ "$CODE" -ne 0 ] && printf '%s' "$OUT" | grep -qF '"status":"stuck"'; then
    pass "fault:CLI play 透传 drop wake" inject
else
    fail "fault:CLI play 透传 drop wake" inject \
        "exit=$CODE status=ok；CLI 未读取 GO2CPP_REPLAY_DROP_WAKE（仅 fanin demo 支持）"
fi

# ---------------- CLI 集成 ----------------
expect_ok "cli-replay-play:应完整重放（replay_executed=true）" integration \
    '"replay_executed":true' env GO2CPP_REPLAY_TOOL="$REPLAY_BIN" "$CLI" replay play "$FANIN"

# ---------------- 资源限制 ----------------
OUT="$(timeout 20 bash -c 'ulimit -v 65536; exec "$@"' _ "$REPLAY_BIN" play "$FANIN" 2>&1)"; CODE=$?
if [ "$CODE" -ne 139 ] && [ "$CODE" -ne 134 ] && [ "$CODE" -ne 137 ]; then
    pass "resource:64MiB 虚拟内存下 play 不崩溃" resource "exit=$CODE"
else
    fail "resource:64MiB 虚拟内存下 play 不崩溃" resource "exit=$CODE"
fi
OUT="$(timeout 20 bash -c 'ulimit -n 32; exec "$@"' _ "$REPLAY_BIN" play "$FANIN" 2>&1)"; CODE=$?
if [ "$CODE" -ne 139 ] && [ "$CODE" -ne 134 ] && [ "$CODE" -ne 137 ]; then
    pass "resource:fd=32 下 play 不崩溃" resource "exit=$CODE"
else
    fail "resource:fd=32 下 play 不崩溃" resource "exit=$CODE"
fi
OUT="$(timeout 20 "$RECORD_BIN" /dev/full 2>&1)"; CODE=$?
if [ "$CODE" -ne 0 ] && [ "$CODE" -ne 139 ] && [ "$CODE" -ne 134 ]; then
    pass "resource:/dev/full 写入失败" resource "exit=$CODE"
else
    fail "resource:/dev/full 写入失败" resource "exit=$CODE out=$(echo "$OUT" | tail -2 | tr '\n' '|')"
fi

summary
