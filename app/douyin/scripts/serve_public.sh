#!/usr/bin/env bash
# 抖音 demo 公网部署：启动 7 个进程 + 隧道 client（subdomain=douyin）。
# 停止：scripts/stop_public.sh（按 pid 精确 kill，不用 pkill -f）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
DB="${DOUYIN_DB:-$ROOT/data/douyin.db}"
PID_FILE="$LOG_DIR/serve_public.pids"
TUNNEL_BIN="${TUNNEL_BIN:-/tmp/build-tunnel/tunnel-client}"
TUNNEL_SERVER="${TUNNEL_SERVER:-82.157.147.127:7443}"
TUNNEL_SUBDOMAIN="${TUNNEL_SUBDOMAIN:-douyin}"
TUNNEL_TOKEN="${TUNNEL_TOKEN:-MyShop2026}"
GW_PORT="${GW_PORT:-28080}"

mkdir -p "$LOG_DIR" "$(dirname "$DB")"
: >"$PID_FILE"

say() { printf '[serve] %s\n' "$*"; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    say "找不到 $bin，请先跑 build.sh"; exit 1
  fi
  DOUYIN_DB="$DB" setsid nohup "$bin" "$port" >>"$LOG_DIR/serve_${name}.log" 2>&1 < /dev/null &
  local pid=$!
  echo "$name $pid $port" >>"$PID_FILE"
  local ready=0
  for _ in $(seq 1 200); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then ready=1; break; fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    say "$name 启动失败，日志尾部："; tail -5 "$LOG_DIR/serve_${name}.log"; exit 1
  fi
  say "$name :$port 就绪 pid=$pid"
}

say "启动 6 业务 + gateway（串行等 health，避开 WAL 建表竞态）"
start_service user_service 29101
start_service video_service 29102
start_service feed_service 29103
start_service interaction_service 29104
start_service live_service 29105
start_service search_service 29106
start_service gateway "$GW_PORT"

say "本机健康检查"
for p in 29101 29102 29103 29104 29105 29106; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$p/health")
  say "  :$p /health = $code"
done
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$GW_PORT/api/health/all")
say "  :$GW_PORT /api/health/all = $code"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$GW_PORT/api/feed?limit=3")
say "  :$GW_PORT /api/feed = $code"

if [[ ! -x "$TUNNEL_BIN" ]]; then
  say "隧道二进制不存在：$TUNNEL_BIN（公网不可用，本机仍可访问）"
else
  say "启动隧道 client（subdomain=$TUNNEL_SUBDOMAIN → 127.0.0.1:$GW_PORT）"
  setsid nohup "$TUNNEL_BIN" --server "$TUNNEL_SERVER" --local "$GW_PORT" \
      --subdomain "$TUNNEL_SUBDOMAIN" --token "$TUNNEL_TOKEN" \
      >>"$LOG_DIR/serve_tunnel.log" 2>&1 < /dev/null &
  echo "tunnel $! 0" >>"$PID_FILE"
  sleep 4
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 \
      "http://82.157.147.127:8443/$TUNNEL_SUBDOMAIN/api/feed?limit=3")
  say "公网 /$TUNNEL_SUBDOMAIN/api/feed = $code"
  say "公网地址：http://82.157.147.127:8443/$TUNNEL_SUBDOMAIN/"
fi

say "pid 文件：$PID_FILE"
