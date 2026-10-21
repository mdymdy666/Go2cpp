#!/usr/bin/env bash
# ctest 包装：scenario-deadlock-routes
#
# 120 client × 4 路由 → 锁顺序死锁 → Doctor 受控解锁的完整链路。
# 依赖：构建时 GO2CPP_DEBUG_DUMP=ON、GO2CPP_DIAGNOSTIC=ON、GO2CPP_WITH_CLI=ON；
#       构建目录解析顺序见 scenario_ctest_common.sh。
# 缺依赖 → exit 77（ctest SKIP），不 FAIL。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/scenarios/scenario_ctest_common.sh
source "${HERE}/scenario_ctest_common.sh"

if ! scenario_resolve_build_dir; then
    scenario_skip "未找到场景构建目录（可设 GO2CPP_SCENARIO_BUILD_DIR；候选 build-doctor-full / build-debug-full / build-enhanced）"
fi
BUILD_DIR="${SCENARIO_BUILD_DIR}"

scenario_require_exec "${BUILD_DIR}/go2cpp_deadlock_routes" " deadlock 场景服务端"
scenario_require_exec "${BUILD_DIR}/tools/cli/go2cpp-cli" " go2cpp-cli"

# 与手工验收参数一致：120 client + 600ms 加载。加载窗口缩短但保留
# 「>100 client 并发 → 锁顺序死锁 → Doctor 受控解锁」的完整路径。
exec bash "${HERE}/deadlock_routes/run_deadlock_scenario.sh" "${BUILD_DIR}" \
    --clients 120 --load-ms 600
