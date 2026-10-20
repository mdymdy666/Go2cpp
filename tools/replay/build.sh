#!/usr/bin/env bash
# 构建 tools/replay（独立工程，不改顶层 CMake，不影响默认构建）。
#
#   ./tools/replay/build.sh            # 构建库 + CLI + demo + 独立单测
#   GO2CPP_REPLAY_BUILD_DIR=/tmp/x ./tools/replay/build.sh
#   GO2CPP_REPLAY_LIB_DIR=$PWD/build-integration ./tools/replay/build.sh
#
# 需要先在指定目录构建出 libgo2cpp_*.so（默认工作树根目录的 build-work/）。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../.." && pwd)"
build_dir="${GO2CPP_REPLAY_BUILD_DIR:-/tmp/go2cpp-replay-build}"
lib_dir="${GO2CPP_REPLAY_LIB_DIR:-${root}/build-work}"

if [[ ! -d "${lib_dir}" ]]; then
    echo "[replay] 缺少 ${lib_dir}；请先在根目录完成一次常规构建" >&2
    exit 1
fi

cmake -S "${here}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGO2CPP_REPLAY=ON \
    -DGO2CPP_REPLAY_ROOT="${root}" \
    -DGO2CPP_REPLAY_LIB_DIR="${lib_dir}"
cmake --build "${build_dir}" -j"$(nproc)"

echo "[replay] built:"
echo "  ${build_dir}/replay"
echo "  ${build_dir}/go2cpp_replay_record"
echo "  ${build_dir}/go2cpp_replay_replay"
echo "  ${build_dir}/go2cpp_replay_select"
echo "  ${build_dir}/go2cpp_replay_context"
echo "  ${build_dir}/go2cpp_replay_values"
echo "  ${build_dir}/go2cpp_replay_tests"
echo "[replay] ctest: ctest --test-dir ${build_dir} --output-on-failure"
echo "[replay] all scenarios: ${here}/scripts/run_all.sh ${build_dir}"
