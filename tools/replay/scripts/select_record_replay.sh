#!/usr/bin/env bash
# select 多 channel 竞争：N 轮录制 + 重放，检查 chosen 序列与消息序列一致。
#
#   ./tools/replay/scripts/select_record_replay.sh [build_dir] [rounds]
set -euo pipefail

build_dir="${1:-/tmp/go2cpp-replay-build}"
rounds="${2:-5}"
select_bin="${build_dir}/go2cpp_replay_select"

if [[ ! -x "${select_bin}" ]]; then
    echo "[select] 缺少 ${select_bin}，先运行 tools/replay/build.sh" >&2
    exit 2
fi

failures=0
for round in $(seq 1 "${rounds}"); do
    file="/tmp/g2c-replay-select-${round}.g2cr"
    record_log="/tmp/g2c-replay-select-${round}.record.log"
    replay_log="/tmp/g2c-replay-select-${round}.replay.log"

    timeout 60 "${select_bin}" record "${file}" >"${record_log}" 2>&1
    timeout 60 "${select_bin}" replay "${file}" >"${replay_log}" 2>&1

    if grep -q "consistency=MATCH" "${replay_log}"; then
        choices=$(grep -o "selects=[0-9]*" "${replay_log}" | head -1)
        echo "[select] round ${round}: MATCH ${choices}"
    else
        failures=$((failures + 1))
        echo "[select] round ${round}: MISMATCH" >&2
        tail -12 "${replay_log}" >&2 || true
    fi
done

if [[ "${failures}" -ne 0 ]]; then
    echo "[select] ${failures}/${rounds} rounds failed" >&2
    exit 1
fi
echo "[select] all ${rounds} rounds matched"
