#!/usr/bin/env bash
# =============================================================================
# scripts/check_build_freshness.sh —— 构建产物与源码的一致性门禁。
#
# 背景：2026-11-04 两次事故（13 个假 SEGFAULT、Parser 假复现）的根因都是
#       「旧 exe + 新 so」不同批。本脚本把「记得先重建」变成机器校验：
#       源码比产物新、或 exe 落后于它自己 NEEDED 的 .so，一律判定需要重建。
#
# 用法：
#   bash scripts/check_build_freshness.sh                      # 默认 <repo>/.build-bento-wsl
#   bash scripts/check_build_freshness.sh --build-dir DIR      # 指定构建目录
#   bash scripts/check_build_freshness.sh --repo DIR           # 指定源码树（默认脚本上一级）
#   bash scripts/check_build_freshness.sh --quiet              # 通过时只打印一行
#   bash scripts/check_build_freshness.sh --json               # 机器可读输出
#   bash scripts/check_build_freshness.sh --no-order-check     # 关闭 exe/so 一致性检查
#   bash scripts/check_build_freshness.sh --no-deps-filter     # 不按 .d 过滤头文件
#   bash scripts/check_build_freshness.sh --list-only          # 只列清单，始终退出 0
#
# 判定：
#   R0 构建目录/关键产物缺失                 → 需要重建（exit 1）
#   R1 最新源码 mtime > 最新关键产物 mtime   → 需要重建（exit 1）
#   R2 可执行文件比它 NEEDED 的某个 .so 旧   → 需要重建（exit 1，exe/so 不同批）
#   关键产物 = go2cpp_tests（可执行）+ libgo2cpp_*.so*（共享库）。
#
# 头文件口径：src/**/*.cpp 全部参与比较；include/go2cpp/**/*.hpp 只比较
#   构建目录 *.d 依赖文件里出现过的（即真的被某个编译单元消费的）。否则
#   聚合头（bytes.hpp / sync.hpp 等未被内部包含的 public 入口）被 touch 后
#   会永久误报「需要重建」，而重建根本无法修复。没有 *.d 时退回全部头文件。
#
# 退出码：0 = 新鲜，可以跑测试/链接；1 = 需要重建；2 = 用法或环境错误。
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd -P)"
REPO="$(cd -- "$SCRIPT_DIR/.." >/dev/null 2>&1 && pwd -P)"
BUILD_DIR=""
QUIET=0
JSON=0
ORDER_CHECK=1
DEPS_FILTER=1
LIST_ONLY=0
MAX_STALE_LIST=20
CONSUMED_HPP_FILE=""
# 注意：EXIT trap 的最后一条命令若成功，会覆盖脚本原本的退出码（bash 行为），
# 门禁会永远 exit 0。这里显式保存/恢复，并在退出前解除 trap。
cleanup_tmp() {
    local rc=$?
    trap - EXIT
    [ -n "$CONSUMED_HPP_FILE" ] && rm -f "$CONSUMED_HPP_FILE"
    exit "$rc"
}
trap cleanup_tmp EXIT

usage() {
    awk 'NR>2 { if ($0 ~ /^# ====/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)
            [ $# -ge 2 ] || { printf '错误: --build-dir 需要参数\n' >&2; exit 2; }
            BUILD_DIR="$2"; shift ;;
        --build-dir=*) BUILD_DIR="${1#*=}" ;;
        --repo)
            [ $# -ge 2 ] || { printf '错误: --repo 需要参数\n' >&2; exit 2; }
            REPO="$2"; shift ;;
        --repo=*) REPO="${1#*=}" ;;
        --quiet|-q) QUIET=1 ;;
        --json) JSON=1; QUIET=1 ;;
        --no-order-check) ORDER_CHECK=0 ;;
        --no-deps-filter) DEPS_FILTER=0 ;;
        --list-only) LIST_ONLY=1 ;;
        -h|--help) usage; exit 0 ;;
        *) printf '错误: 未知参数 %s（用 --help 看用法）\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

REPO="$(cd -- "$REPO" >/dev/null 2>&1 && pwd -P)" || {
    printf '错误: 源码树不存在\n' >&2; exit 2; }
if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO/.build-bento-wsl"
fi
# 相对路径按源码树解析，避免调用方 cwd 不同导致误判。
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$REPO/$BUILD_DIR" ;;
esac

