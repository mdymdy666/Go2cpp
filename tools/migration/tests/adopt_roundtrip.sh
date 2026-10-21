#!/usr/bin/env bash
# 主工程跨进程迁移 API 的 adopt 往返验收（GO2CPP_MIGRATION_API=ON）。
#
# 用法：bash tools/migration/tests/adopt_roundtrip.sh [build-dir]
#   build-dir 默认按 build-enhanced → build-migration-adopt 探测；
#   也可用 GO2CPP_MIGRATION_BUILD 指定。
#
# 覆盖（对应 docs/fiber-migration.md 的手工命令）：
#   1. dump：进程 A 导出 Fiber 栈 + channel 等待状态，退出码 0；
#   2. restore：进程 B 导入 → 投递消息 42 → Fiber 恢复并 Completed；
#   3. selftest：正常闭环 / 重复导入 100 次 / 篡改拒绝 / 未签名拒绝 /
#      跨 build-id 拒绝；
#   4. 反例二进制（_alt）导入同一快照必须失败（build-id 不同）；
#   5. 未设置 MIGRATION_SIGNING_KEY 时导出必须失败（默认拒绝未签名快照）。
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

pick_build_dir() {
    local candidate
    for candidate in "${GO2CPP_MIGRATION_BUILD:-}" \
                     "$ROOT/build-enhanced" \
                     "$ROOT/build-migration-adopt" \
                     "$ROOT/build-debug-full"; do
        [ -n "$candidate" ] || continue
        if [ -x "$candidate/go2cpp_fiber_adopt_demo" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    printf '%s' "${GO2CPP_MIGRATION_BUILD:-$ROOT/build-enhanced}"
    return 1
}

BUILD_DIR="$(pick_build_dir)" || true
DEMO="$BUILD_DIR/go2cpp_fiber_adopt_demo"
ALT="$BUILD_DIR/go2cpp_fiber_adopt_demo_alt"

if [ ! -x "$DEMO" ]; then
    echo "SKIP: 缺少 $DEMO"
    echo "  先构建：cmake -S . -B build-enhanced -DGO2CPP_BUILD_ENHANCED=ON -DGO2CPP_WITH_CLI=ON"
    echo "          cmake --build build-enhanced -j"
    exit 77
fi

WORK="$(mktemp -d /tmp/go2cpp-adopt.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
# selftest 子命令不会自建输出目录：不先建好，第一次 dump 就会
# 「打开快照文件失败: No such file or directory」。
mkdir -p "$WORK/selftest"

PASS=0
FAIL=0
FAILED=()

pass() { PASS=$((PASS + 1)); echo "PASS  $1"; }
fail() { FAIL=$((FAIL + 1)); FAILED+=("$1"); echo "FAIL  $1  ${2:-}"; }

# run_case <name> <expect: ok|fail> <pattern> <cmd...>
run_case() {
    local name="$1" expect="$2" pattern="$3"
    shift 3
    local out code
    out="$(timeout 120 "$@" 2>&1)"
    code=$?
    if [ "$expect" = "ok" ] && [ "$code" -ne 0 ]; then
        fail "$name" "exit=$code head=$(printf '%s' "$out" | head -3 | tr '\n' '|')"
        printf '%s\n' "$out" | tail -25 | sed 's/^/    | /'
        return 0
    fi
    if [ "$expect" = "fail" ] && [ "$code" -eq 0 ]; then
        fail "$name" "期望失败但 exit=0 head=$(printf '%s' "$out" | head -3 | tr '\n' '|')"
        return 0
    fi
    if [ -n "$pattern" ] && ! printf '%s' "$out" | grep -qF -- "$pattern"; then
        fail "$name" "exit=$code 缺 '$pattern' head=$(printf '%s' "$out" | head -3 | tr '\n' '|')"
        printf '%s\n' "$out" | tail -20 | sed 's/^/    | /'
        return 0
    fi
    pass "$name"
}

export MIGRATION_SIGNING_KEY="${MIGRATION_SIGNING_KEY:-hex:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff}"
SNAP="$WORK/fiber-adopt.mig"

echo "== build_dir=$BUILD_DIR"
run_case "dump:导出 Fiber 栈与 channel 等待状态" ok "[A] 导出成功" \
    "$DEMO" dump "$SNAP"
run_case "restore:导入 → 消息 42 → Fiber Completed" ok "[B] Fiber 恢复执行并正常结束：state=Completed" \
    "$DEMO" restore "$SNAP"
run_case "selftest:全场景（闭环/重复导入/篡改/未签名/build-id）" ok "全部通过" \
    "$DEMO" selftest "$WORK/selftest"

if [ -x "$ALT" ]; then
    run_case "跨 build-id：反例二进制导入必须被拒绝" fail "导入失败" \
        "$ALT" restore "$SNAP"
else
    echo "SKIP  $ALT 不存在，跳过跨 build-id 反例"
fi

# 未签名快照：清空密钥后 dump 必须失败（默认拒绝生成未签名快照）。
UNSIGNED_OUT="$(env -u MIGRATION_SIGNING_KEY timeout 120 "$DEMO" dump "$WORK/unsigned.mig" 2>&1)"
UNSIGNED_CODE=$?
if [ "$UNSIGNED_CODE" -ne 0 ]; then
    pass "未签名：缺少 MIGRATION_SIGNING_KEY 时 dump 被拒绝"
else
    fail "未签名：缺少 MIGRATION_SIGNING_KEY 时 dump 被拒绝" "exit=0"
fi

echo
echo "==== adopt-roundtrip: PASS=$PASS FAIL=$FAIL ===="
if [ "$FAIL" -ne 0 ]; then
    printf 'FAILED: %s\n' "${FAILED[*]}"
    exit 1
fi
exit 0
