#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# P2P 模块测试统一入口。
#
# 用法：
#   tests/p2p/run_p2p_tests.sh [构建目录] [--bench]
#
# 默认构建目录为 .build-bento-wsl；--bench 额外运行基准（不注册 ctest）。
# 退出码：0 全部通过；1 有测试失败；2 构建目录/可执行文件缺失。
# ---------------------------------------------------------------------------
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${1:-${PROJECT_ROOT}/.build-bento-wsl}"
RUN_BENCH=0
if [[ "${2:-}" == "--bench" || "${1:-}" == "--bench" ]]; then
    RUN_BENCH=1
    if [[ "${1:-}" == "--bench" ]]; then
        BUILD_DIR="${PROJECT_ROOT}/.build-bento-wsl"
    fi
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "[ERROR] 构建目录不存在: ${BUILD_DIR}" >&2
    echo "        先执行: cmake -S . -B .build-bento-wsl && cmake --build .build-bento-wsl -j" >&2
    exit 2
fi

TESTS=(
    p2p_test_stun
    p2p_test_p2p
    p2p_test_tcp_punch
    p2p_test_p2p_interprocess
)

failures=0
for test_name in "${TESTS[@]}"; do
    binary="${BUILD_DIR}/${test_name}"
    if [[ ! -x "${binary}" ]]; then
        echo "[ERROR] 缺少可执行文件: ${binary}" >&2
        exit 2
    fi
    echo "=== ${test_name} ==="
    if ! "${binary}"; then
        echo "[FAIL] ${test_name}" >&2
        failures=$((failures + 1))
    fi
done

if [[ "${RUN_BENCH}" -eq 1 ]]; then
    if [[ -x "${BUILD_DIR}/p2p_bench" ]]; then
        echo "=== p2p_bench ==="
        "${BUILD_DIR}/p2p_bench"
    else
        echo "[WARN] 未构建 p2p_bench，跳过基准" >&2
    fi
fi

if [[ "${failures}" -ne 0 ]]; then
    echo "[RESULT] ${failures} 个测试文件失败" >&2
    exit 1
fi
echo "[RESULT] P2P 测试全部通过"
