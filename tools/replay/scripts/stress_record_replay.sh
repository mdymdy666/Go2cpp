#!/usr/bin/env bash
# 压测：连续录制 + 重放 N 轮，每轮真实调度交错不同，检查 replay 均一致。
#
#   ./tools/replay/scripts/stress_record_replay.sh [build_dir] [rounds]
#
# 退出码 0 表示全部轮次 MATCH；非 0 表示有轮次不一致（会打印日志尾部）。
set -euo pipefail

build_dir="${1:-/tmp/go2cpp-replay-build}"
rounds="${2:-10}"
record_bin="${build_dir}/go2cpp_replay_record"
replay_bin="${build_dir}/go2cpp_replay_replay"

if [[ ! -x "${record_bin}" || ! -x "${replay_bin}" ]]; then
    echo "[stress] 缺少可执行文件，先运行 tools/replay/build.sh" >&2
    exit 2
fi

failures=0
for round in $(seq 1 "${rounds}"); do
    file="/tmp/go2cpp-replay-stress-${round}.g2cr"
    record_log="/tmp/go2cpp-replay-stress-${round}.record.log"
    replay_log="/tmp/go2cpp-replay-stress-${round}.replay.log"

    timeout 60 "${record_bin}" "${file}" >"${record_log}" 2>&1
    timeout 60 "${replay_bin}" "${file}" >"${replay_log}" 2>&1

    if grep -q "consistency=MATCH" "${replay_log}"; then
        events=$(grep -o "events=[0-9]*" "${record_log}" | head -1)
        echo "[stress] round ${round}: MATCH ${events}"
    else
        failures=$((failures + 1))
        echo "[stress] round ${round}: MISMATCH (${replay_log})" >&2
        tail -8 "${replay_log}" >&2 || true
    fi
done

if [[ "${failures}" -ne 0 ]]; then
    echo "[stress] ${failures}/${rounds} rounds failed" >&2
    exit 1
fi
echo "[stress] all ${rounds} rounds matched"
