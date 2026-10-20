#!/usr/bin/env bash
# 「可迁移协程」v3（safepoint id + 句柄 + 签名）场景测试。
#
# 用法：
#   tools/migration/tests/run_tests.sh <构建目录>
#   # 默认构建目录：<repo>/build-migration
#
# 覆盖：
#   - PIE/ASLR 下的跨进程恢复（栈地址与代码地址都与生成进程不同）
#   - safepoint id：v1 布局 dump → v2 布局 restore（指令偏移不同、槽位相同）
#   - lambda：MIGRATION_SYMBOL_FN 注册的 noinline lambda 可 dump/restore
#   - 句柄表：std::string* / POD 对象随栈迁移，恢复后逐字段校验
#   - 签名：正确密钥通过；错误/缺失密钥、未签名、HMAC 篡改全部拒绝
#   - 符号：缺失拒绝、同名冲突拒绝、符号表版本差异明细
#   - 拒绝路径：CRC、截断、magic、build-id、跨可执行文件
#   - 随机/压力：随机消息、随机字节篡改、重复 dump/restore
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build-migration}"
MIGRATE="$BUILD_DIR/migrate"
MIGRATE_ALT="$BUILD_DIR/migrate_alt"
MIGRATE_CONFLICT="$BUILD_DIR/migrate_conflict"
SO_V1="$BUILD_DIR/libmigration_demo_v1.so"
SO_V2="$BUILD_DIR/libmigration_demo_v2.so"
SO_SP_V1="$BUILD_DIR/libmigration_demo_sp_v1.so"
SO_SP_V2="$BUILD_DIR/libmigration_demo_sp_v2.so"
SO_LAMBDA="$BUILD_DIR/libmigration_demo_lambda.so"
SO_UNREGISTERED="$BUILD_DIR/libmigration_demo_unregistered.so"

if [ ! -x "$MIGRATE" ]; then
  echo "找不到可执行文件：$MIGRATE" >&2
  echo "先执行：cmake -S tools/migration -B build-migration && cmake --build build-migration -j" >&2
  exit 2
fi

# 固定测试密钥（不是生产密钥，只用于本地测试）。
export MIGRATION_SIGNING_KEY="hex:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
TEST_KEY="$MIGRATION_SIGNING_KEY"
WRONG_KEY="hex:ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"

WORK="$(mktemp -d /tmp/migration-tests.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

ok() { printf 'PASS  %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n      %s\n' "$1" "$2"; fail=$((fail + 1)); }

# expect_ok <名称> <期望子串> <命令...>
expect_ok() {
  local name="$1" pattern="$2"
  shift 2
  local out code
  out="$("$@" 2>&1)"
  code=$?
  if [ "$code" -ne 0 ]; then
    bad "$name" "退出码 $code，输出：$(printf '%s' "$out" | head -3 | tr '\n' ' ')"
    return
  fi
  if ! printf '%s' "$out" | grep -qF "$pattern"; then
    bad "$name" "输出中缺少「$pattern」"
    return
  fi
  ok "$name"
}

# expect_fail <名称> <期望子串> <命令...>
expect_fail() {
  local name="$1" pattern="$2"
  shift 2
  local out code
  out="$("$@" 2>&1)"
  code=$?
  if [ "$code" -eq 0 ]; then
    bad "$name" "本应失败但退出码为 0"
    return
  fi
  if ! printf '%s' "$out" | grep -qF "$pattern"; then
    bad "$name" "失败信息中缺少「$pattern」，实际：$(printf '%s' "$out" | tail -2 | tr '\n' ' ')"
    return
  fi
  ok "$name"
}

# 从输出里提取一个匹配的片段。
extract() {
  grep -oE "$1" | head -1
}

echo "== 构建目录: $BUILD_DIR"
echo "== 临时目录: $WORK"
echo

SNAP="$WORK/fiber.mig"

# ---------------------------------------------------------------- 加密自检
expect_ok "crypto.selftest：SHA-256/HMAC 测试向量" "SHA-256/HMAC 自检通过" \
  "$MIGRATE" selftest

# ---------------------------------------------------------------- 基础恢复
expect_ok "A.dump：Fiber 挂起在通道等待" "Fiber 已挂起" "$MIGRATE" dump "$SNAP"
if [ -s "$SNAP" ]; then ok "A.dump：快照文件非空"; else bad "A.dump：快照文件非空" "文件为空"; fi

