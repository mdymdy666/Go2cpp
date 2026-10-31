#!/usr/bin/env bash
# G2CRPLY v5 资源事件录制/回放 demo 入口。
#
#   tests/replay/run_resources_v5_demo.sh [build-dir]
#
# 需要一个 GO2CPP_REPLAY=ON 的完整构建（默认 build-enhanced）。
# 退出码：0 = 录制与回放计数/checksum 全部一致；非 0 = 失败。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../.." && pwd)"
build_dir="${1:-build-enhanced}"
bin="${root}/${build_dir}/go2cpp_resources_v5_demo"

if [[ ! -x "${bin}" ]]; then
    echo "[v5-demo] 缺少 ${bin}" >&2
    echo "[v5-demo] 先构建：cmake --build ${root}/${build_dir} --target go2cpp_resources_v5_demo" >&2
    exit 2
fi

recording="$(mktemp "/tmp/go2cpp_resources_v5_demo_XXXXXX.g2cr")"
trap 'rm -f "${recording}"' EXIT

output="$("${bin}" "${recording}")"
printf '%s\n' "${output}"

if ! grep -q '^\[v5-demo\] MATCH ' <<<"${output}"; then
    echo "[v5-demo] 回放结果与录制不一致" >&2
    exit 1
fi
echo "[v5-demo] OK"
