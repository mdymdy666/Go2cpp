#!/usr/bin/env bash
# go2cpp::error 增强测试一键入口：配置 + 构建 + ctest。
#
# 环境变量：
#   BUILD_DIR  构建目录（默认 /tmp/go2cpp-error-enhanced-build）
#   JOBS       并行度（默认 nproc）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/go2cpp-error-enhanced-build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure
