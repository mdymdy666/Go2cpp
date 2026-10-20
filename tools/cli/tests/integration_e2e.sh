#!/usr/bin/env bash
# Go2Cpp 四线真实联调脚本（Debug / 迁移 / 重放 / CLI）。
#
# 前置构建（本脚本只做校验，不重复全量构建）：
#   cmake -B build-integration -DGO2CPP_DEBUG_DUMP=ON -DGO2CPP_WITH_CLI=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-integration -j
#   cmake -S tools/migration -B build-migration && cmake --build build-migration -j
#   cmake -S tools/replay -B build-replay -DGO2CPP_REPLAY=ON \
#         -DGO2CPP_REPLAY_LIB_DIR=$PWD/build-integration && cmake --build build-replay -j
#
# 运行（仓库根目录或任意目录均可）：
#   bash tools/cli/tests/integration_e2e.sh
# 可选环境变量：GO2CPP_BUILD_INTEGRATION / GO2CPP_BUILD_MIGRATION /
#               GO2CPP_BUILD_REPLAY / GO2CPP_INTEGRATION_EVIDENCE
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_INTEGRATION="${GO2CPP_BUILD_INTEGRATION:-$ROOT/build-integration}"
BUILD_MIGRATION="${GO2CPP_BUILD_MIGRATION:-$ROOT/build-migration}"
BUILD_REPLAY="${GO2CPP_BUILD_REPLAY:-$ROOT/build-replay}"
CLI="$BUILD_INTEGRATION/tools/cli/go2cpp-cli"
MIGRATE="$BUILD_MIGRATION/migrate"
RECORD="$BUILD_REPLAY/go2cpp_replay_record"
REPLAY="$BUILD_REPLAY/go2cpp_replay_replay"
OFFLINE_REPLAY="$BUILD_REPLAY/replay"
DEMO_SRC="$SCRIPT_DIR/debug_longrun_demo.cpp"
PROBE="$SCRIPT_DIR/debug_socket_probe.py"
EVID="${GO2CPP_INTEGRATION_EVIDENCE:-/tmp/go2cpp-integration-evidence}"
DEMO="$EVID/bin/go2cpp_debug_demo"
DEMO_PIDFILE="$EVID/debug-demo.pid"
DEMO_LOG="$EVID/debug-demo.log"
PASS=0
FAIL=0
DEMO_PID=""

