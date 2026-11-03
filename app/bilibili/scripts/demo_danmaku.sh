#!/usr/bin/env bash
# B 站弹幕服务一键 demo：编译 → 起服务 → 双/三连接 WS 演示 → 历史/限流 → 停服。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${DANMAKU_PORT:-29203}"
DB="$ROOT/data/demo.db"

mkdir -p "$ROOT/build" "$ROOT/logs" "$ROOT/data"
rm -f "$DB" "$DB-wal" "$DB-shm"

"$ROOT/build.sh"

"$ROOT/build/danmaku_service" --port "$PORT" --db "$DB" \
  > "$ROOT/logs/danmaku_service.log" 2>&1 &
PID=$!

cleanup() {
  kill "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:$PORT/api/danmaku/1/online" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

cd "$ROOT"
set +e
python3 scripts/demo_danmaku.py --port "$PORT" 2>&1 | tee "$ROOT/logs/demo.log"
DEMO_STATUS=${PIPESTATUS[0]}
set -e

echo "=== 服务端日志（节选） ==="
tail -n 40 "$ROOT/logs/danmaku_service.log"
exit "$DEMO_STATUS"
