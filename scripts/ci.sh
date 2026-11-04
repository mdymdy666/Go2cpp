#!/usr/bin/env bash
# =============================================================================
# scripts/ci.sh —— 一条命令验证 Go2Cpp（configure + build + ctest）。
#
# 用法：
#   bash scripts/ci.sh                     # 默认 --quick：Release 全量构建 + 全量 ctest
#   bash scripts/ci.sh --full              # 额外 ASAN 构建 + 并发/网络子集 + verify_metrics --quick
#   bash scripts/ci.sh --build-dir PATH    # 指定构建目录（默认 $PWD/.ci-build；CI 用 /tmp/...）
#   bash scripts/ci.sh --no-configure      # 复用已有构建目录，只跑 build + ctest
#   bash scripts/ci.sh --jobs N            # 并行度（默认 nproc）
#
# 退出码：0 = 所有步骤成功；1 = 任一步骤失败；2 = 用法/环境错误。
#
# 设计说明：
#   - 构建目录默认放在仓库内的 .ci-build，CI 里通过 --build-dir 指到 /tmp；
#     所有日志写入 <build-dir>/ci-logs/，不污染源码树。
#   - 生成器：新建目录且系统有 ninja 时用 Ninja，否则用 CMake 默认；
#     已有 CMakeCache.txt 的目录不再传 -G（避免与缓存里的生成器冲突）。
#   - 构建开关对齐 README「四、构建与测试」的主构建（Bento + MCP + app + p2p），
#     另加 SQLite（CI 安装 libsqlite3-dev；本地缺依赖时 configure 会报错）。
#   - --full 的 ASAN：仓库当前没有 GO2CPP_ENABLE_ASAN 选项（已 grep 确认），
#     因此走编译器 flag；若将来该选项出现，脚本自动改用它。
#   - tools/verify_metrics.sh 硬编码 $ROOT/.build-bento-wsl 作为构建目录；
#     --full 时若该目录不存在，脚本临时建符号链接指向当前构建目录，退出时删除。
# =============================================================================
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd -P)"
MODE="quick"
BUILD_DIR=""
JOBS=""
DO_CONFIGURE=1
VERIFY_LINK_CREATED=0
START_ALL=$(date +%s)

usage() {
    sed -n '3,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --quick) MODE="quick" ;;
        --full) MODE="full" ;;
        --build-dir)
            [ $# -ge 2 ] || { printf '错误: --build-dir 需要参数\n' >&2; exit 2; }
            BUILD_DIR="$2"; shift ;;
        --no-configure) DO_CONFIGURE=0 ;;
        --jobs)
            [ $# -ge 2 ] || { printf '错误: --jobs 需要参数\n' >&2; exit 2; }
            JOBS="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf '错误: 未知参数 %s（用 --help 看用法）\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$PWD/.ci-build"
fi

if [ -z "$JOBS" ]; then
    JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

mkdir -p "$BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"
LOG_DIR="$BUILD_DIR/ci-logs"
mkdir -p "$LOG_DIR"

cleanup() {
    # 只删本脚本创建的符号链接，绝不碰真实目录。
    if [ "$VERIFY_LINK_CREATED" -eq 1 ] && [ -L "$ROOT/.build-bento-wsl" ]; then
        rm -f "$ROOT/.build-bento-wsl"
    fi
}
trap cleanup EXIT

fmt_duration() {
    local s="$1"
    if [ "$s" -ge 3600 ]; then
        printf '%dh%02dm%02ds' $((s / 3600)) $((s % 3600 / 60)) $((s % 60))
    elif [ "$s" -ge 60 ]; then
        printf '%dm%02ds' $((s / 60)) $((s % 60))
    else
        printf '%ds' "$s"
    fi
}

STEP_NAME=""
STEP_START=0
step_begin() {
    STEP_NAME="$1"
    STEP_START=$(date +%s)
    printf '[ci] %s ...\n' "$STEP_NAME"
}
step_ok() {
    local dur=$(( $(date +%s) - STEP_START ))
    printf '[ci] %s %s OK\n' "$STEP_NAME" "$(fmt_duration "$dur")"
}
step_fail() {
    local dur=$(( $(date +%s) - STEP_START ))
    printf '[ci] %s %s FAIL\n' "$STEP_NAME" "$(fmt_duration "$dur")" >&2
    exit 1
}

# 统计 ctest 的 passed/failed/skipped：
#   优先解析 --output-junit 的 XML（ctest 的输出行随版本变化，不可靠）；
#   没有 python3 或没有 XML 时退回 ctest 汇总行，缺失项记 '?'。
CTEST_PASSED="?"
CTEST_FAILED="?"
CTEST_SKIPPED="?"
parse_ctest() {
    local junit="$1" log="$2"
    if [ -f "$junit" ] && command -v python3 >/dev/null 2>&1; then
        local out
        out="$(python3 - "$junit" <<'PY'
import sys
import xml.etree.ElementTree as ET

try:
    root = ET.parse(sys.argv[1]).getroot()
except Exception:
    print("? ? ?")
    raise SystemExit(0)

suite = root if root.tag == "testsuite" else root.find("testsuite")
if suite is None:
    print("? ? ?")
    raise SystemExit(0)


def num(name):
    try:
        return int(suite.get(name, "0") or 0)
    except ValueError:
        return 0


tests = num("tests")
failed = num("failures") + num("errors")
skipped = num("skipped") + num("disabled")
passed = max(tests - failed - skipped, 0)
print(passed, failed, skipped)
PY
)"
        if [ -n "$out" ] && [ "$out" != "? ? ?" ]; then
            read -r CTEST_PASSED CTEST_FAILED CTEST_SKIPPED <<<"$out"
            return
        fi
    fi
    # 兜底：从汇总行 "XX% tests passed, N tests failed out of M" 取总数与失败数。
    local line total
    line="$(grep -E 'tests passed, .* tests failed out of' "$log" 2>/dev/null | tail -1 || true)"
    if [ -n "$line" ]; then
        CTEST_FAILED="$(printf '%s\n' "$line" | sed -E 's/.* ([0-9]+) tests failed.*/\1/')"
        total="$(printf '%s\n' "$line" | sed -E 's/.* out of ([0-9]+).*/\1/')"
        CTEST_PASSED=$((total - CTEST_FAILED))
        CTEST_SKIPPED="0"
    fi
}