cleanup() {
    if [ -n "$DEMO_PID" ]; then
        kill "$DEMO_PID" 2>/dev/null || true
        wait "$DEMO_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -rf "$EVID"
mkdir -p "$EVID/bin"

say() { printf '\n===== %s =====\n' "$*"; }
ok() { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$*"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n' "$*"; }

need_file() {
    if [ ! -x "$1" ]; then
        printf '缺少可执行文件: %s\n' "$1" >&2
        exit 2
    fi
}
need_file "$CLI"
need_file "$MIGRATE"
need_file "$RECORD"
need_file "$REPLAY"
need_file "$OFFLINE_REPLAY"

# ---------------------------------------------------------------------------
# 1. Debug：真实长跑进程 + 真实 Unix socket + go2cpp-cli
# ---------------------------------------------------------------------------
say "1. DebugServer 真实联调"
if [ ! -x "$DEMO" ]; then
    if [ ! -f "$DEMO_SRC" ]; then
        printf '缺少 demo 源码 %s，且二进制不存在\n' "$DEMO_SRC" >&2
        exit 2
    fi
    g++ -std=c++17 -O2 -g -I"$ROOT/include" -I"$ROOT/third_party" "$DEMO_SRC" \
        -L"$BUILD_INTEGRATION" -Wl,-rpath,"$BUILD_INTEGRATION" \
        -lgo2cpp_debug -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_channel \
        -lgo2cpp_sync -lgo2cpp_io -lgo2cpp_context -lgo2cpp_log -lgo2cpp_error \
        -lgo2cpp_debug_event -lpthread -o "$DEMO" || exit 2
fi

# 清理上一轮残留进程（只认本脚本的固定二进制路径）。
pkill -f "$DEMO" 2>/dev/null || true
sleep 0.2
rm -f "$DEMO_PIDFILE" "$DEMO_LOG"
nohup "$DEMO" > "$DEMO_LOG" 2>&1 &
DEMO_PID=$!
printf '%s\n' "$DEMO_PID" > "$DEMO_PIDFILE"
for _ in $(seq 1 300); do
    if [ -S "/tmp/go2cpp-$DEMO_PID.sock" ]; then break; fi
    sleep 0.02
done
cat "$DEMO_LOG" | tee "$EVID/debug-demo.log"
if [ -S "/tmp/go2cpp-$DEMO_PID.sock" ]; then
    ok "DebugServer 监听 /tmp/go2cpp-$DEMO_PID.sock"
else
    bad "DebugServer 未创建 socket"
    kill "$DEMO_PID" 2>/dev/null || true
    exit 1
fi

say "1.1 go2cpp-cli status --pid $DEMO_PID"
"$CLI" status --pid "$DEMO_PID" > "$EVID/debug-status.txt" 2>&1
STATUS_RC=$?
cat "$EVID/debug-status.txt"
[ "$STATUS_RC" -eq 0 ] && ok "status 退出码 0" || bad "status 退出码 $STATUS_RC"
grep -q '协议: GO2CPP-DEBUG/1' "$EVID/debug-status.txt" && ok "status 协议 GO2CPP-DEBUG/1" || bad "status 协议缺失"
grep -q '调度器: processors=2 workers=2 running=true' "$EVID/debug-status.txt" && ok "status 调度器字段来自真实 scheduler" || bad "status 调度器字段不符"
grep -q '任务: 3 个（kWaiting=3）' "$EVID/debug-status.txt" && ok "status tasks=3/kWaiting=3" || bad "status tasks 不符"
grep -q 'runqueue: local=0 global=0 per_p=\[0, 0\]' "$EVID/debug-status.txt" && ok "status runqueue 字段完整" || bad "status runqueue 不符"
if grep -q '注: 有' "$EVID/debug-status.txt"; then bad "status 报告字段缺失"; else ok "status 无字段缺失告警"; fi

say "1.2 go2cpp-cli status --pid $DEMO_PID --json"
"$CLI" status --pid "$DEMO_PID" --json > "$EVID/debug-status.json" 2>&1
STATUS_JSON_RC=$?
python3 -m json.tool "$EVID/debug-status.json" > /dev/null 2>&1
JSON_VALID=$?
[ "$STATUS_JSON_RC" -eq 0 ] && ok "status --json 退出码 0" || bad "status --json 退出码 $STATUS_JSON_RC"
[ "$JSON_VALID" -eq 0 ] && ok "status --json 输出合法 JSON" || bad "status --json 不是合法 JSON"
head -20 "$EVID/debug-status.json"

say "1.3 go2cpp-cli dump --pid $DEMO_PID --sections scheduler,fiber,channel --format json"
"$CLI" dump --pid "$DEMO_PID" --sections scheduler,fiber,channel --format json \
    > "$EVID/debug-dump.json" 2> "$EVID/debug-dump.err"
DUMP_RC=$?
[ "$DUMP_RC" -eq 0 ] && ok "dump 退出码 0" || bad "dump 退出码 $DUMP_RC"
[ -s "$EVID/debug-dump.err" ] && { bad "dump 有 stderr 输出"; cat "$EVID/debug-dump.err"; } || ok "dump 无 stderr"
python3 -m json.tool "$EVID/debug-dump.json" | sed -n '1,120p'

say "1.4 直接 socket 探测并与 CLI 输出逐字段对比"
if [ -f "$PROBE" ]; then
    python3 "$PROBE" "$DEMO_PID" "$EVID/debug-dump.json" > "$EVID/debug-probe.txt" 2>&1
    PROBE_RC=$?
    cat "$EVID/debug-probe.txt"
    [ "$PROBE_RC" -eq 0 ] && ok "DebugServer 原始响应与 CLI dump 字段一致" || bad "socket 探测失败（rc=$PROBE_RC）"
else
    bad "缺少 socket 探测脚本 $PROBE"
fi

say "1.5 未注册组件错误路径（dump --sections io）"
"$CLI" dump --pid "$DEMO_PID" --sections io > "$EVID/debug-unavailable.txt" 2>&1
UNAVAILABLE_RC=$?
cat "$EVID/debug-unavailable.txt"
[ "$UNAVAILABLE_RC" -eq 3 ] && ok "未注册组件退出码 3" || bad "未注册组件退出码 $UNAVAILABLE_RC（期望 3）"
grep -q "目标进程未注册组件 'io'" "$EVID/debug-unavailable.txt" && ok "未注册组件错误信息准确" || bad "未注册组件错误信息不符"

say "1.6 dump ALL（text）"
"$CLI" dump --pid "$DEMO_PID" --sections all --format text > "$EVID/debug-dump-all.txt" 2>&1
DUMP_ALL_RC=$?
[ "$DUMP_ALL_RC" -eq 0 ] && ok "dump ALL 退出码 0" || bad "dump ALL 退出码 $DUMP_ALL_RC"
grep -q 'schema=go2cpp.dump.all/1' "$EVID/debug-dump-all.txt" && ok "dump ALL 聚合 schema 存在" || bad "dump ALL 聚合 schema 缺失"
grep -q '\[component channel-send\]' "$EVID/debug-dump-all.txt" && ok "dump ALL 含 channel-send 组件" || bad "dump ALL 缺 channel-send"
head -12 "$EVID/debug-dump-all.txt"

kill "$DEMO_PID" 2>/dev/null || true
wait "$DEMO_PID" 2>/dev/null || true
rm -f "/tmp/go2cpp-$DEMO_PID.sock"
DEMO_PID=""

# ---------------------------------------------------------------------------
# 2. 迁移 v3 端到端（v2 兼容读）
# ---------------------------------------------------------------------------
# v3 默认强制 HMAC 签名；联调用固定测试密钥（不要用于生产）。
export MIGRATION_SIGNING_KEY="${MIGRATION_SIGNING_KEY:-hex:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff}"
say "2. 迁移 v3：dump -> verify -> restore"
SNAP="$EVID/fiber.mig"
"$MIGRATE" dump "$SNAP" > "$EVID/migrate-dump.txt" 2>&1
MIG_DUMP_RC=$?
cat "$EVID/migrate-dump.txt"
[ "$MIG_DUMP_RC" -eq 0 ] && ok "migrate dump 退出码 0" || bad "migrate dump 退出码 $MIG_DUMP_RC"
MAGIC=$(head -c 8 "$SNAP" 2>/dev/null)
[ "$MAGIC" = "G2CMIG03" ] && ok "快照魔数 G2CMIG03" || bad "快照魔数=$MAGIC"
python3 - "$SNAP" <<'PY'
import struct, sys
h = open(sys.argv[1], "rb").read(224)
version, header_size = struct.unpack_from("<II", h, 8)
print(f"header: magic={h[:8].decode()} version={version} header_size={header_size} "
      f"size={__import__('os').path.getsize(sys.argv[1])}")
assert h[:8] == b"G2CMIG03" and version == 3 and header_size == 224
PY
[ $? -eq 0 ] && ok "快照头 version=3/header_size=224" || bad "快照头字段不符"

"$MIGRATE" verify "$SNAP" > "$EVID/migrate-verify.txt" 2>&1
MIG_VERIFY_RC=$?
cat "$EVID/migrate-verify.txt"
[ "$MIG_VERIFY_RC" -eq 0 ] && ok "migrate verify 退出码 0" || bad "migrate verify 退出码 $MIG_VERIFY_RC"
grep -q 'summary: 符号化协程快照' "$EVID/migrate-verify.txt" && ok "verify 报告含符号化摘要" || bad "verify 报告缺少符号化摘要"

"$MIGRATE" restore "$SNAP" > "$EVID/migrate-restore.txt" 2>&1
MIG_RESTORE_RC=$?
cat "$EVID/migrate-restore.txt"
[ "$MIG_RESTORE_RC" -eq 0 ] && ok "migrate restore 退出码 0" || bad "migrate restore 退出码 $MIG_RESTORE_RC"
grep -q 'restore 完成' "$EVID/migrate-restore.txt" && ok "restore 完成（跨进程 Fiber 继续执行）" || bad "restore 未报告完成"

say "2.1 CLI migrate restore（真实 v3 快照）"
GO2CPP_MIGRATE_TOOL="$MIGRATE" "$CLI" migrate restore "$SNAP" > "$EVID/cli-migrate-restore.txt" 2>&1
CLI_RESTORE_RC=$?
cat "$EVID/cli-migrate-restore.txt"
[ "$CLI_RESTORE_RC" -eq 0 ] && ok "CLI migrate restore 退出码 0" || bad "CLI migrate restore 退出码 $CLI_RESTORE_RC"
grep -q 'restore 完成' "$EVID/cli-migrate-restore.txt" && ok "CLI 调用的后端完成恢复" || bad "CLI 恢复输出不符"

say "2.2 CLI migrate dump（真实后端）"
CLI_SNAP="$EVID/cli-fiber.mig"
GO2CPP_MIGRATE_TOOL="$MIGRATE" "$CLI" migrate dump --pid $$ --gid 0 --out "$CLI_SNAP" \
    > "$EVID/cli-migrate-dump.txt" 2>&1
CLI_DUMP_RC=$?
cat "$EVID/cli-migrate-dump.txt"
[ "$CLI_DUMP_RC" -eq 0 ] && ok "CLI migrate dump 退出码 0" || bad "CLI migrate dump 退出码 $CLI_DUMP_RC"
grep -q '迁移快照:' "$EVID/cli-migrate-dump.txt" && ok "CLI 打印快照摘要" || bad "CLI 缺少快照摘要"
grep -q 'G2CMIG03，版本 3' "$EVID/cli-migrate-dump.txt" && ok "CLI 识别 G2CMIG03/v3" || bad "CLI 未识别 G2CMIG03/v3"

say "2.3 版本/魔数路径（v1 拒绝、v2 兼容读、version=9 拒绝）"
printf 'G2CMIG01' > "$EVID/legacy-v1.mig"
head -c 120 /dev/zero >> "$EVID/legacy-v1.mig"
python3 - "$EVID/legacy-v2.mig" <<'PY'
import struct, sys
h = bytearray(192)
h[0:8] = b"G2CMIG02"
struct.pack_into("<I", h, 8, 2)      # version
struct.pack_into("<I", h, 12, 192)   # header_size
struct.pack_into("<I", h, 20, 2)     # section_count
struct.pack_into("<Q", h, 44, 42)    # fiber_id
struct.pack_into("<Q", h, 60, 65536)  # stack_size
open(sys.argv[1], "wb").write(h)
PY
python3 - "$EVID/version9.mig" <<'PY'
import struct, sys
h = bytearray(224)
h[0:8] = b"G2CMIG03"
struct.pack_into("<I", h, 8, 9)      # version=9（不支持）
struct.pack_into("<I", h, 12, 224)   # header_size
open(sys.argv[1], "wb").write(h)
PY
GO2CPP_MIGRATE_TOOL="$MIGRATE" "$CLI" migrate restore "$EVID/legacy-v1.mig" \
    > "$EVID/cli-reject-v1.txt" 2>&1
V1_RC=$?
cat "$EVID/cli-reject-v1.txt"
[ "$V1_RC" -eq 4 ] && ok "CLI 拒绝 v1 魔数（退出码 4）" || bad "CLI v1 退出码 $V1_RC"
grep -q '迁移文件格式不匹配' "$EVID/cli-reject-v1.txt" && ok "v1 错误信息明确" || bad "v1 错误信息不符"

"$CLI" debug "$EVID/legacy-v2.mig" > "$EVID/cli-debug-v2.txt" 2>&1
V2_DEBUG_RC=$?
cat "$EVID/cli-debug-v2.txt"
[ "$V2_DEBUG_RC" -eq 0 ] && ok "CLI 兼容读 G2CMIG02/v2" || bad "CLI v2 兼容读失败（$V2_DEBUG_RC）"
grep -q 'G2CMIG02，版本 2' "$EVID/cli-debug-v2.txt" && ok "v2 摘要正确" || bad "v2 摘要不符"

GO2CPP_MIGRATE_TOOL="$MIGRATE" "$CLI" migrate restore "$EVID/version9.mig" \
    > "$EVID/cli-reject-v9.txt" 2>&1
V9_RC=$?
cat "$EVID/cli-reject-v9.txt"
[ "$V9_RC" -eq 4 ] && ok "CLI 拒绝 version=9（退出码 4）" || bad "CLI v9 退出码 $V9_RC"
grep -q 'version=9' "$EVID/cli-reject-v9.txt" && ok "版本校验错误信息含 version=9" || bad "版本校验错误信息不符"

# ---------------------------------------------------------------------------
# 3. 重放端到端
# ---------------------------------------------------------------------------
say "3. 重放：record -> replay（96 rendezvous）"
RECORDING="$EVID/fanin.g2cr"
"$RECORD" "$RECORDING" > "$EVID/replay-record.txt" 2>&1
RECORD_RC=$?
cat "$EVID/replay-record.txt"
[ "$RECORD_RC" -eq 0 ] && ok "go2cpp_replay_record 退出码 0" || bad "record 退出码 $RECORD_RC"
grep -q 'consumer received=96' "$EVID/replay-record.txt" && ok "record: consumer received=96" || bad "record 消息数不符"
python3 - "$RECORDING" <<'PY'
import sys
data = open(sys.argv[1], "rb").read(8)
print("record magic:", data)
assert data == b"G2CRPLY\x00"
PY
[ $? -eq 0 ] && ok "记录魔数 G2CRPLY" || bad "记录魔数不符"

"$REPLAY" "$RECORDING" > "$EVID/replay-replay.txt" 2>&1
REPLAY_RC=$?
cat "$EVID/replay-replay.txt"
[ "$REPLAY_RC" -eq 0 ] && ok "go2cpp_replay_replay 退出码 0" || bad "replay 退出码 $REPLAY_RC"
grep -q 'consistency=MATCH' "$EVID/replay-replay.txt" && ok "replay: consistency=MATCH" || bad "replay 一致性不符"
grep -q 'replayed: checksum=' "$EVID/replay-replay.txt" && ok "replay 打印重放校验和" || bad "replay 缺少校验和"

say "3.1 CLI replay play（真实离线 replay 后端）"
GO2CPP_REPLAY_TOOL="$OFFLINE_REPLAY" "$CLI" replay play "$RECORDING" > "$EVID/cli-replay-play.txt" 2> "$EVID/cli-replay-play.err"
CLI_PLAY_RC=$?
cat "$EVID/cli-replay-play.txt"
[ "$CLI_PLAY_RC" -eq 0 ] && ok "CLI replay play 退出码 0" || bad "CLI replay play 退出码 $CLI_PLAY_RC"
python3 - "$EVID/cli-replay-play.txt" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
print("cli replay report: schema=%s status=%s messages=%s checksum_ok=%s" % (
    report["schema"], report["status"], report["stats"]["messages"], report["checksum_ok"]))
assert report["schema"] == "go2cpp.replay.report/1"
assert report["status"] == "ok"
assert report["checksum_ok"] is True
assert report["stats"]["messages"] > 0
PY
[ $? -eq 0 ] && ok "CLI replay 报告 JSON 字段与后端一致" || bad "CLI replay 报告不符"

say "3.2 故障注入：record 时丢第 17 次唤醒，replay 复现卡死"
STUCK="$EVID/stuck.g2cr"
GO2CPP_REPLAY_DROP_WAKE=17 GO2CPP_REPLAY_WATCHDOG_SEC=1 "$RECORD" "$STUCK" \
    > "$EVID/replay-record-stuck.txt" 2>&1
STUCK_RECORD_RC=$?
cat "$EVID/replay-record-stuck.txt"
[ -s "$STUCK" ] && ok "丢唤醒记录文件已生成" || bad "丢唤醒记录文件为空"
"$REPLAY" "$STUCK" > "$EVID/replay-replay-stuck.txt" 2>&1
STUCK_REPLAY_RC=$?
cat "$EVID/replay-replay-stuck.txt"
[ "$STUCK_REPLAY_RC" -ne 0 ] && ok "重放复现卡死（非零退出码 $STUCK_REPLAY_RC）" || bad "重放未复现卡死"
grep -qi 'stuck\|MISMATCH' "$EVID/replay-replay-stuck.txt" && ok "重放报告 stuck/MISMATCH" || bad "重放报告缺少 stuck/MISMATCH"

say "3.3 CLI replay play 透传 GO2CPP_REPLAY_DROP_WAKE"
GO2CPP_REPLAY_TOOL="$OFFLINE_REPLAY" GO2CPP_REPLAY_DROP_WAKE=17 "$CLI" replay play "$STUCK" \
    > "$EVID/cli-replay-drop.txt" 2>&1
CLI_DROP_RC=$?
cat "$EVID/cli-replay-drop.txt"
[ "$CLI_DROP_RC" -ne 0 ] && ok "CLI 丢唤醒复现 stuck（非零退出码 $CLI_DROP_RC）" || bad "CLI 丢唤醒仍返回 0（D4 未修复）"
grep -qi 'stuck' "$EVID/cli-replay-drop.txt" && ok "CLI 报告含 stuck" || bad "CLI 报告缺少 stuck"

# ---------------------------------------------------------------------------
say "结果"
printf 'PASS=%d FAIL=%d\n' "$PASS" "$FAIL"
printf '证据目录: %s\n' "$EVID"
[ "$FAIL" -eq 0 ]