if [ ! -d "$REPO/include/go2cpp" ] || [ ! -d "$REPO/src" ]; then
    printf '错误: %s 不是 Go2Cpp 源码树（缺 include/go2cpp 或 src）\n' "$REPO" >&2
    exit 2
fi

# ts_to_human <epoch-seconds> → "YYYY-MM-DD HH:MM:SS"
ts_to_human() {
    local ts="$1"
    if [ -z "$ts" ] || [ "$ts" = "0" ]; then
        printf '(无)'
        return
    fi
    date -d "@$ts" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || printf '%s' "$ts"
}

# ts_int <float> → 整数秒（去掉小数部分）
ts_int() { printf '%s' "${1%%.*}"; }

# 收集「被构建系统消费过的头文件」：编译器依赖文件 *.d 里出现过的头文件。
# 聚合头（public 入口）不被内部 TU 包含，touch 它们不会产生任何新产物；
# 若一并参与比较，门禁会永久报「需要重建」且重建无法修复。
if [ "$DEPS_FILTER" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    CONSUMED_HPP_FILE="$(mktemp)"
    find "$BUILD_DIR" -name '*.d' -type f -print0 2>/dev/null |
        xargs -0 -r grep -hoE '[^ :\\]+/include/go2cpp/[^ :\\]+\.hpp' 2>/dev/null |
        sort -u > "$CONSUMED_HPP_FILE" || true
fi

# 收集源文件：输出 "<mtime> <path>"，按 mtime 升序。
# .cpp 全部参与；.hpp 在 --no-deps-filter 或没有 *.d 时全部参与。
SOURCE_LIST="$(find "$REPO/include/go2cpp" "$REPO/src" -type f \
    \( -name '*.hpp' -o -name '*.cpp' \) -printf '%T@ %p\n' 2>/dev/null |
    awk -v list="$CONSUMED_HPP_FILE" '
        BEGIN {
            if (list != "") {
                while ((getline l < list) > 0) ok[l] = 1
                close(list)
            }
        }
        /\.cpp$/ { print; next }
        { if (list == "" || ($2 in ok)) print }
    ' | sort -k1,1n)"
SOURCE_COUNT="$(printf '%s\n' "$SOURCE_LIST" | grep -c . || true)"
if [ -z "$SOURCE_LIST" ] || [ "$SOURCE_COUNT" -eq 0 ]; then
    printf '错误: 在 %s 下没找到任何 *.hpp / *.cpp\n' "$REPO" >&2
    exit 2
fi
SOURCE_NEWEST_LINE="$(printf '%s\n' "$SOURCE_LIST" | tail -1)"
SOURCE_NEWEST_TS="${SOURCE_NEWEST_LINE%% *}"
SOURCE_NEWEST_PATH="${SOURCE_NEWEST_LINE#* }"
HPP_COUNT="$(printf '%s\n' "$SOURCE_LIST" | grep -c '\.hpp$' || true)"
CPP_COUNT="$(printf '%s\n' "$SOURCE_LIST" | grep -c '\.cpp$' || true)"
HPP_TOTAL="$(find "$REPO/include/go2cpp" -name '*.hpp' -type f | wc -l)"

# 收集关键产物：可执行 go2cpp_tests + libgo2cpp_*.so*（含版本号后缀）。
ARTIFACT_DIR_OK=0
[ -d "$BUILD_DIR" ] && ARTIFACT_DIR_OK=1
EXE_LIST=""
LIB_LIST=""
if [ "$ARTIFACT_DIR_OK" -eq 1 ]; then
    EXE_LIST="$(find "$BUILD_DIR" -maxdepth 4 -type f -name 'go2cpp_tests' -printf '%T@ %p\n' 2>/dev/null | sort -k1,1n)"
    # 关键产物同时收集 .so 与 .a：bento/distributed 等是静态库，
    # 只查 .so 会把「静态库产物已更新」误判为 STALE。
    LIB_LIST="$(find "$BUILD_DIR" -maxdepth 4 -type f \( -name 'libgo2cpp_*.so*' -o -name 'libgo2cpp_*.a' \) -printf '%T@ %p\n' 2>/dev/null | sort -k1,1n)"
fi
ARTIFACT_LIST="$(printf '%s\n%s\n' "$EXE_LIST" "$LIB_LIST" | grep -c . || true)"
ARTIFACT_NEWEST_TS=""
ARTIFACT_NEWEST_PATH=""
ARTIFACT_OLDEST_TS=""
if [ -n "$EXE_LIST$LIB_LIST" ]; then
    ARTIFACT_NEWEST_LINE="$(printf '%s\n%s\n' "$EXE_LIST" "$LIB_LIST" | grep . | sort -k1,1n | tail -1)"
    ARTIFACT_NEWEST_TS="${ARTIFACT_NEWEST_LINE%% *}"
    ARTIFACT_NEWEST_PATH="${ARTIFACT_NEWEST_LINE#* }"
    ARTIFACT_OLDEST_TS="$(printf '%s\n%s\n' "$EXE_LIST" "$LIB_LIST" | grep . | sort -k1,1n | head -1 | cut -d' ' -f1)"
fi
EXE_NEWEST_TS=""
if [ -n "$EXE_LIST" ]; then
    EXE_NEWEST_TS="$(printf '%s\n' "$EXE_LIST" | tail -1 | cut -d' ' -f1)"
fi

REASONS=()
STALE_LINES=()

# R0：构建目录或关键产物缺失。
if [ "$ARTIFACT_DIR_OK" -eq 0 ]; then
    REASONS+=("build-dir-missing")
elif [ -z "$EXE_LIST" ] || [ -z "$LIB_LIST" ]; then
    REASONS+=("artifact-missing")
fi

# R1：最新源码比最新关键产物新 → 一定有代码没被编进产物。
if [ "$ARTIFACT_DIR_OK" -eq 1 ] && [ -n "$ARTIFACT_NEWEST_TS" ] &&
   awk -v a="$SOURCE_NEWEST_TS" -v b="$ARTIFACT_NEWEST_TS" 'BEGIN{exit !(a>b)}'; then
    REASONS+=("source-newer-than-artifact")
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        fts="${line%% *}"; fpath="${line#* }"
        if awk -v a="$fts" -v b="$ARTIFACT_NEWEST_TS" 'BEGIN{exit !(a>b)}'; then
            STALE_LINES+=("$(printf '%s\t%s' "$fpath" \
                "source $(ts_to_human "$(ts_int "$fts")") +$(( $(ts_int "$fts") - $(ts_int "$ARTIFACT_NEWEST_TS") ))s")")
        fi
    done <<< "$(printf '%s\n' "$SOURCE_LIST" | sort -k1,1nr)"
fi

# R2：可执行文件落后于它 NEEDED 的 .so → 典型的「旧 exe + 新 so」。
# 只检查 exe 实际依赖的库（readelf -d 的 NEEDED），不要求无关库与 exe 同步，
# 否则「只重建了某模块 .so」会造成永远修不好的假阳性。
if [ "$ORDER_CHECK" -eq 1 ] && [ -n "$EXE_LIST" ] && [ -n "$LIB_LIST" ]; then
    while IFS= read -r exe_line; do
        [ -n "$exe_line" ] || continue
        exe_ts="${exe_line%% *}"; exe_path="${exe_line#* }"
        needed=""
        if command -v readelf >/dev/null 2>&1; then
            needed="$(readelf -d "$exe_path" 2>/dev/null |
                sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
        elif command -v objdump >/dev/null 2>&1; then
            needed="$(objdump -p "$exe_path" 2>/dev/null |
                sed -n 's/^[[:space:]]*NEEDED[[:space:]]\{1,\}\(.*\)$/\1/p')"
        fi
        while IFS= read -r so_name; do
            case "$so_name" in libgo2cpp_*.so*) ;; *) continue ;; esac
            so_line="$(printf '%s\n' "$LIB_LIST" | awk -v n="/$so_name" 'index($2, n) == length($2) - length(n) + 1' | tail -1)"
            [ -n "$so_line" ] || continue
            so_ts="${so_line%% *}"; so_path="${so_line#* }"
            if awk -v a="$so_ts" -v b="$exe_ts" 'BEGIN{exit !(a>(b+1))}'; then
                STALE_LINES+=("$(printf '%s\t%s' "$exe_path" \
                    "exe/so $(ts_to_human "$(ts_int "$so_ts")") 落后于 $so_path +$(( $(ts_int "$so_ts") - $(ts_int "$exe_ts") ))s")")
            fi
        done <<< "$needed"
    done <<< "$EXE_LIST"
    if [ "${#STALE_LINES[@]}" -gt 0 ] && [ "${REASONS[*]}" != *"exe-older-than-needed-so"* ]; then
        for item in "${STALE_LINES[@]}"; do
            case "$item" in *"exe/so "*)
                REASONS+=("exe-older-than-needed-so"); break ;;
            esac
        done
    fi
