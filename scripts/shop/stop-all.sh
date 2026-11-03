#!/usr/bin/env bash
# /tmp/shop 统一停止：只用 pkill -x 精确名（禁止 pkill -f），并复核端口已释放。
set -uo pipefail

ROOT="${ROOT:-/tmp/shop}"
NAMES=(gateway product_service review_service order_service tunnel-client)
PORTS=(8080 9101 9102 9103)

say() { printf '[stop] %s\n' "$*"; }

for n in "${NAMES[@]}"; do
  if pgrep -x "$n" >/dev/null 2>&1; then
    pkill -x "$n" 2>/dev/null
    for _ in $(seq 1 50); do
      pgrep -x "$n" >/dev/null 2>&1 || break
      sleep 0.1
    done
    if pgrep -x "$n" >/dev/null 2>&1; then
      say "$n 5 秒内未退出，发送 KILL"
      pkill -9 -x "$n" 2>/dev/null
      sleep 0.3
    fi
    say "$n 已停止"
  else
    say "$n 未运行"
  fi
done

rm -f "$ROOT"/data/*.pid 2>/dev/null

echo '--- 端口复核 ---'
bad=0
for p in "${PORTS[@]}"; do
  if ss -ltn 2>/dev/null | grep -q ":$p "; then
    say "端口 $p 仍被占用"
    bad=1
  else
    say "端口 $p 已释放"
  fi
done

if ((bad != 0)); then
  ss -ltnp 2>/dev/null | grep -E ':(8080|9101|9102|9103) ' || true
  exit 1
fi
say '全部已停止'
exit 0