OUT_DUMP="$("$MIGRATE" dump "$WORK/again.mig" 2>&1)"
expect_ok "B.restore：收到第 1 条消息" "收到消息 #1 = 4242" "$MIGRATE" restore "$SNAP"
expect_ok "B.restore：跨进程恢复的 64KiB 栈 payload 校验通过" "栈上 payload 校验通过" \
  "$MIGRATE" restore "$SNAP"
expect_ok "B.restore：Fiber 正常完成且 result 正确" "result=7（期望 7）" "$MIGRATE" restore "$SNAP"

# ---------------------------------------------------------------- 符号化证据
OUT_RESTORE="$("$MIGRATE" restore "$SNAP" 2>&1)"
OLD_STACK="$(printf '%s' "$OUT_RESTORE" | extract '旧栈 0x[0-9a-f]+')"
NEW_STACK="$(printf '%s' "$OUT_RESTORE" | extract '新栈 0x[0-9a-f]+')"
if [ -n "$OLD_STACK" ] && [ -n "$NEW_STACK" ] && [ "$OLD_STACK" != "$NEW_STACK" ]; then
  ok "symbolic.stack：栈被重定位到新地址（$OLD_STACK → $NEW_STACK）"
else
  bad "symbolic.stack：栈地址应不同" "old=$OLD_STACK new=$NEW_STACK"
fi

ENTRY_A="$(printf '%s' "$OUT_DUMP" | extract '入口=0x[0-9a-f]+')"
ENTRY_B="$(printf '%s' "$OUT_RESTORE" | extract '→ 0x[0-9a-f]+')"
if [ -n "$ENTRY_A" ] && [ -n "$ENTRY_B" ] && [ "$ENTRY_A" != "$ENTRY_B" ]; then
  ok "symbolic.pie：入口地址按符号解析到新值（$ENTRY_A vs $ENTRY_B）"
else
  bad "symbolic.pie：入口地址应不同" "A=$ENTRY_A B=$ENTRY_B"
fi

OUT_DUMP2="$("$MIGRATE" dump "$WORK/again2.mig" 2>&1)"
ENTRY_A2="$(printf '%s' "$OUT_DUMP2" | extract '入口=0x[0-9a-f]+')"
if [ -n "$ENTRY_A" ] && [ -n "$ENTRY_A2" ] && [ "$ENTRY_A" != "$ENTRY_A2" ]; then
  ok "symbolic.aslr：两次 dump 的入口地址不同（$ENTRY_A vs $ENTRY_A2）"
else
  bad "symbolic.aslr：两次 dump 的地址应不同" "A=$ENTRY_A A2=$ENTRY_A2"
fi

# ---------------------------------------------------------------- 格式 / 签名
expect_ok "verify：报告挂起态与等待类型" "fiber state=2" "$MIGRATE" verify "$SNAP"
expect_ok "verify：报告挂起点的 safepoint id" "safepoint_id=0x10001" "$MIGRATE" verify "$SNAP"
expect_ok "verify：符号目录非空" "symbols=" "$MIGRATE" verify "$SNAP"
expect_ok "verify：报告重定位数量" "relocs=" "$MIGRATE" verify "$SNAP"
expect_ok "verify：签名校验通过" "verified=yes" "$MIGRATE" verify "$SNAP"
expect_ok "verify：符号表与快照一致" "符号表: 与快照一致" "$MIGRATE" verify "$SNAP"

expect_fail "signature.wrong_key：错误密钥被拒绝" "签名密钥不匹配" \
  env MIGRATION_SIGNING_KEY="$WRONG_KEY" "$MIGRATE" restore "$SNAP"
expect_fail "signature.missing_key：缺少密钥被拒绝" "未配置签名密钥" \
  env -u MIGRATION_SIGNING_KEY "$MIGRATE" restore "$SNAP"
expect_fail "signature.dump_without_key：无密钥拒绝生成快照" "未配置签名密钥" \
  env -u MIGRATION_SIGNING_KEY "$MIGRATE" dump "$WORK/nokey.mig"

MIGRATION_INSECURE_NO_SIGNATURE=1 "$MIGRATE" dump "$WORK/unsigned.mig" >/dev/null 2>&1 \
  && ok "signature.unsigned_dump：测试开关可生成未签名快照" \
  || bad "signature.unsigned_dump：测试开关可生成未签名快照" "dump 失败"
expect_fail "signature.unsigned_rejected：未签名快照被拒绝" "快照未签名" \
  "$MIGRATE" restore "$WORK/unsigned.mig"

