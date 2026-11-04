#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# check_infra_usage.sh —— 检查 src/ 中绕过统一基础设施的写法。
#
# 规则（见 docs/modules/infra-usage.md）：
#   - 日志必须走 go2cpp::logging：禁止 printf / std::cout / getenv 里的输出式用法；
#   - 配置必须走 go2cpp::config：禁止散落 getenv；
#   - 错误必须走 go2cpp::error：本脚本只检查前两类（返回值语义无法用文本匹配）。
#
# 排除：
#   - src/logging/ 与 src/config/ 是入口自身的实现；
#   - 纯注释行（//、/*、* 开头）不计违规，避免文档说明被误报。
#
# 用法：
#   scripts/check_infra_usage.sh            # 输出违规文件列表
#   scripts/check_infra_usage.sh --verbose  # 附带 file:line 明细
#
# 退出码：0 = 没有违规；1 = 存在违规；2 = 运行环境异常（不在仓库根等）。
# ---------------------------------------------------------------------------
set -uo pipefail

verbose=0
if [[ "${1:-}" == "--verbose" || "${1:-}" == "-v" ]]; then
    verbose=1
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "${script_dir}/.." && pwd)
if [[ ! -d "${repo_root}/src" ]]; then
    echo "check_infra_usage: 找不到 ${repo_root}/src，请在仓库内运行" >&2
    exit 2
fi
cd "${repo_root}" || exit 2

# grep 的命中行格式固定为 path:line:content；用 read 拆分，避免再嵌套 grep。
declare -A file_counts=()
declare -a details=()
violation_total=0

while IFS= read -r hit; do
    [[ -z "${hit}" ]] && continue
    path=${hit%%:*}
    rest=${hit#*:}
    lineno=${rest%%:*}
    content=${rest#*:}

    # 入口自身实现不检查。
    case "${path}" in
        src/logging/*|src/config/*) continue ;;
    esac

    # 跳过纯注释行（行首空白后是 //、/* 或 *）。
    trimmed=${content#"${content%%[![:space:]]*}"}
    case "${trimmed}" in
        '//'*|'/*'*|'*'*) continue ;;
    esac

    violation_total=$((violation_total + 1))
    file_counts["${path}"]=$(( ${file_counts["${path}"]:-0} + 1 ))
    details+=("${path}:${lineno}:${trimmed}")
done < <(grep -rnE '\bprintf\b|\bstd::cout\b|\bgetenv\b' src/ 2>/dev/null || true)

if (( violation_total == 0 )); then
    echo "check_infra_usage: OK，未发现绕过 go2cpp::logging / go2cpp::config 的写法"
    exit 0
fi

echo "check_infra_usage: 发现 ${violation_total} 处疑似违规，涉及 ${#file_counts[@]} 个文件"
for path in "${!file_counts[@]}"; do
    printf '%4d  %s\n' "${file_counts[${path}]}" "${path}"
done | sort -rn

if (( verbose == 1 )); then
    echo
    echo "明细："
    printf '%s\n' "${details[@]}" | sort
fi

exit 1