has_asan_option() {
    grep -rq 'option(GO2CPP_ENABLE_ASAN\|GO2CPP_ENABLE_ASAN' \
        "$ROOT/CMakeLists.txt" "$ROOT"/cmake/*.cmake 2>/dev/null
}

# 主构建开关（README「四、构建与测试」主构建 + SQLite）。
configure_args=(
    -DCMAKE_BUILD_TYPE=Release
    -DGO2CPP_BUILD_TESTS=ON
    -DGO2CPP_BUILD_EXAMPLES=ON
    -DGO2CPP_WITH_BENTO=ON
    -DGO2CPP_WITH_MCP=ON
    -DMCP_BUILD_TESTS=ON
    -DGO2CPP_WITH_APP=ON
    -DGO2CPP_WITH_P2P=ON
    -DGO2CPP_WITH_SQLITE=ON
    -DGO2CPP_BUILD_OS_MONITOR_TESTS=OFF
)

generator_args=()
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

printf '[ci] repo=%s\n' "$ROOT"
printf '[ci] mode=%s build_dir=%s jobs=%s\n' "$MODE" "$BUILD_DIR" "$JOBS"

# ---------------------------------------------------------------- configure
if [ "$DO_CONFIGURE" -eq 1 ]; then
    step_begin "configure"
    if ! cmake -S "$ROOT" -B "$BUILD_DIR" "${generator_args[@]}" "${configure_args[@]}" \
            2>&1 | tee "$LOG_DIR/configure.log"; then
        step_fail
    fi
    step_ok
else
    printf '[ci] configure skipped (--no-configure)\n'
fi

# -------------------------------------------------------------------- build
step_begin "build"
if ! cmake --build "$BUILD_DIR" --parallel "$JOBS" 2>&1 | tee "$LOG_DIR/build.log"; then
    step_fail
fi
step_ok

# -------------------------------------------------------------------- ctest
step_begin "ctest"
# 已知的环境相关失败（WSL 内核语义 / 线程数假设 / 文档数字漂移）。
# 默认严格（不排除）；CI 里设 CI_ALLOW_KNOWN=1 时排除这 3 项，避免红得没有信息量。
CTEST_EXCLUDE=()
if [ "${CI_ALLOW_KNOWN:-0}" = "1" ]; then
    CTEST_EXCLUDE=(-E "io_hook|go2cpp_tests|docs_consistency")
    printf '[ci] allow-known: 排除 io_hook / go2cpp_tests / docs_consistency（已知环境相关）\n'
fi
CTEST_LOG="$LOG_DIR/ctest.log"
CTEST_JUNIT="$LOG_DIR/ctest-junit.xml"
rm -f "$CTEST_JUNIT"
set +e
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 600 \
      "${CTEST_EXCLUDE[@]}" \
      --output-junit "$CTEST_JUNIT" 2>&1 | tee "$CTEST_LOG"
CTEST_RC=${PIPESTATUS[0]}
set -e
parse_ctest "$CTEST_JUNIT" "$CTEST_LOG"
if [ "$CTEST_RC" -ne 0 ]; then
    printf '[ci] ctest summary: passed=%s failed=%s skipped=%s\n' \
        "$CTEST_PASSED" "$CTEST_FAILED" "$CTEST_SKIPPED" >&2
    step_fail
fi
step_ok
printf '[ci] ctest summary: passed=%s failed=%s skipped=%s\n' \
    "$CTEST_PASSED" "$CTEST_FAILED" "$CTEST_SKIPPED"

# --------------------------------------------------------------------- full
if [ "$MODE" = "full" ]; then
    ASAN_DIR="${BUILD_DIR}-asan"
    # 生成器按 ASAN_DIR 自身缓存判断：主目录与 ASAN 目录可能残留不同生成器的缓存。
    asan_gen_args=()
    if [ ! -f "$ASAN_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then
        asan_gen_args=(-G Ninja)
    fi
    step_begin "asan-configure"
    asan_args=()
    if has_asan_option; then
        asan_args+=(-DGO2CPP_ENABLE_ASAN=ON)
    else
        asan_args+=(
            "-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
            "-DCMAKE_C_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address"
            "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address"
        )
    fi
    if ! cmake -S "$ROOT" -B "$ASAN_DIR" "${asan_gen_args[@]}" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DGO2CPP_BUILD_TESTS=ON \
            -DGO2CPP_BUILD_EXAMPLES=OFF \
            -DGO2CPP_WITH_BENTO=ON \
            -DGO2CPP_WITH_MCP=OFF \
            -DGO2CPP_WITH_APP=OFF \
            -DGO2CPP_WITH_P2P=OFF \
            -DGO2CPP_WITH_SQLITE=ON \
            -DGO2CPP_BUILD_OS_MONITOR_TESTS=OFF \
            "${asan_args[@]}" 2>&1 | tee "$LOG_DIR/asan-configure.log"; then
        step_fail
    fi
    step_ok

    step_begin "asan-build"
    if ! cmake --build "$ASAN_DIR" --parallel "$JOBS" 2>&1 | tee "$LOG_DIR/asan-build.log"; then
        step_fail
    fi
    step_ok

    step_begin "asan-ctest"
    ASAN_REGEX='go2cpp_(scheduler|channel|fiber|parking|concurrency|select|sync|net|socket|http2|timer|hook|os|io)'
    ASAN_LOG="$LOG_DIR/asan-ctest.log"
    ASAN_JUNIT="$LOG_DIR/asan-ctest-junit.xml"
    rm -f "$ASAN_JUNIT"
    set +e
    ctest --test-dir "$ASAN_DIR" -R "$ASAN_REGEX" --output-on-failure --timeout 600 \
          --output-junit "$ASAN_JUNIT" 2>&1 | tee "$ASAN_LOG"
    ASAN_RC=${PIPESTATUS[0]}
    set -e
    parse_ctest "$ASAN_JUNIT" "$ASAN_LOG"
    if [ "$ASAN_RC" -ne 0 ]; then
        printf '[ci] asan-ctest summary: passed=%s failed=%s skipped=%s\n' \
            "$CTEST_PASSED" "$CTEST_FAILED" "$CTEST_SKIPPED" >&2
        step_fail
    fi
    step_ok
    printf '[ci] asan-ctest summary: passed=%s failed=%s skipped=%s\n' \
        "$CTEST_PASSED" "$CTEST_FAILED" "$CTEST_SKIPPED"

    step_begin "verify-metrics"
    # verify_metrics.sh 硬编码 $ROOT/.build-bento-wsl；不存在时临时链接到当前构建目录。
    if [ ! -e "$ROOT/.build-bento-wsl" ] && [ "$BUILD_DIR" != "$ROOT/.build-bento-wsl" ]; then
        ln -s "$BUILD_DIR" "$ROOT/.build-bento-wsl"
        VERIFY_LINK_CREATED=1
    fi
    if ! bash "$ROOT/tools/verify_metrics.sh" --quick 2>&1 | tee "$LOG_DIR/verify-metrics.log"; then
        step_fail
    fi
    step_ok
fi

TOTAL=$(( $(date +%s) - START_ALL ))
printf '[ci] SUMMARY mode=%s build_dir=%s total=%s ctest_passed=%s ctest_failed=%s ctest_skipped=%s\n' \
    "$MODE" "$BUILD_DIR" "$(fmt_duration "$TOTAL")" \
    "$CTEST_PASSED" "$CTEST_FAILED" "$CTEST_SKIPPED"
