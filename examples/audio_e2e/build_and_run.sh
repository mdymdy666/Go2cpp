#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 一条命令跑完音频端到端演示：
#   1) 构建主工程（若 libgo2cpp_media.a 不存在）；
#   2) 构建 examples/audio_e2e；
#   3) 跑 5 个场景（进程内 PCM/G.711/Opus、弱网、UDP、可选 Pulse 真实设备）。
#
# 用法：
#   ./build_and_run.sh              # 只跑内存/UDP 场景
#   ./build_and_run.sh --real       # 额外跑 PulseAudio 真实采集+播放
#   ./build_and_run.sh --frames 200 --jitter-ms 30 --drop-every 20
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
BUILD="${ROOT}/.build-audio-e2e"
MAIN_BUILD="${ROOT}/.build-bento-wsl"

if [[ ! -f "${MAIN_BUILD}/libgo2cpp_media.a" ]]; then
    echo "[1/3] 构建主工程（首次较慢）..."
    cmake -S "${ROOT}" -B "${MAIN_BUILD}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DGO2CPP_WITH_BENTO=ON \
        -DGO2CPP_WITH_MEDIA=ON
    cmake --build "${MAIN_BUILD}" -j "$(nproc)"
else
    echo "[1/3] 主工程已构建：${MAIN_BUILD}/libgo2cpp_media.a"
fi

echo "[2/3] 构建 audio_e2e ..."
cmake -S "${HERE}" -B "${BUILD}" -G Ninja -DGO2CPP_ROOT="${ROOT}"
cmake --build "${BUILD}" -j "$(nproc)"

echo "[3/3] 运行演示 ..."
mkdir -p "${BUILD}/artifacts"
"${BUILD}/audio_e2e_demo" --artifacts "${BUILD}/artifacts" "$@"
echo
echo "产物：${BUILD}/artifacts/session.json 与 captured.wav"