# 密钥文件路径（不是环境变量）
printf '%s' "$TEST_KEY" > "$WORK/signing.key"
expect_ok "signature.key_file：从密钥文件加载并通过校验" "result=7（期望 7）" \
  env -u MIGRATION_SIGNING_KEY MIGRATION_SIGNING_KEY_FILE="$WORK/signing.key" \
  "$MIGRATE" restore "$SNAP"

# 篡改一个字节并重算 CRC —— CRC 通过，但 HMAC 必须拒绝。
cp "$SNAP" "$WORK/hmac_tamper.mig"
python3 - "$WORK/hmac_tamper.mig" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
# 找到 Summary 段（type=5），改其中一个字节；然后重算 CRC，让 CRC 检查通过，
# 这样能单独验证 HMAC 拒绝路径。
header_size = 224
section_size = 48
count = struct.unpack_from("<I", data, 20)[0]
target = None
for i in range(count):
    off = header_size + i * section_size
    stype, _flags, soff, ssize = struct.unpack_from("<IIQQ", data, off)
    if stype == 5:
        target = soff
        break
if target is None:
    raise SystemExit("找不到 summary 段")
data[target] ^= 0x01
# 同步更新该段的段级 CRC（段表项 +40），让段 CRC 检查也通过。
data[120:124] = b"\x00\x00\x00\x00"
for i in range(count):
    off = header_size + i * section_size
    stype, _flags, soff, ssize = struct.unpack_from("<IIQQ", data, off)
    if stype == 5:
        section_crc = zlib.crc32(bytes(data[soff:soff + ssize])) & 0xFFFFFFFF
        struct.pack_into("<I", data, off + 40, section_crc)
        break
# 最后重算整文件 CRC，让 CRC 检查通过 —— 只剩 HMAC 能拦住篡改。
crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
data[120:124] = struct.pack("<I", crc)
open(path, "wb").write(data)
PY
expect_fail "signature.hmac_tamper：CRC 通过但 HMAC 拒绝" "签名校验失败" \
  "$MIGRATE" restore "$WORK/hmac_tamper.mig"

# ---------------------------------------------------------------- 参数 / 重复
expect_ok "repeat.dump：第二次 dump 成功" "dump 完成" "$MIGRATE" dump "$WORK/fiber2.mig"
expect_ok "repeat.restore：第二个快照同样可恢复" "收到消息 #1 = 4242" "$MIGRATE" restore "$WORK/fiber2.mig"
expect_ok "custom.message：自定义两条消息" "收到消息 #2 = 200" \
  env MIGRATION_MSG1=100 MIGRATION_MSG2=200 "$MIGRATE" restore "$SNAP"

# ---------------------------------------------------------------- SO 场景
if [ -f "$SO_V1" ] && [ -f "$SO_V2" ]; then
  SNAP_SO="$WORK/fiber_so.mig"
  OUT_SO_DUMP="$(MIGRATION_DEMO_SO="$SO_V1" "$MIGRATE" dump "$SNAP_SO" 2>&1)"
  if printf '%s' "$OUT_SO_DUMP" | grep -qF "已加载 demo SO"; then
    ok "so.dump：进程 A 加载 v1 SO 并挂起"
  else
    bad "so.dump：进程 A 加载 v1 SO 并挂起" "$(printf '%s' "$OUT_SO_DUMP" | tail -2)"
  fi
  OUT_SO_RESTORE="$(MIGRATION_DEMO_SO="$SO_V2" "$MIGRATE" restore "$SNAP_SO" 2>&1)"
  if printf '%s' "$OUT_SO_RESTORE" | grep -qF "result=7（期望 7）"; then
    ok "so.restore：v1 dump → v2 restore 成功恢复"
  else
    bad "so.restore：v1 dump → v2 restore 成功恢复" "$(printf '%s' "$OUT_SO_RESTORE" | tail -3)"
  fi
  SO_ADDR_DUMP="$(printf '%s' "$OUT_SO_DUMP" | extract '入口=0x[0-9a-f]+')"
  SO_ADDR_RESTORE="$(printf '%s' "$OUT_SO_RESTORE" | extract '→ 0x[0-9a-f]+')"
  if [ -n "$SO_ADDR_DUMP" ] && [ -n "$SO_ADDR_RESTORE" ] && [ "$SO_ADDR_DUMP" != "$SO_ADDR_RESTORE" ]; then
    ok "so.addr：v1/v2 加载地址不同（$SO_ADDR_DUMP vs $SO_ADDR_RESTORE）"
  else
    bad "so.addr：v1/v2 加载地址应不同" "v1=$SO_ADDR_DUMP v2=$SO_ADDR_RESTORE"
  fi
  # 不加载 SO 时，快照引用的 migration_demo_body 无法解析 → 必须拒绝。
  expect_fail "so.missing_symbol：未加载 SO 时按符号缺失拒绝" "migration_demo_body" \
    "$MIGRATE" restore "$SNAP_SO"
