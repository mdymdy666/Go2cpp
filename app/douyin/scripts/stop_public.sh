#!/usr/bin/env bash
# 停止 serve_public.sh 启动的进程（按 pid 精确 kill，先校验 /proc/<pid>/exe）。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID_FILE="$ROOT/logs/serve_public.pids"
[[ -f "$PID_FILE" ]] || { echo "[stop] 无 pid 文件"; exit 0; }
while read -r name pid port; do
  [[ -z "$pid" || ! -d "/proc/$pid" ]] && continue
  exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
  case "$exe" in
    *"/build/"*|*"/tunnel-client"*) kill "$pid" 2>/dev/null && echo "[stop] $name pid=$pid 已停止" ;;
    *) echo "[stop] 跳过 $name pid=$pid（exe=$exe 不匹配）" ;;
  esac
done <"$PID_FILE"
rm -f "$PID_FILE"
