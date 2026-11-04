#!/usr/bin/env bash
# 断线续传（Resume）验收：起 3 个 danmaku_service 实例，跑 Python 场景，按 pid 收尾。
#
#   scripts/accept_resume.sh
#   RESUME_PORT=29303 GAP_PORT=29304 RATE_PORT=29305 scripts/accept_resume.sh
#
# 实例：
#   RESUME_PORT  resume-buffer=1000, rate-limit=0  （补发 6..15 / 压测 / 并发重连）
#   GAP_PORT     resume-buffer=4,    rate-limit=0  （resume_gap 与区间边界）
#   RATE_PORT    resume-buffer=1000, rate-limit=5  （限流回归）
#
# 进程管理：每个实例记录 pid，退出时先校验 /proc/<pid>/cmdline 指向本脚本编译的
# 二进制，再 kill；不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/danmaku_service"
RESUME_PORT="${RESUME_PORT:-29303}"
GAP_PORT="${GAP_PORT:-29304}"
RATE_PORT="${RATE_PORT:-29305}"
TMP="$(mktemp -d)"
PIDS=()

cleanup() {
  local entry name pid
  for entry in "${PIDS[@]:-}"; do
    name="${entry%%:*}"
    pid="${entry##*:}"
    if [[ -r "/proc/$pid/cmdline" ]] && tr '\0' ' ' <"/proc/$pid/cmdline" | grep -qF "$BIN"; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      echo "[cleanup] 已停止 $name pid=$pid"
    fi
  done
  rm -rf "$TMP"
}
trap cleanup EXIT

start_instance() {
  local name="$1" port="$2" buffer="$3" rate="$4"
  "$BIN" --port "$port" --db "$TMP/$name.db" --rate-limit "$rate" \
    --resume-buffer "$buffer" --max-queue 256 >>"$TMP/$name.log" 2>&1 &
  local pid=$!
  PIDS+=("$name:$pid")
  echo "[start] $name pid=$pid port=$port resume-buffer=$buffer rate-limit=$rate"
}

wait_ready() {
  local port="$1" name="$2" i
  for i in $(seq 1 200); do
    if python3 - "$port" <<'PY'
import socket, sys
sock = socket.socket()
sock.settimeout(0.2)
try:
    sock.connect(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    sock.close()
PY
    then
      return 0
    fi
    sleep 0.05
  done
  echo "[error] $name 未在 10s 内监听端口 $port" >&2
  cat "$TMP/$name.log" >&2
  return 1
}

echo "[build] 编译 danmaku_service"
if ! "$ROOT/build.sh" >"$TMP/build.log" 2>&1; then
  echo "[error] 编译失败：" >&2
  cat "$TMP/build.log" >&2
  exit 1
fi
tail -2 "$TMP/build.log"

start_instance resume "$RESUME_PORT" 1000 0
start_instance gap "$GAP_PORT" 4 0
start_instance rate "$RATE_PORT" 1000 5

wait_ready "$RESUME_PORT" resume || exit 1
wait_ready "$GAP_PORT" gap || exit 1
wait_ready "$RATE_PORT" rate || exit 1

python3 "$ROOT/scripts/accept_resume.py" \
  --port "$RESUME_PORT" --gap-port "$GAP_PORT" --rate-port "$RATE_PORT"
STATUS=$?

echo "[logs] 三个实例的 stderr/stdout 片段："
for name in resume gap rate; do
  echo "--- $name ---"
  grep -E 'resume|gap|join|listening' "$TMP/$name.log" | head -20
done

exit "$STATUS"
