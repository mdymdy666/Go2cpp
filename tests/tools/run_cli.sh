#!/usr/bin/env bash
# ctest 包装：go2cpp-cli 验收测试（213 项断言）。
#
# 复用工作树中已构建的 go2cpp-cli（build-cli / build-enhanced / build-work）。
# 找不到时以 77 退出，CMake 侧 SKIP_RETURN_CODE=77 记为 SKIP。
#
# 环境变量：
#   GO2CPP_CLI_BIN  显式指定 go2cpp-cli 路径（优先）
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${GO2CPP_CLI_BIN:-}"

if [ -z "$BIN" ]; then
    for candidate in "$ROOT/build-cli/tools/cli/go2cpp-cli" "$ROOT/build-enhanced/tools/cli/go2cpp-cli" "$ROOT/build-doctor-full/tools/cli/go2cpp-cli" "$ROOT/build-work/tools/cli/go2cpp-cli"; do
        if [ -x "$candidate" ]; then
            BIN="$candidate"
            break
        fi
    done
fi

if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "SKIP tools-cli：未找到 go2cpp-cli"
    echo "  期望: $GO2CPP_CLI_BIN 或 <repo>/build-cli/tools/cli/go2cpp-cli"
    echo "  构建: cmake -S . -B build-cli -DGO2CPP_WITH_CLI=ON && cmake --build build-cli -j"
    exit 77
fi

echo "tools-cli: bin=$BIN"
exec env BIN="$BIN" bash "$ROOT/tools/cli/tests/run_all.sh"
