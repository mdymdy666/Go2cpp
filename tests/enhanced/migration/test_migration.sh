#!/usr/bin/env bash
# Migration 能力增强测试矩阵（真实 tools/migration v3 二进制）。
#
# 覆盖：跨进程 dump/restore、safepoint、lambda、句柄、签名正负路径、
#       CRC/magic/截断/build-id、符号冲突、资源限制、CLI 集成。
# 随机 1000 次位翻转与 100 轮压测见 stress_migration.sh。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPABILITY=migration
source "$HERE/../lib/common.sh"
common_init

WORK="$(make_work migration)"
SNAP="$WORK/fiber.mig"
SP_SNAP="$WORK/fiber_sp.mig"
LAMBDA_SNAP="$WORK/fiber_lambda.mig"
WRONG_KEY="hex:ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"
SO_SP_V1="$MIG_DIR/libmigration_demo_sp_v1.so"
SO_SP_V2="$MIG_DIR/libmigration_demo_sp_v2.so"
SO_LAMBDA="$MIG_DIR/libmigration_demo_lambda.so"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$MIGRATE" ]; then
    fail "fixture:migrate" normal "缺少 $MIGRATE"
    summary
    exit 1
fi

expect_ok "selftest:SHA-256/HMAC 向量" normal "SHA-256/HMAC 自检通过" "$MIGRATE" selftest
expect_ok "dump:挂起并写快照" normal "Fiber 已挂起" "$MIGRATE" dump "$SNAP"
if [ -s "$SNAP" ]; then
    pass "dump:快照非空" normal "$(stat -c%s "$SNAP") bytes"
else
    fail "dump:快照非空" normal "文件为空"
fi
expect_ok "restore:跨进程恢复" normal "result=7（期望 7）" "$MIGRATE" restore "$SNAP"
expect_ok "restore:消息 #1" normal "收到消息 #1 = 4242" "$MIGRATE" restore "$SNAP"
expect_ok "verify:签名通过" normal "verified=yes" "$MIGRATE" verify "$SNAP"
expect_ok "verify:safepoint id" normal "safepoint_id=0x10001" "$MIGRATE" verify "$SNAP"
expect_ok "verify:符号/重定位" normal "symbols=8 relocs=22" "$MIGRATE" verify "$SNAP"
expect_ok "random-messages:自定义两条消息" normal "收到消息 #2 = 200" \
    env MIGRATION_MSG1=100 MIGRATION_MSG2=200 "$MIGRATE" restore "$SNAP"

# ---------------- safepoint / lambda / 句柄 ----------------
if [ -f "$SO_SP_V1" ] && [ -f "$SO_SP_V2" ]; then
    expect_ok "safepoint:dump v1 布局" normal "safepoint=0x20001" \
        env MIGRATION_DEMO_SO="$SO_SP_V1" "$MIGRATE" dump "$SP_SNAP"
    OUT="$(env MIGRATION_DEMO_SO="$SO_SP_V2" "$MIGRATE" restore "$SP_SNAP" 2>&1)"; CODE=$?
    if [ "$CODE" -eq 0 ] && printf '%s' "$OUT" | grep -qF "result=7（期望 7）"; then
        pass "safepoint:v1 dump → v2 布局恢复" normal
    else
        fail "safepoint:v1 dump → v2 布局恢复" normal "exit=$CODE $(echo "$OUT" | tail -2 | tr '\n' '|')"
    fi
    expect_contains "safepoint:指令偏移变化" normal "旧 " "$OUT"
    expect_contains "handles:std::string + POD 重建" normal "句柄校验通过：label=\"跨进程标签\" payload=heap-payload" "$OUT"
    expect_ok "handles:快照记录 2 个句柄" normal "句柄 2 个" \
        env MIGRATION_DEMO_SO="$SO_SP_V1" "$MIGRATE" dump "$WORK/sp2.mig"
    expect_ok "symbol-table:版本差异明细" normal "新增: SpExtraSymbol" \
        env MIGRATION_DEMO_EXTRA_SYMBOL=1 MIGRATION_DEMO_SO="$SO_SP_V2" "$MIGRATE" restore "$SP_SNAP"
else
    skip "safepoint:*" normal "未构建 sp_v1/sp_v2 SO"
fi

if [ -f "$SO_LAMBDA" ]; then
    expect_ok "lambda:dump" normal "safepoint=0x30001" \
        env MIGRATION_DEMO_SO="$SO_LAMBDA" "$MIGRATE" dump "$LAMBDA_SNAP"
    expect_ok "lambda:restore" normal "result=7（期望 7）" \
        env MIGRATION_DEMO_SO="$SO_LAMBDA" "$MIGRATE" restore "$LAMBDA_SNAP"
    expect_ok "lambda:demangled 名 + kind=3" normal "kind=3" \
        env MIGRATION_DEMO_SO="$SO_LAMBDA" "$MIGRATE" verify "$LAMBDA_SNAP"
else
    skip "lambda:*" normal "未构建 lambda SO"
fi

