#!/usr/bin/env bash
# B 站弹幕 Web demo 一键启动：
#   1) danmaku_service（默认 29203，DB /tmp/bili_demo.db，--rate-limit 100）
#   2) python3 http.server（默认 8000，根目录 web/）
# Ctrl+C 同时停止两者；pid 落 logs/，不使用 pkill -f。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE="$ROOT/build/danmaku_service"
WEB_DIR="$ROOT/web"
VIDEO="$WEB_DIR/demo.mp4"
PORT="${DANMAKU_PORT:-29203}"
HTTP_PORT="${WEB_PORT:-8000}"
DB="${DANMAKU_DB:-/tmp/bili_demo.db}"
LOG_DIR="$ROOT/logs"
PID_SERVICE="$LOG_DIR/demo_web.service.pid"
PID_HTTP="$LOG_DIR/demo_web.http.pid"
SERVICE_PID=""
HTTP_PID=""

mkdir -p "$LOG_DIR"

log() { printf '[demo_web] %s\n' "$*"; }
die() { printf '[demo_web][错误] %s\n' "$*" >&2; exit 1; }

# 只杀自己启动的进程（按 pid 文件），绝不 pkill -f。
cleanup() {
  set +e
  for pid_file in "$PID_SERVICE" "$PID_HTTP"; do
    [ -f "$pid_file" ] || continue
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null
      for _ in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
      done
      if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        log "强制停止 pid=$pid"
      else
        log "已停止 pid=$pid"
      fi
    fi
    rm -f "$pid_file"
  done
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# ---- 0. 视频 ----------------------------------------------------------------
if [ ! -s "$VIDEO" ]; then
  command -v ffmpeg >/dev/null 2>&1 || die "缺少 web/demo.mp4，且未找到 ffmpeg"
  log "生成 40s 测试视频 → web/demo.mp4"
  ffmpeg -y -loglevel error \
    -f lavfi -i testsrc=duration=40:size=1280x720:rate=30 \
    -f lavfi -i sine=frequency=440:duration=40 \
    -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest "$VIDEO"
fi

# ---- 1. danmaku_service -----------------------------------------------------
[ -x "$SERVICE" ] || die "未找到 $SERVICE，请先在 app/bilibili 执行 bash build.sh"

if curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/danmaku/1/online" 2>/dev/null; then
  log "端口 $PORT 已有 danmaku_service，复用（退出时不会停止它）"
else
  : > "$LOG_DIR/danmaku_service.log"
  log "启动 danmaku_service :$PORT（db=$DB rate-limit=100）"
  "$SERVICE" --port "$PORT" --db "$DB" --rate-limit 100 \
    >> "$LOG_DIR/danmaku_service.log" 2>&1 &
  SERVICE_PID=$!
  echo "$SERVICE_PID" > "$PID_SERVICE"
fi

# ---- 2. 静态站点 ------------------------------------------------------------
if curl -sf -o /dev/null "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null; then
  die "端口 $HTTP_PORT 已被占用（可能上次没退干净），请先释放"
fi
: > "$LOG_DIR/web_http.log"
log "启动静态站点 :$HTTP_PORT（web/）"
python3 -m http.server "$HTTP_PORT" --bind 0.0.0.0 --directory "$WEB_DIR" \
  >> "$LOG_DIR/web_http.log" 2>&1 &
HTTP_PID=$!
echo "$HTTP_PID" > "$PID_HTTP"

# ---- 3. 等待就绪 ------------------------------------------------------------
READY=0
for _ in $(seq 1 120); do
  if curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/danmaku/1/online" 2>/dev/null &&
     curl -sf -o /dev/null "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null; then
    READY=1
    break
  fi
  sleep 0.1
done
if [ "$READY" != "1" ]; then
  tail -n 20 "$LOG_DIR/danmaku_service.log" 2>/dev/null || true
  tail -n 20 "$LOG_DIR/web_http.log" 2>/dev/null || true
  die "启动超时，见上方日志"
fi

HOST_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
[ -n "${HOST_IP:-}" ] || HOST_IP="127.0.0.1"

log "就绪"
cat <<EOF

============================================================
 B 站弹幕 Web Demo 已就绪
------------------------------------------------------------
 页面（推荐）  : http://localhost:$HTTP_PORT/
 页面（备用）  : http://127.0.0.1:$HTTP_PORT/
 局域网地址    : http://$HOST_IP:$HTTP_PORT/
 视频          : http://localhost:$HTTP_PORT/demo.mp4
 WS 弹幕房间   : ws://localhost:$PORT/ws/danmaku?video_id=1
 历史弹幕 API  : http://localhost:$PORT/api/danmaku/1
 服务日志      : $LOG_DIR/danmaku_service.log
 站点日志      : $LOG_DIR/web_http.log
------------------------------------------------------------
 验收：两个浏览器窗口打开页面，A 发弹幕 B 实时收到。
 停止：Ctrl+C（两个进程一起停，pid 文件 logs/demo_web.*.pid）
============================================================

EOF

# ---- 4. 守住子进程 ----------------------------------------------------------
while true; do
  if [ -n "$SERVICE_PID" ] && ! kill -0 "$SERVICE_PID" 2>/dev/null; then
    log "danmaku_service 已退出（pid=$SERVICE_PID），退出 demo"
    break
  fi
  if ! kill -0 "$HTTP_PID" 2>/dev/null; then
    log "http.server 已退出（pid=$HTTP_PID），退出 demo"
    break
  fi
  sleep 1
done
