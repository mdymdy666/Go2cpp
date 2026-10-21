#!/usr/bin/env bash
# 场景测试的 ctest 包装公共层。
#
# 职责：
#   1) 解析场景构建目录，优先级：
#        GO2CPP_SCENARIO_BUILD_DIR > 当前工作目录（ctest 的构建目录）>
#        build-doctor-full > build-debug-full > build-enhanced
#      显式指定 GO2CPP_SCENARIO_BUILD_DIR 时只认它：指错目录立即报告，
#      不会静默回退到别的构建，避免「测了另一个目录却以为测的是这个」。
#   2) 缺依赖（构建目录 / 可执行 / CLI / python3）时以 77 退出，配合
#      cmake 里的 SKIP_RETURN_CODE=77 记为 Skipped，不把「环境没准备」
#      伪装成 Failed，也不伪装成 Passed。
#
# 用法（见同目录 run_*_ctest.sh）：
#   source "$(dirname "${BASH_SOURCE[0]}")/scenario_ctest_common.sh"
#   if ! scenario_resolve_build_dir; then
#       scenario_skip "未找到场景构建目录"
#   fi
#   BUILD_DIR="$SCENARIO_BUILD_DIR"
set -u

SCENARIO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCENARIO_BUILD_DIR=""

# scenario_skip <原因>：输出 SKIP 原因并以 77 退出。
scenario_skip() {
    echo "SKIP: $*" >&2
    exit 77
}

# scenario_resolve_build_dir：成功时设置全局 SCENARIO_BUILD_DIR 并返回 0；
# 没有可用构建目录时返回 1（调用方负责 scenario_skip 给出统一提示）。
scenario_resolve_build_dir() {
    local candidate
    if [ -n "${GO2CPP_SCENARIO_BUILD_DIR:-}" ]; then
        if [ -f "${GO2CPP_SCENARIO_BUILD_DIR}/CMakeCache.txt" ]; then
            SCENARIO_BUILD_DIR="${GO2CPP_SCENARIO_BUILD_DIR}"
            return 0
        fi
        echo "显式指定的 GO2CPP_SCENARIO_BUILD_DIR=${GO2CPP_SCENARIO_BUILD_DIR}" \
            "不是 CMake 构建目录（缺 CMakeCache.txt）" >&2
        return 1
    fi
    # ctest 运行时的工作目录就是构建目录：优先测「当前构建」，
    # 避免在 build-enhanced 里跑 ctest 却命中旧的 build-doctor-full 二进制。
    if [ -f "${PWD}/CMakeCache.txt" ]; then
        SCENARIO_BUILD_DIR="${PWD}"
        echo "使用当前工作目录作为场景构建目录：${SCENARIO_BUILD_DIR}" >&2
        return 0
    fi
    for candidate in \
        "${SCENARIO_ROOT}/build-doctor-full" \
        "${SCENARIO_ROOT}/build-debug-full" \
        "${SCENARIO_ROOT}/build-enhanced"; do
        if [ -f "${candidate}/CMakeCache.txt" ]; then
            SCENARIO_BUILD_DIR="${candidate}"
            echo "使用场景构建目录：${SCENARIO_BUILD_DIR}" >&2
            return 0
        fi
    done
    return 1
}

# scenario_require_exec <路径> <描述>：缺可执行文件时 SKIP。
scenario_require_exec() {
    if [ ! -x "$1" ]; then
        scenario_skip "缺少$2：$1（先用 -DGO2CPP_BUILD_ENHANCED=ON -DGO2CPP_WITH_CLI=ON 构建）"
    fi
}

# scenario_require_python3：缺 python3 时 SKIP。
scenario_require_python3() {
    if ! command -v python3 >/dev/null 2>&1; then
        scenario_skip "需要 python3（场景脚本用它校验视图/驱动 Doctor）"
    fi
}