# ---------------- 签名负路径 ----------------
expect_fail "signature:错误密钥被拒绝" error "签名密钥不匹配" \
    env MIGRATION_SIGNING_KEY="$WRONG_KEY" "$MIGRATE" restore "$SNAP"
expect_fail "signature:无密钥拒绝 dump" error "未配置签名密钥" \
    env -u MIGRATION_SIGNING_KEY "$MIGRATE" dump "$WORK/nokey.mig"
expect_ok "signature:测试开关生成未签名快照" error "dump 完成" \
    env MIGRATION_INSECURE_NO_SIGNATURE=1 "$MIGRATE" dump "$WORK/unsigned.mig"
expect_fail "signature:未签名快照被拒绝" error "快照未签名" "$MIGRATE" restore "$WORK/unsigned.mig"

# ---------------- 损坏 / 格式负路径 ----------------
cp "$SNAP" "$WORK/crc.mig"
python3 - "$WORK/crc.mig" <<'PY'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
data[-9] ^= 0xFF
open(path, "wb").write(data)
PY
expect_fail "corrupt:CRC 单字节篡改" error "CRC 校验失败" "$MIGRATE" restore "$WORK/crc.mig"

cp "$SNAP" "$WORK/magic.mig"
printf 'XXXXXXXX' | dd of="$WORK/magic.mig" bs=1 seek=0 count=8 conv=notrunc status=none
expect_fail "corrupt:magic 篡改" error "magic 不匹配" "$MIGRATE" restore "$WORK/magic.mig"

head -c 300 "$SNAP" > "$WORK/trunc.mig"
expect_fail "corrupt:截断到 300 字节" boundary "file_size" "$MIGRATE" restore "$WORK/trunc.mig"

if [ -x "$MIGRATE_ALT" ]; then
    expect_fail "corrupt:跨可执行文件 build-id" error "build-id 不匹配" "$MIGRATE_ALT" restore "$SNAP"
else
    skip "corrupt:跨可执行文件 build-id" error "未构建 migrate_alt"
fi

if [ -x "$MIGRATE_CONFLICT" ]; then
    expect_fail "symbol:同名冲突被拒绝" error "符号名冲突" "$MIGRATE_CONFLICT" dump "$WORK/conflict.mig"
else
    skip "symbol:同名冲突被拒绝" error "未构建 migrate_conflict"
fi

if [ -f "$SO_SP_V1" ]; then
    expect_fail "heap:未登记堆指针被拒绝" error "未登记的堆指针" \
        env MIGRATION_DEMO_UNREGISTERED_HEAP=1 MIGRATION_DEMO_SO="$SO_SP_V1" \
        "$MIGRATE" dump "$WORK/unregistered.mig"
fi

# ---------------- CLI 集成（当前 CLI 只认 G2CMIG02） ----------------
run_expect "cli-migrate-dump:真实 G2CMIG03 应成功" 0 integration \
    env GO2CPP_MIGRATE_TOOL="$MIGRATE" "$CLI" migrate dump --pid "$$" --gid 1 --out "$WORK/cli.mig"
expect_ok "cli-debug:真实 G2CMIG03 应可分析" integration "快照头" "$CLI" debug "$SNAP"
run_expect "cli-migrate-restore:真实 G2CMIG03 应成功" 0 integration \
    env GO2CPP_MIGRATE_TOOL="$MIGRATE" "$CLI" migrate restore "$SNAP"

# ---------------- 资源限制 ----------------
OUT="$(timeout 20 bash -c 'ulimit -n 32; exec "$@"' _ "$MIGRATE" dump "$WORK/fd.mig" 2>&1)"; CODE=$?
if [ "$CODE" -ne 139 ] && [ "$CODE" -ne 134 ] && [ "$CODE" -ne 137 ]; then
    pass "resource:fd=32 下 dump 不崩溃" resource "exit=$CODE"
else
    fail "resource:fd=32 下 dump 不崩溃" resource "exit=$CODE $(echo "$OUT" | tail -2 | tr '\n' '|')"
fi

OUT="$(timeout 20 bash -c 'ulimit -v 65536; exec "$@"' _ "$MIGRATE" dump "$WORK/mem.mig" 2>&1)"; CODE=$?
if [ "$CODE" -ne 139 ] && [ "$CODE" -ne 134 ] && [ "$CODE" -ne 137 ]; then
    pass "resource:64MiB 虚拟内存下 dump 不崩溃" resource "exit=$CODE"
else
    fail "resource:64MiB 虚拟内存下 dump 不崩溃" resource "exit=$CODE $(echo "$OUT" | tail -2 | tr '\n' '|')"
fi

OUT="$(timeout 20 "$MIGRATE" dump /dev/full 2>&1)"; CODE=$?
if [ "$CODE" -ne 0 ] && [ "$CODE" -ne 139 ] && [ "$CODE" -ne 134 ]; then
    pass "resource:/dev/full 磁盘满时 dump 失败" resource "exit=$CODE"
else
    fail "resource:/dev/full 磁盘满时 dump 失败" resource \
        "exit=$CODE（期望非 0 且非崩溃）$(echo "$OUT" | tail -2 | tr '\n' '|')"
fi

summary
