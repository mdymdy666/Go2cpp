#!/usr/bin/env bash
# Go2Cpp 五能力增强测试一键入口。
#
# 顺序：Debug 主矩阵/父 park/压测 → 迁移主矩阵/压测 → 回放主矩阵/压测
#       → CLI 主矩阵/压测 → Doctor 主矩阵 → 报告。
#
# 设计：
# - 产品缺陷（FAIL）不会中止后续能力，全部跑完后汇总；
# - 只有脚本自身超时/崩溃（harness）才让本脚本返回非 0；
# - 矩阵 / 统计 / 报告分别写 /tmp/test_enhance_matrix.tsv、
#   /tmp/test_enhance_stats.tsv、/tmp/test_enhance.md（可用 MATRIX_FILE /
#   STATS_FILE / REPORT_FILE 覆盖）；这些只是输出文件，不是构建依赖。
# - 构建产物按「环境变量 → build-enhanced → 各能力构建目录」解析；
#   不再依赖 /tmp/go2cpp-cli-build。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENHANCED="$ROOT/tests/enhanced"
export MATRIX_FILE="${MATRIX_FILE:-/tmp/test_enhance_matrix.tsv}"
export STATS_FILE="${STATS_FILE:-/tmp/test_enhance_stats.tsv}"
LOG_DIR="${LOG_DIR:-/tmp/test_enhance_logs}"
rm -f "$MATRIX_FILE" "$STATS_FILE"
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
printf 'capability\tcase\tkind\tstatus\tdetail\n' > "$MATRIX_FILE"
printf 'capability\tname\tvalue\tdetail\n' > "$STATS_FILE"

HARNESS_FAIL=0

run_one() { # name script timeout
    local name="$1" script="$2" limit="$3"
    echo "=== [$(date +%H:%M:%S)] $name"
    timeout "$limit" bash "$script" >"$LOG_DIR/$name.log" 2>&1
    local code=$?
    if [ "$code" -eq 124 ] || [ "$code" -eq 137 ] || [ "$code" -eq 139 ] || [ "$code" -eq 127 ]; then
        HARNESS_FAIL=$((HARNESS_FAIL + 1))
        printf 'harness\t%s\tharness\tFAIL\ttimeout/exit=%s\n' "$name" "$code" >> "$MATRIX_FILE"
        echo "    HARNESS-FAIL exit=$code（见 $LOG_DIR/$name.log）"
    else
        tail -2 "$LOG_DIR/$name.log" | sed 's/^/    /'
    fi
}

# 构建产物定位：环境变量优先，其次探测工作树内的构建目录（见 lib/common.sh）。
# 旧版本硬编码 /tmp/go2cpp-cli-build，清理 /tmp 或换机器后入口直接失效。
source "$ENHANCED/lib/common.sh"
common_init

echo "=== 构建产物解析"
echo "    ROOT       = $ROOT"
echo "    CLI        = $CLI"
echo "    DEBUG_DEMO = $DEBUG_DEMO"
echo "    DIAG_DEMO  = $DIAG_DEMO"
echo "    REPLAY_DIR = $REPLAY_DIR"
echo "    MIG_DIR    = $MIG_DIR"

if [ ! -x "$CLI" ]; then
    echo "缺少 CLI 构建产物：$CLI" >&2
    echo "请先构建其一，或用 GO2CPP_CLI=<path> 指定：" >&2
    echo "  cmake -S . -B build-enhanced -DGO2CPP_BUILD_ENHANCED=ON -DGO2CPP_WITH_CLI=ON" >&2
    echo "  cmake --build build-enhanced -j" >&2
    echo "  # 或最小组件构建：cmake -S . -B build-cli -DGO2CPP_WITH_CLI=ON && cmake --build build-cli -j" >&2
    echo "SKIP: 缺少 CLI，五能力增强矩阵未执行" >&2
    exit 77
fi

run_one "debug-matrix" "$ENHANCED/debug/test_debug.sh" 180
run_one "debug-parent-park" "$ENHANCED/debug/test_parent_park.sh" 120
run_one "debug-stress" "$ENHANCED/debug/stress_debug.sh" 300
run_one "migration-matrix" "$ENHANCED/migration/test_migration.sh" 180
run_one "migration-stress" "$ENHANCED/migration/stress_migration.sh" 180
run_one "replay-matrix" "$ENHANCED/replay/test_replay.sh" 180
run_one "replay-stress" "$ENHANCED/replay/stress_replay.sh" 180
run_one "cli-matrix" "$ENHANCED/cli/test_cli.sh" 180
run_one "cli-stress" "$ENHANCED/cli/stress_cli.sh" 180
run_one "doctor-matrix" "$ENHANCED/doctor/test_doctor.sh" 180

echo "=== [$(date +%H:%M:%S)] report"
python3 "$ENHANCED/report.py" | sed 's/^/    /'

if [ "$HARNESS_FAIL" -ne 0 ]; then
    echo "HARNESS_FAIL=$HARNESS_FAIL（测试脚本自身超时/崩溃，见 $LOG_DIR）" >&2
    exit 1
fi
echo "全部能力脚本执行完毕（产品 FAIL 见报告；矩阵 $MATRIX_FILE）"
exit 0
