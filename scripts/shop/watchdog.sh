#!/usr/bin/env bash
# /tmp/shop tunnel-client 运维兜底（watchdog）。
#
# 定位：**运维兜底，不是自愈**。根因（服务端 GOAWAY 后 client 退出重连循环）
# 已在 src/tunnel/client.cpp RunLoop 修掉；本脚本只处理「client 进程整个消失
# 或健康检查持续失败」这类 client 自身无法处理的场景（例如被 pkill、OOM）。
#
# 每 WATCHDOG_INTERVAL 秒（默认 10）：
#   1) pgrep -x tunnel-client 为空 → 按 start-all.sh 的方式 setsid nohup 拉起；
#   2) 公网 health 连续 2 次非 200 → SIGTERM 该 pid，2 秒后仍在则 SIGKILL，再拉起。
# 只按精确 pid 结束进程，不用 pkill -f；单实例由 flock 保证。
set -uo pipefail

ROOT="${ROOT:-/tmp/shop}"
LOGDIR="${ROOT}/logs"
PIDDIR="${ROOT}/data"
TUNNEL_BIN="${TUNNEL_BIN:-/tmp/build-tunnel/tunnel-client}"
TUNNEL_SERVER="${TUNNEL_SERVER:-82.157.147.127:7443}"
TUNNEL_SUBDOMAIN="${TUNNEL_SUBDOMAIN:-shop}"
TUNNEL_TOKEN="${TUNNEL_TOKEN:-MyShop2026}"
PUBLIC_URL="${PUBLIC_URL:-http://82.157.147.127:8443/shop/api/health}"
INTERVAL="${WATCHDOG_INTERVAL:-10}"

mkdir -p "$LOGDIR" "$PIDDIR" || exit 1

log() { printf '[watchdog %s] %s\n' "$(date '+%F %T')" "$*"; }

# 单实例：拿不到锁说明已有 watchdog 在跑。
exec 9>"$PIDDIR/watchdog.lock"
if ! flock -n 9; then
  log "已有 watchdog 在运行，退出"
  exit 1
fi
printf '%s\n' "$$" >"$PIDDIR/watchdog.pid"
log "启动 pid=$$ interval=${INTERVAL}s target=$PUBLIC_URL"

start_client() {
  if [[ ! -x "$TUNNEL_BIN" ]]; then
    log "二进制不存在或不可执行：$TUNNEL_BIN"
    return 1
  fi
  setsid nohup "$TUNNEL_BIN" \
    --server "$TUNNEL_SERVER" --local "${SHOP_PORT:-8080}" \
    --subdomain "$TUNNEL_SUBDOMAIN" --token "$TUNNEL_TOKEN" \
    >>"$LOGDIR/tunnel-client.log" 2>&1 </dev/null &
  local pid=$!
  printf '%s\n' "$pid" >"$PIDDIR/tunnel-client.pid"
  log "tunnel-client 已拉起 pid=$pid"
}

stop_pid() {
  local pid="$1" i
  [[ -n "$pid" ]] || return 0
  kill -TERM "$pid" 2>/dev/null || true
  for ((i = 0; i < 20; i++)); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.1
  done
  kill -KILL "$pid" 2>/dev/null || true
  log "pid=$pid 未响应 SIGTERM，已 SIGKILL"
}

fails=0
while true; do
  pid="$(pgrep -x tunnel-client 2>/dev/null | head -n1)"
  if [[ -z "$pid" ]]; then
    log "tunnel-client 不存在，拉起"
    start_client
  else
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$PUBLIC_URL" 2>/dev/null)"
    [[ -n "$code" ]] || code="000"
    if [[ "$code" == "200" ]]; then
      fails=0
    else
      fails=$((fails + 1))
      log "公网 health=$code（第 $fails 次），client pid=$pid"
      if ((fails >= 2)); then
        stop_pid "$pid"
        start_client
        fails=0
      fi
    fi
  fi
  sleep "$INTERVAL"
done