fi

FRESH=1
[ "${#REASONS[@]}" -gt 0 ] && FRESH=0

if [ "$JSON" -eq 1 ]; then
    printf '{"fresh":%s,"repo":"%s","build_dir":"%s","sources":{"count":%s,"hpp":%s,"hpp_total":%s,"cpp":%s,"newest":"%s","newest_ts":"%s"},"artifacts":{"count":%s,"exe":%s,"so":%s,"newest_ts":"%s","oldest_ts":"%s"},"reasons":[' \
        "$([ "$FRESH" -eq 1 ] && echo true || echo false)" "$REPO" "$BUILD_DIR" \
        "$SOURCE_COUNT" "$HPP_COUNT" "$HPP_TOTAL" "$CPP_COUNT" "$SOURCE_NEWEST_PATH" "$SOURCE_NEWEST_TS" \
        "$ARTIFACT_LIST" "$(printf '%s\n' "$EXE_LIST" | grep -c . || true)" \
        "$(printf '%s\n' "$LIB_LIST" | grep -c . || true)" "$ARTIFACT_NEWEST_TS" "$ARTIFACT_OLDEST_TS"
    sep=""
    for r in "${REASONS[@]}"; do printf '%s"%s"' "$sep" "$r"; sep=","; done
    printf ']}\n'
    [ "$LIST_ONLY" -eq 1 ] && exit 0
    [ "$FRESH" -eq 1 ] && exit 0 || exit 1
