#!/usr/bin/env bash
# /tmp/shop 外部探针：每 60 秒探测公网健康检查。
#
#   ./probe.sh               无限循环，每 60 秒一次
#   ./probe.sh --once        只探一次（200 退出 0，非 200 退出 1）
#   ./probe.sh --minutes N   跑 N 次（约 N 分钟）后退出
#
# 非 200 时向 logs/probe.log 追加一行：时间戳 + code + 连续失败数；
# 200 时连续失败数清零。stdout 每次都会打一行结果。
set -uo pipefail

ROOT="${ROOT:-/tmp/shop}"
URL="${PROBE_URL:-http://82.157.147.127:8443/shop/api/health}"
LOG="${PROBE_LOG:-${ROOT}/logs/probe.log}"
INTERVAL="${PROBE_INTERVAL:-60}"

ONCE=0
COUNT=0 # 0 = 无限循环

while [[ $# -gt 0 ]]; do
  case "$1" in
    --once) ONCE=1; shift ;;
    --minutes) COUNT="${2:-0}"; shift 2 || true ;;
    --interval) INTERVAL="${2:-60}"; shift 2 || true ;;
    -h | --help)
      echo "用法: $0 [--once] [--minutes N] [--interval 秒]"
      exit 0
      ;;
    *)
      printf '未知参数: %s\n' "$1" >&2
      exit 2
      ;;
  esac
done

mkdir -p "$(dirname "$LOG")"
fail_streak=0

probe() {
  local code ts
  ts="$(date '+%Y-%m-%d %H:%M:%S')"
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$URL" 2>/dev/null)"
  [[ -n "$code" ]] || code="000"
  if [[ "$code" == "200" ]]; then
    fail_streak=0
    printf '%s code=200 OK streak=0\n' "$ts"
    return 0
  fi
  fail_streak=$((fail_streak + 1))
  printf '%s code=%s FAIL streak=%d\n' "$ts" "$code" "$fail_streak"
  printf '%s code=%s streak=%d url=%s\n' "$ts" "$code" "$fail_streak" "$URL" >>"$LOG"
  return 1
}

if ((ONCE)); then
  probe
  exit $?
fi

i=0
while :; do
  i=$((i + 1))
  probe || true
  if ((COUNT > 0 && i >= COUNT)); then
    break
  fi
  sleep "$INTERVAL"
done
exit 0
