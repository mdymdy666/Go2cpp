#!/usr/bin/env bash
# ctest 包装：scenario-ctx-cancel
#
# Doctor ctx.cancel + ContextRollback undo 解死锁（cancel / force / external
# 三模式）。依赖：GO2CPP_DEBUG_DUMP=ON + GO2CPP_DIAGNOSTIC=ON + python3。
# 缺依赖 → exit 77（ctest SKIP），不 FAIL。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/scenarios/scenario_ctest_common.sh
source "${HERE}/scenario_ctest_common.sh"

if ! scenario_resolve_build_dir; then
    scenario_skip "未找到场景构建目录（可设 GO2CPP_SCENARIO_BUILD_DIR；候选 build-doctor-full / build-debug-full / build-enhanced）"
fi
BUILD_DIR="${SCENARIO_BUILD_DIR}"

scenario_require_exec "${BUILD_DIR}/go2cpp_ctx_cancel_rollback" " ctx_cancel 场景 demo"
scenario_require_python3
if [ ! -f "${HERE}/../debug/doctor_client.py" ]; then
    scenario_skip "缺少 Doctor 客户端 tests/debug/doctor_client.py"
fi

exec bash "${HERE}/ctx_cancel_rollback/run_ctx_cancel_rollback.sh" "${BUILD_DIR}"
