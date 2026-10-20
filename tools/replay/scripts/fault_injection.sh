#!/usr/bin/env bash
# 故障注入演示：丢失一次唤醒 → 录制进程卡死 → 重放判定 stuck。
#
#   ./tools/replay/scripts/fault_injection.sh [build_dir]
#
# 两种注入：
#   1) 录制端注入（GO2CPP_REPLAY_DROP_WAKE）：真实进程卡死，看门狗补写 kEnd；
#   2) 重放端注入：正常记录 + 重放时丢弃一次 wake，重演卡死。
set -euo pipefail

build_dir="${1:-/tmp/go2cpp-replay-build}"
record_bin="${build_dir}/go2cpp_replay_record"
replay_bin="${build_dir}/go2cpp_replay_replay"
tool_bin="${build_dir}/replay"

stuck_file="/tmp/go2cpp-replay-stuck.g2cr"
good_file="/tmp/go2cpp-replay-good.g2cr"

echo "=== [1/3] 录制端注入：drop wake #20（进程应卡死，看门狗 5s 收尾）"
set +e
GO2CPP_REPLAY_DROP_WAKE=20 GO2CPP_REPLAY_WATCHDOG_SEC=3 \
    timeout 30 "${record_bin}" "${stuck_file}"
record_code=$?
set -e
echo "[fault] record exit=${record_code}（0=看门狗按预期强制收尾）"

echo "=== [2/3] 重放坏记录：应报告 stuck"
set +e
"${replay_bin}" "${stuck_file}"
replay_code=$?
set -e
echo "[fault] replay exit=${replay_code}（1=stuck）"

echo "=== [3/3] 正常记录 + 重放端注入 drop wake #20"
"${record_bin}" "${good_file}" >/dev/null
set +e
GO2CPP_REPLAY_DROP_WAKE=20 "${replay_bin}" "${good_file}"
inject_code=$?
set -e
echo "[fault] replay-with-injection exit=${inject_code}（1=stuck）"

echo "=== 极简工具对坏记录的 JSON 结论"
set +e
"${tool_bin}" "${stuck_file}" | head -c 400
echo
set -e

if [[ "${replay_code}" -eq 0 || "${inject_code}" -eq 0 ]]; then
    echo "[fault] 预期 stuck，但重放返回 0" >&2
    exit 1
fi
echo "[fault] 演示通过：两种注入都被重放判定为卡死"
