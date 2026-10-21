#!/usr/bin/env bash
# ctest 包装：tools/migration 的 53 项场景测试。
#
# 复用工作树中已构建的 tools/migration 独立工程（ctest 不应暗地里触发
# 一次完整构建）。找不到构建目录时打印 SKIP 原因并以 77 退出；CMake 侧
# 用 SKIP_RETURN_CODE=77 记为 SKIP 而不是 FAIL。
#
# 环境变量：
#   GO2CPP_MIGRATION_BUILD_DIR  显式指定构建目录（优先）
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${GO2CPP_MIGRATION_BUILD_DIR:-}"

if [ -z "$BUILD" ]; then
    for candidate in "$ROOT/build-migration"                      "$ROOT/build-migration-adopt"                      "$ROOT/build-migration-tests"; do
        if [ -x "$candidate/migrate" ]; then
            BUILD="$candidate"
            break
        fi
    done
fi

if [ -z "$BUILD" ] || [ ! -x "$BUILD/migrate" ]; then
    echo "SKIP tools-migration：未找到 tools/migration 构建产物"
    echo "  期望: $GO2CPP_MIGRATION_BUILD_DIR 或 <repo>/build-migration/migrate"
    echo "  构建: cmake -S tools/migration -B build-migration && cmake --build build-migration -j"
    exit 77
fi

echo "tools-migration: build_dir=$BUILD"
exec bash "$ROOT/tools/migration/tests/run_tests.sh" "$BUILD"
