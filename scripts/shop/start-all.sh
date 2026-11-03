#!/usr/bin/env bash
# /tmp/shop 统一启动（幂等）。
#
# 顺序：product_service → review_service → order_service（等端口）→ gateway
#       → tunnel-client（等 2 秒后查公网）→ 六项健康检查。
# 幂等：已在跑的进程不重复启动；端口被外部进程占用则报错。
# 停止：./stop-all.sh（只用 pkill -x 精确名）。
set -uo pipefail

ROOT="${ROOT:-/tmp/shop}"
SHOP_PORT="${SHOP_PORT:-8080}"
PRODUCT_PORT="${PRODUCT_PORT:-9101}"
REVIEW_PORT="${REVIEW_PORT:-9102}"
ORDER_PORT="${ORDER_PORT:-9103}"

TUNNEL_BIN="${TUNNEL_BIN:-/tmp/build-tunnel/tunnel-client}"
TUNNEL_SERVER="${TUNNEL_SERVER:-82.157.147.127:7443}"
TUNNEL_SUBDOMAIN="${TUNNEL_SUBDOMAIN:-shop}"
TUNNEL_TOKEN="${TUNNEL_TOKEN:-MyShop2026}"
PUBLIC_URL="${PUBLIC_URL:-http://82.157.147.127:8443/shop/api/health}"

BASE="http://127.0.0.1:${SHOP_PORT}"
LOGDIR="${ROOT}/logs"
PIDDIR="${ROOT}/data"

mkdir -p "$LOGDIR" "$PIDDIR"
cd "$ROOT" || exit 1

FAILED=0
say() { printf '[start] %s\n' "$*"; }
warn() { printf '[start] %s\n' "$*" >&2; }

port_listening() { ss -ltn 2>/dev/null | grep -q ":$1 "; }
pid_of() { pgrep -x "$1" 2>/dev/null | head -n1; }

# start_svc <name> <port|""> <argv...>
start_svc() {
  local name="$1" port="$2"
  shift 2
  local running
  running="$(pid_of "$name")"
  if [[ -n "$running" ]]; then
    printf '%s\n' "$running" >"$PIDDIR/$name.pid"
    say "$name 已在运行 pid=$running，跳过启动"
    return 0
  fi
  if [[ -n "$port" ]] && port_listening "$port"; then
    warn "错误：端口 $port 已被其它进程占用，拒绝启动 $name（先 ./stop-all.sh 或查占用）"
    FAILED=1
    return 1
  fi
  setsid nohup "$@" >"$LOGDIR/$name.log" 2>&1 </dev/null &
  local pid=$!
  printf '%s\n' "$pid" >"$PIDDIR/$name.pid"
  say "$name 启动 pid=$pid${port:+ port=$port} log=$LOGDIR/$name.log"
  return 0
}

# wait_port <port> [秒]
wait_port() {
  local port="$1" secs="${2:-6}" i n
  n=$((secs * 10))
  for ((i = 0; i < n; i++)); do
    port_listening "$port" && return 0
    sleep 0.1
  done
  return 1
}

check_http() { # <label> <url>
  local label="$1" url="$2" code
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$url" 2>/dev/null)"
  [[ -n "$code" ]] || code="000"
  if [[ "$code" == "200" ]]; then
    printf '[health] %-24s %s  OK   %s\n' "$label" "$code" "$url"
    return 0
  fi
  printf '[health] %-24s %s  FAIL %s\n' "$label" "$code" "$url"
  FAILED=1
  return 1
}

# 公网入口：刚起 tunnel 可能还在建连，重试最多 10 秒。
check_public() {
  local i code
  for ((i = 0; i < 10; i++)); do
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$PUBLIC_URL" 2>/dev/null)"
    [[ -n "$code" ]] || code="000"
    if [[ "$code" == "200" ]]; then
      printf '[health] %-24s %s  OK   %s\n' "public /shop/api/health" "$code" "$PUBLIC_URL"
      return 0
    fi
    sleep 1
  done
  printf '[health] %-24s %s  FAIL %s\n' "public /shop/api/health" "$code" "$PUBLIC_URL"
  FAILED=1
  warn "公网入口异常：tunnel-client pid=$(pid_of tunnel-client || echo none)"
  warn "tunnel-client 日志末尾："
  tail -n 5 "$LOGDIR/tunnel-client.log" 2>/dev/null | sed 's/^/[start]   /' >&2 || true
  warn "（/__tunnel__/status 路由在本网关不存在，实测返回 404）"
  return 1
}

# ---------- 1. 三个后端 ----------
say "1/5 启动后端服务"
start_svc product_service "$PRODUCT_PORT" \
  "$ROOT/build/product_service" "$PRODUCT_PORT" "$ROOT/data/product.db"
start_svc review_service "$REVIEW_PORT" \
  "$ROOT/build/review_service" "$REVIEW_PORT" "$ROOT/data/review-kv"
start_svc order_service "$ORDER_PORT" \
  "$ROOT/build/order_service" "$ORDER_PORT" "$ROOT/data/order.db" "127.0.0.1:${PRODUCT_PORT}"

# ---------- 2. 等后端端口 ----------
say "2/5 等待后端端口"
for pair in "product_service:$PRODUCT_PORT" "review_service:$REVIEW_PORT" "order_service:$ORDER_PORT"; do
  name="${pair%%:*}"
  port="${pair##*:}"
  if wait_port "$port" 8; then
    say "端口 $port 已就绪（$name）"
  else
    warn "端口 $port 8 秒内未就绪（$name），日志尾部："
    tail -n 5 "$LOGDIR/$name.log" 2>/dev/null | sed 's/^/[start]   /' >&2 || true
    FAILED=1
  fi
done

# ---------- 3. 网关 ----------
say "3/5 启动网关"
start_svc gateway "$SHOP_PORT" "$ROOT/build/gateway" "$SHOP_PORT" "$ROOT/www"
if wait_port "$SHOP_PORT" 8; then
  say "端口 $SHOP_PORT 已就绪（gateway）"
else
  warn "网关端口 $SHOP_PORT 未就绪，日志尾部："
  tail -n 5 "$LOGDIR/gateway.log" 2>/dev/null | sed 's/^/[start]   /' >&2 || true
  FAILED=1
fi

# ---------- 4. tunnel-client ----------
say "4/5 启动 tunnel-client"
if [[ -x "$TUNNEL_BIN" ]]; then
  start_svc tunnel-client "" \
    "$TUNNEL_BIN" --server "$TUNNEL_SERVER" --local "$SHOP_PORT" \
    --subdomain "$TUNNEL_SUBDOMAIN" --token "$TUNNEL_TOKEN"
  sleep 2
else
  warn "tunnel-client 二进制不存在或不可执行：$TUNNEL_BIN"
  FAILED=1
fi

# ---------- 5. 健康检查：本机 5 项 + 公网 1 项 ----------
say "5/5 健康检查"
check_http "GET /api/health" "$BASE/api/health"
check_http "GET /api/products" "$BASE/api/products"
check_http "GET /api/product/1" "$BASE/api/product/1"
check_http "GET /api/reviews/1" "$BASE/api/reviews/1"
check_http "GET /images/p1.png" "$BASE/images/p1.png"
check_public

echo
if ((FAILED != 0)); then
  echo "[start] 结果：有检查项失败（非 0 退出）"
  exit 1
fi
echo "[start] 结果：全部就绪（本机 $BASE + 公网 $PUBLIC_URL）"
echo "[start] 停止：$ROOT/stop-all.sh   探针：$ROOT/probe.sh --minutes 5"
exit 0