else
  echo "SKIP  so.*：未构建 libmigration_demo_v1/v2.so"
fi

# ---------------------------------------------------------------- safepoint id
if [ -f "$SO_SP_V1" ] && [ -f "$SO_SP_V2" ]; then
  SNAP_SP="$WORK/fiber_sp.mig"
  SP_DUMP_V1="$(MIGRATION_DEMO_SO="$SO_SP_V1" "$MIGRATE" dump "$SNAP_SP" 2>&1)"
  if printf '%s' "$SP_DUMP_V1" | grep -qF "safepoint=0x20001"; then
    ok "sp.dump：v1 布局在 id=0x20001 挂起"
  else
    bad "sp.dump：v1 布局在 id=0x20001 挂起" "$(printf '%s' "$SP_DUMP_V1" | tail -2)"
  fi
  expect_ok "sp.verify：快照记录 1 个挂起点槽位" "safepoints=1" "$MIGRATE" verify "$SNAP_SP"
  expect_ok "sp.handle_count：快照记录 2 个句柄" "句柄 2 个" \
    env MIGRATION_DEMO_SO="$SO_SP_V1" "$MIGRATE" dump "$WORK/sp_handle.mig"

  SP_RESTORE_V2="$(MIGRATION_DEMO_SO="$SO_SP_V2" "$MIGRATE" restore "$SNAP_SP" 2>&1)"
  if printf '%s' "$SP_RESTORE_V2" | grep -qF "result=7（期望 7）"; then
    ok "sp.restore：v1 布局 dump → v2 布局 restore 成功"
  else
    bad "sp.restore：v1 布局 dump → v2 布局 restore 成功" \
      "$(printf '%s' "$SP_RESTORE_V2" | tail -3)"
  fi
  if printf '%s' "$SP_RESTORE_V2" | grep -qF "句柄校验通过：label=\"跨进程标签\" payload=heap-payload"; then
    ok "sp.handles：std::string + POD 句柄跨进程重建并校验"
  else
    bad "sp.handles：std::string + POD 句柄跨进程重建并校验" \
      "$(printf '%s' "$SP_RESTORE_V2" | grep -F '校验' | tail -2)"
  fi
  OLD_OFF="$(printf '%s' "$SP_RESTORE_V2" | extract '旧 [^ ]+\+0x[0-9a-f]+' | extract '0x[0-9a-f]+$')"
  NEW_OFF="$(printf '%s' "$SP_RESTORE_V2" | extract '新 [^ ]+\+0x[0-9a-f]+' | extract '0x[0-9a-f]+$')"
  if [ -n "$OLD_OFF" ] && [ -n "$NEW_OFF" ] && [ "$OLD_OFF" != "$NEW_OFF" ]; then
    ok "sp.layout_changed：指令偏移不同（$OLD_OFF → $NEW_OFF）仍恢复"
  else
    bad "sp.layout_changed：指令偏移应不同" "old=$OLD_OFF new=$NEW_OFF"
  fi

  # 两版各自 dump：挂起点槽位（栈帧布局）相同，指令偏移不同。
  SP_V1_VERIFY="$("$MIGRATE" verify "$WORK/sp_handle.mig" 2>&1)"
  MIGRATION_DEMO_SO="$SO_SP_V2" "$MIGRATE" dump "$WORK/fiber_sp_v2.mig" >/dev/null 2>&1
  SP_V2_VERIFY="$("$MIGRATE" verify "$WORK/fiber_sp_v2.mig" 2>&1)"
  SLOT_V1="$(printf '%s' "$SP_V1_VERIFY" | extract 'safepoint id=0x20001 .* slot_off=0x[0-9a-f]+' | extract 'slot_off=0x[0-9a-f]+')"
  SLOT_V2="$(printf '%s' "$SP_V2_VERIFY" | extract 'safepoint id=0x20001 .* slot_off=0x[0-9a-f]+' | extract 'slot_off=0x[0-9a-f]+')"
  if [ -n "$SLOT_V1" ] && [ "$SLOT_V1" = "$SLOT_V2" ]; then
    ok "sp.slot_same：两版挂起点槽位相同（$SLOT_V1），帧布局一致"
  else
    bad "sp.slot_same：两版挂起点槽位应相同" "v1=$SLOT_V1 v2=$SLOT_V2"
  fi
  OFF_V1="$(printf '%s' "$SP_V1_VERIFY" | extract 'old_offset=0x[0-9a-f]+')"
  OFF_V2="$(printf '%s' "$SP_V2_VERIFY" | extract 'old_offset=0x[0-9a-f]+')"
  if [ -n "$OFF_V1" ] && [ "$OFF_V1" != "$OFF_V2" ]; then
    ok "sp.offset_diff：两版记录的回地址偏移不同（$OFF_V1 vs $OFF_V2）"
  else
    bad "sp.offset_diff：两版记录的回地址偏移应不同" "v1=$OFF_V1 v2=$OFF_V2"
  fi

  # 旧版 SO 没有 safepoint 段 → 恢复必须拒绝并指出缺失的 id。
  expect_fail "sp.missing_id：旧 SO 恢复 safepoint 快照被拒绝" "挂起点 id=0x20001" \
    env MIGRATION_DEMO_SO="$SO_V1" "$MIGRATE" restore "$SNAP_SP"

  # 未登记堆指针 → dump 拒绝
  expect_fail "heap.unregistered：未登记堆指针被拒绝" "未登记的堆指针" \
    env MIGRATION_DEMO_UNREGISTERED_HEAP=1 MIGRATION_DEMO_SO="$SO_SP_V1" \
    "$MIGRATE" dump "$WORK/unregistered_heap.mig"

  # 符号表版本差异：v2 SO 额外注册一个符号，恢复时列出明细但不阻断。
  expect_ok "symbol.table_diff：符号表差异列出明细" "新增: SpExtraSymbol" \
    env MIGRATION_DEMO_EXTRA_SYMBOL=1 MIGRATION_DEMO_SO="$SO_SP_V2" \
    "$MIGRATE" restore "$SNAP_SP"