fi

if [ "$QUIET" -eq 0 ] || [ "$FRESH" -eq 0 ]; then
    printf '[freshness] repo=%s\n' "$REPO"
    printf '[freshness] build_dir=%s\n' "$BUILD_DIR"
    printf '[freshness] sources=%s (hpp=%s/%s consumed, cpp=%s) newest=%s @%s\n' \
        "$SOURCE_COUNT" "$HPP_COUNT" "$HPP_TOTAL" "$CPP_COUNT" "$SOURCE_NEWEST_PATH" \
        "$(ts_to_human "$(ts_int "$SOURCE_NEWEST_TS")")"
    if [ "$ARTIFACT_DIR_OK" -eq 0 ]; then
        printf '[freshness] artifacts=0（构建目录不存在）\n'
    else
        printf '[freshness] artifacts=%s (exe=%s so=%s) newest=%s @%s\n' \
            "$ARTIFACT_LIST" "$(printf '%s\n' "$EXE_LIST" | grep -c . || true)" \
            "$(printf '%s\n' "$LIB_LIST" | grep -c . || true)" \
            "${ARTIFACT_NEWEST_PATH:-(无)}" "$(ts_to_human "$(ts_int "${ARTIFACT_NEWEST_TS:-0}")")"
    fi
fi

if [ "$FRESH" -eq 1 ]; then
    [ "$QUIET" -eq 0 ] && printf '[freshness] RESULT: FRESH\n'
    exit 0
fi

printf '[freshness] RESULT: STALE —— 需要重建\n'
printf '需要重建（源码/产物不一致）：\n'
i=0
for line in "${STALE_LINES[@]}"; do
    i=$((i + 1))
    [ "$i" -le "$MAX_STALE_LIST" ] || break
    printf '  [%s]\n' "$(printf '%s' "$line" | sed 's/\t/  /g')"
done
total="${#STALE_LINES[@]}"
if [ "$total" -gt "$MAX_STALE_LIST" ]; then
    printf '  ... 另有 %s 条，详见 --json 输出\n' "$((total - MAX_STALE_LIST))"
fi
printf '提示：bash scripts/ci.sh --quick（或 cmake --build %s --parallel %s）后重试\n' \
    "$BUILD_DIR" "$(nproc 2>/dev/null || echo 4)"
[ "$LIST_ONLY" -eq 1 ] && exit 0
exit 1
