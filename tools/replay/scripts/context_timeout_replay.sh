#!/usr/bin/env bash
# Context / 超时 / 虚拟时钟：录制端真实等待 ~100ms，重放端只推进虚拟时间。
#
#   ./tools/replay/scripts/context_timeout_replay.sh [build_dir] [rounds]
set -euo pipefail

build_dir="${1:-/tmp/go2cpp-replay-build}"
rounds="${2:-3}"
context_bin="${build_dir}/go2cpp_replay_context"

if [[ ! -x "${context_bin}" ]]; then
    echo "[context] 缺少 ${context_bin}，先运行 tools/replay/build.sh" >&2
    exit 2
fi

failures=0
for round in $(seq 1 "${rounds}"); do
    file="/tmp/g2c-replay-context-${round}.g2cr"
    record_log="/tmp/g2c-replay-context-${round}.record.log"
    replay_log="/tmp/g2c-replay-context-${round}.replay.log"

    timeout 60 "${context_bin}" record "${file}" >"${record_log}" 2>&1
    timeout 60 "${context_bin}" replay "${file}" >"${replay_log}" 2>&1

    if grep -q "consistency=MATCH virtual_clock=OK" "${replay_log}"; then
        wall=$(grep -o "wall_ms=[0-9.]*" "${replay_log}" | head -1)
        virtual=$(grep -o "virtual_ms=[0-9.]*" "${replay_log}" | head -1)
        echo "[context] round ${round}: MATCH ${wall} ${virtual}"
    else
        failures=$((failures + 1))
        echo "[context] round ${round}: MISMATCH/SLOW" >&2
        tail -10 "${replay_log}" >&2 || true
    fi
done

if [[ "${failures}" -ne 0 ]]; then
    echo "[context] ${failures}/${rounds} rounds failed" >&2
    exit 1
fi
echo "[context] all ${rounds} rounds matched with virtual clock"
