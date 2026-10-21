#!/usr/bin/env bash
# ctest 包装：tools/replay 单测 + 6 个端到端脚本场景。
#
# 复用工作树中已构建的 tools/replay 独立工程（build-replay / build-enhanced）。
# 找不到时以 77 退出（CMake 侧 SKIP_RETURN_CODE=77 记为 SKIP 而不是 FAIL）。
#
# 环境变量：
#   GO2CPP_REPLAY_BUILD_DIR  显式指定构建目录（优先）
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="${GO2CPP_REPLAY_BUILD_DIR:-}"

if [ -z "$DIR" ]; then
    for candidate in "$ROOT/build-replay" "$ROOT/build-enhanced"; do
        if [ -x "$candidate/go2cpp_replay_tests" ] && [ -x "$candidate/replay" ]; then
            DIR="$candidate"
            break
        fi
    done
fi

if [ -z "$DIR" ] || [ ! -x "$DIR/go2cpp_replay_tests" ] || [ ! -x "$DIR/replay" ]; then
    echo "SKIP tools-replay：未找到 tools/replay 构建产物"
    echo "  期望: $GO2CPP_REPLAY_BUILD_DIR 或 <repo>/build-replay/{replay,go2cpp_replay_tests}"
    echo "  构建: GO2CPP_REPLAY_BUILD_DIR=build-replay ./tools/replay/build.sh"
    exit 77
fi

echo "tools-replay: build_dir=$DIR"
echo "--- [1/2] 单元测试：go2cpp_replay_tests"
"$DIR/go2cpp_replay_tests"
unit=$?
if [ "$unit" -ne 0 ]; then
    echo "tools-replay: 单元测试失败（exit=$unit）"
    exit "$unit"
fi

echo "--- [2/2] 端到端场景：tools/replay/scripts/run_all.sh"
exec bash "$ROOT/tools/replay/scripts/run_all.sh" "$DIR"
