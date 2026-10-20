#!/usr/bin/env bash
# 非 i64 值：std::string 与带 Serialize/Deserialize 的结构体录制/重放一致。
#
#   ./tools/replay/scripts/values_record_replay.sh [build_dir] [rounds]
set -euo pipefail

build_dir="${1:-/tmp/go2cpp-replay-build}"
rounds="${2:-3}"
values_bin="${build_dir}/go2cpp_replay_values"

if [[ ! -x "${values_bin}" ]]; then
    echo "[values] 缺少 ${values_bin}，先运行 tools/replay/build.sh" >&2
    exit 2
fi

failures=0
for round in $(seq 1 "${rounds}"); do
    file="/tmp/g2c-replay-values-${round}.g2cr"
    record_log="/tmp/g2c-replay-values-${round}.record.log"
    replay_log="/tmp/g2c-replay-values-${round}.replay.log"

    timeout 60 "${values_bin}" record "${file}" >"${record_log}" 2>&1
    timeout 60 "${values_bin}" replay "${file}" >"${replay_log}" 2>&1

    if grep -q "consistency=MATCH" "${replay_log}"; then
        echo "[values] round ${round}: MATCH $(grep -o 'bytes=[0-9]*' "${replay_log}" | head -1)"
    else
        failures=$((failures + 1))
        echo "[values] round ${round}: MISMATCH" >&2
        tail -10 "${replay_log}" >&2 || true
    fi
done

if [[ "${failures}" -ne 0 ]]; then
    echo "[values] ${failures}/${rounds} rounds failed" >&2
    exit 1
fi
echo "[values] all ${rounds} rounds matched"
