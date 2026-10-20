#!/usr/bin/env bash
# 检查点分片 + 只重放到出事点：
#   - split 产生多个前缀分片；
#   - 中间分片 info/play → partial（到达检查点）；
#   - 最后一个分片是完整记录 → ok；
#   - 完整文件 --until=SEQ → partial，且不判 stuck。
#
#   ./tools/replay/scripts/chunked_replay.sh [build_dir]
set -euo pipefail

build_dir="${1:-/tmp/go2cpp-replay-build}"
tool_bin="${build_dir}/replay"
record_bin="${build_dir}/go2cpp_replay_record"

if [[ ! -x "${tool_bin}" || ! -x "${record_bin}" ]]; then
    echo "[chunked] 缺少构建产物，先运行 tools/replay/build.sh" >&2
    exit 2
fi

file="/tmp/g2c-replay-chunked.g2cr"
out_dir="/tmp/g2c-replay-chunked-parts"
rm -rf "${out_dir}"
timeout 60 "${record_bin}" "${file}" >/dev/null

"${tool_bin}" split "${file}" --every-events=100 --out-dir="${out_dir}" \
    > /tmp/g2c-replay-chunked.split.json 2>/dev/null

part1=$(ls "${out_dir}"/*part0001.g2cr)
part4=$(ls "${out_dir}"/*part0004.g2cr)

if ! "${tool_bin}" info "${part1}" | grep -q '"status":"partial"'; then
    echo "[chunked] part1 不是 partial" >&2
    exit 1
fi
if ! "${tool_bin}" play "${part1}" 2>/dev/null | grep -q '"status":"partial"'; then
    echo "[chunked] part1 play 不是 partial" >&2
    exit 1
fi
if ! "${tool_bin}" play "${part4}" 2>/dev/null | grep -q '"status":"ok"'; then
    echo "[chunked] part4 play 不是 ok" >&2
    exit 1
fi
if ! "${tool_bin}" play "${file}" --until=200 2>/dev/null | grep -q '"status":"partial"'; then
    echo "[chunked] --until=200 不是 partial" >&2
    exit 1
fi
if ! "${tool_bin}" play "${file}" 2>/dev/null | grep -q '"status":"ok"'; then
    echo "[chunked] 完整重放不是 ok" >&2
    exit 1
fi

echo "[chunked] split=${out_dir} part1=partial part4=ok until=partial"