else
  echo "SKIP  sp.* / heap.*：未构建 libmigration_demo_sp_v1/v2.so"
fi

# ---------------------------------------------------------------- lambda
if [ -f "$SO_LAMBDA" ]; then
  SNAP_LAMBDA="$WORK/fiber_lambda.mig"
  expect_ok "lambda.dump：noinline lambda 主体挂起" "safepoint=0x30001" \
    env MIGRATION_DEMO_SO="$SO_LAMBDA" "$MIGRATE" dump "$SNAP_LAMBDA"
  expect_ok "lambda.restore：lambda 跨进程恢复并完成" "result=7（期望 7）" \
    env MIGRATION_DEMO_SO="$SO_LAMBDA" "$MIGRATE" restore "$SNAP_LAMBDA"
  LAMBDA_VERIFY="$(env MIGRATION_DEMO_SO="$SO_LAMBDA" "$MIGRATE" verify "$SNAP_LAMBDA" 2>&1)"
  if printf '%s' "$LAMBDA_VERIFY" | grep -qE 'lambda[^ ]*::operator\(\)' \
     && printf '%s' "$LAMBDA_VERIFY" | grep -qF "kind=3"; then
    ok "lambda.symbol：demangled lambda 名 + kind=3"
  else
    bad "lambda.symbol：demangled lambda 名 + kind=3" \
      "$(printf '%s' "$LAMBDA_VERIFY" | grep -F 'sym[' | grep -F 'lambda' | head -2)"
  fi
else
  echo "SKIP  lambda.*：未构建 libmigration_demo_lambda.so"
fi

# ---------------------------------------------------------------- 反例符号
if [ -x "$MIGRATE_CONFLICT" ]; then
  expect_fail "symbol.conflict：同名符号不同地址被拒绝" "符号名冲突" \
    "$MIGRATE_CONFLICT" dump "$WORK/conflict.mig"
else
  echo "SKIP  symbol.conflict：未构建 migrate_conflict"
fi

if [ -f "$SO_UNREGISTERED" ]; then
  expect_fail "symbol.unregistered：匿名静态函数未注册被拒绝" "MIGRATION_SYMBOL" \
    env MIGRATION_DEMO_SO="$SO_UNREGISTERED" "$MIGRATE" dump "$WORK/unregistered.mig"
