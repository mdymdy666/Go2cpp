#!/usr/bin/env bash
# ctest 包装：scenario-full-trace
#
# 混合 Fiber 场景跑满 10s，抓 6 个 JSON 视图（timeline/gmp/sched/park/timer/
# primitives）并做 park↔wake / enqueue↔claim / timer→G 覆盖率校验。
# 依赖：GO2CPP_DEBUG_DUMP=ON + GO2CPP_WITH_CLI=ON + python3。
# 缺依赖 → exit 77（ctest SKIP），不 FAIL。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/scenarios/scenario_ctest_common.sh
source "${HERE}/scenario_ctest_common.sh"

if ! scenario_resolve_build_dir; then
    scenario_skip "未找到场景构建目录（可设 GO2CPP_SCENARIO_BUILD_DIR；候选 build-doctor-full / build-debug-full / build-enhanced）"
fi
BUILD_DIR="${SCENARIO_BUILD_DIR}"

scenario_require_exec "${BUILD_DIR}/go2cpp_full_trace_scenario" " full_trace 场景"
scenario_require_exec "${BUILD_DIR}/tools/cli/go2cpp-cli" " go2cpp-cli"
scenario_require_python3

# 输出落在构建目录内（不污染 /tmp，也便于 ctest 失败时直接查看 JSON/日志）。
OUT_DIR="${GO2CPP_SCENARIO_OUT_DIR:-${BUILD_DIR}/scenario-full-trace}"
exec env \
    SCENARIO="${BUILD_DIR}/go2cpp_full_trace_scenario" \
    CLI="${BUILD_DIR}/tools/cli/go2cpp-cli" \
    OUT="${OUT_DIR}" \
    bash "${HERE}/full_trace/run_full_trace.sh" 10 8