else
  echo "SKIP  symbol.unregistered：未构建 libmigration_demo_unregistered.so"
fi

# ---------------------------------------------------------------- 拒绝路径
CORRUPT="$WORK/corrupt.mig"
cp "$SNAP" "$CORRUPT"
python3 - "$CORRUPT" <<'PY'
import sys

path = sys.argv[1]
with open(path, "r+b") as f:
    f.seek(-9, 2)
    b = f.read(1)
    f.seek(-9, 2)
    f.write(bytes([b[0] ^ 0xFF]))
PY
expect_fail "reject.corrupt：翻转一个字节被 CRC 拒绝" "CRC 校验失败" "$MIGRATE" restore "$CORRUPT"

TRUNC="$WORK/trunc.mig"
head -c 300 "$SNAP" > "$TRUNC"
expect_fail "reject.truncated：截断文件被拒绝" "file_size" "$MIGRATE" restore "$TRUNC"

BADMAGIC="$WORK/badmagic.mig"
cp "$SNAP" "$BADMAGIC"
printf 'XXXXXXXX' | dd of="$BADMAGIC" bs=1 seek=0 count=8 conv=notrunc status=none
expect_fail "reject.magic：magic 不匹配被拒绝" "magic 不匹配" "$MIGRATE" restore "$BADMAGIC"

if [ -x "$MIGRATE_ALT" ]; then
  expect_fail "reject.foreign_binary：跨可执行文件恢复被 build-id 拒绝" "build-id 不匹配" \
    "$MIGRATE_ALT" restore "$SNAP"
else
  echo "SKIP  reject.foreign_binary：未构建 migrate_alt"
fi

expect_fail "reject.missing：不存在的文件" "打开快照文件失败" "$MIGRATE" restore "$WORK/nope.mig"

# ---------------------------------------------------------------- 随机 / 压力
RANDOM_OK=0
for i in 1 2 3 4 5; do
  m1=$(( (RANDOM % 100000) + 1 ))
  m2=$(( (RANDOM % 100000) + 1 ))
  if env MIGRATION_MSG1="$m1" MIGRATION_MSG2="$m2" "$MIGRATE" restore "$SNAP" >/dev/null 2>&1; then
    RANDOM_OK=$((RANDOM_OK + 1))
  fi
done
if [ "$RANDOM_OK" -eq 5 ]; then
  ok "random.messages：5 组随机消息值全部恢复成功"
else
  bad "random.messages：5 组随机消息值全部恢复成功" "成功 $RANDOM_OK/5"
fi

FUZZ_REJECTED=0
for i in $(seq 1 20); do
  python3 - "$SNAP" "$WORK/fuzz.mig" <<'PY'
import random
import sys

src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())
pos = random.randrange(len(data))
data[pos] ^= 1 << random.randrange(8)
open(dst, "wb").write(data)
PY
  if ! "$MIGRATE" restore "$WORK/fuzz.mig" >/dev/null 2>&1; then
    FUZZ_REJECTED=$((FUZZ_REJECTED + 1))
  fi
done
if [ "$FUZZ_REJECTED" -eq 20 ]; then
  ok "fuzz.random_corrupt：20 次随机位翻转全部被拒绝"
else
  bad "fuzz.random_corrupt：20 次随机位翻转全部被拒绝" "拒绝 $FUZZ_REJECTED/20"
fi

STRESS_OK=0
START=$(date +%s%N)
for i in $(seq 1 10); do
  if timeout 20 "$MIGRATE" dump "$WORK/stress.mig" >/dev/null 2>&1 \
     && timeout 20 "$MIGRATE" restore "$WORK/stress.mig" >/dev/null 2>&1; then
    STRESS_OK=$((STRESS_OK + 1))
  fi
done
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
if [ "$STRESS_OK" -eq 10 ]; then
  ok "stress.loop：10 轮 dump+restore 全部成功（${ELAPSED_MS}ms，平均 $((ELAPSED_MS / 10))ms/轮）"
else
  bad "stress.loop：10 轮 dump+restore 全部成功" "成功 $STRESS_OK/10"
fi

# ---------------------------------------------------------------- 汇总
echo
echo "=============================================="
echo "通过 $pass 项，失败 $fail 项"
echo "=============================================="
[ "$fail" -eq 0 ]
