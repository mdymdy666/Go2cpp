#!/usr/bin/env bash
# 抖音 C10 可靠性验收：进程被 kill -9 后数据仍可读，SQLite 内容不变。
#
# 场景：起 user_service(29101) + video_service(29102)，两者共享同一个 SQLite 库 →
#       注册 alice、发 2 条视频 → 记录 users/videos 行数与库文件字节数 →
#       kill -9 两个服务、等 1s 重启 → 再查 alice 资料与两条视频详情（含作者名）→
#       比对重启前后的 SQLite 快照 → PASS/FAIL。
#
# 只按脚本记录的 PID 清理，不用 pkill -f。默认端口被占用时自动换临时端口
# （用服务自己的 argv[1] 参数；六个服务的 main 都支持 `<bin> <port>`）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_USER="$ROOT/build/user_service"
BIN_VIDEO="$ROOT/build/video_service"
DB="$ROOT/data/reliability.db"
LOG_USER="$ROOT/logs/reliability_user.log"
LOG_VIDEO="$ROOT/logs/reliability_video.log"
TMP="$(mktemp -d)"

PORT_USER="${DOUYIN_RELIABILITY_USER_PORT:-29101}"
PORT_VIDEO="${DOUYIN_RELIABILITY_VIDEO_PORT:-29102}"
BASE_USER=""
BASE_VIDEO=""

USER_PID=""
VIDEO_PID=""
PASSED=0
FAILED=0
TOKEN=""
USER_ID=""
VIDEO_IDS=()

cleanup() {
  for pid in "$VIDEO_PID" "$USER_PID"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$TMP"
}
trap cleanup EXIT

check() { # name expected actual [detail]
  local name="$1" expected="$2" actual="$3" detail="${4:-}"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-34s PASS  HTTP %s %s\n' "$name" "$actual" "$detail"
    PASSED=$((PASSED + 1))
  else
    printf '  %-34s FAIL  HTTP %s (期望 %s) %s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

check_eq() { # name expected actual detail
  local name="$1" expected="$2" actual="$3" detail="${4:-}"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-34s PASS  %s%s\n' "$name" "$actual" "$detail"
    PASSED=$((PASSED + 1))
  else
    printf '  %-34s FAIL  期望 %s，实际 %s%s\n' "$name" "$expected" "$actual" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-34s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-34s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

post() { # base path json [token]
  local base="$1" path="$2" data="$3" token="${4:-}"
  local args=(-s -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$data")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}" "$base$path"
}

get() { # base path
  curl -s -o "$TMP/body" -w '%{http_code}' "$1$2"
}

port_busy() {
  ss -ltn "sport = :$1" 2>/dev/null | tail -n +2 | grep -q .
}

pick_ports() {
  if ! port_busy "$PORT_USER" && ! port_busy "$PORT_VIDEO"; then
    return 0
  fi
  local offset
  for offset in 10000 11000 12000 13000 14000 15000 16000; do
    local candidate_user=$((PORT_USER + offset))
    local candidate_video=$((PORT_VIDEO + offset))
    if ! port_busy "$candidate_user" && ! port_busy "$candidate_video"; then
      echo "[reliability] 默认端口 $PORT_USER/$PORT_VIDEO 被占用，改用 $candidate_user/$candidate_video"
      PORT_USER="$candidate_user"
      PORT_VIDEO="$candidate_video"
      return 0
    fi
  done
  echo "[reliability] 找不到可用端口，放弃" >&2
  return 1
}

wait_health() { # port name pid log
  local port="$1" name="$2" pid="$3" log="$4"
  local i
  for i in $(seq 1 200); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "[reliability] $name 启动后退出，日志：" >&2
      tail -n 20 "$log" >&2
      return 1
    fi
    sleep 0.05
  done
  echo "[reliability] $name 健康检查超时（端口 $port），日志：" >&2
  tail -n 20 "$log" >&2
  return 1
}

start_user() {
  DOUYIN_DB="$DB" "$BIN_USER" "$PORT_USER" >>"$LOG_USER" 2>&1 &
  USER_PID=$!
  wait_health "$PORT_USER" user_service "$USER_PID" "$LOG_USER"
}

start_video() {
  DOUYIN_DB="$DB" "$BIN_VIDEO" "$PORT_VIDEO" >>"$LOG_VIDEO" 2>&1 &
  VIDEO_PID=$!
  wait_health "$PORT_VIDEO" video_service "$VIDEO_PID" "$LOG_VIDEO"
}

# SQLite 快照：users/videos 行数 + 库文件（db + wal + shm）总字节数。
# sqlite3 客户端在验收机不一定安装，统一用 python3 标准库读取。
snapshot() { # out.json
  python3 - "$DB" "$1" <<'PY'
import json
import os
import sqlite3
import sys

db_path, out_path = sys.argv[1], sys.argv[2]
con = sqlite3.connect(db_path, timeout=5)
counts = {}
for table in ("users", "videos"):
    counts[table] = con.execute("SELECT COUNT(*) FROM %s" % table).fetchone()[0]
con.close()
total = 0
for suffix in ("", "-wal", "-shm"):
    path = db_path + suffix
    if os.path.exists(path):
        total += os.path.getsize(path)
with open(out_path, "w", encoding="utf-8") as fh:
    json.dump({"users": counts["users"], "videos": counts["videos"], "bytes": total},
              fh, separators=(",", ":"))
PY
}

snap_get() { # file key
  sed -n "s/.*\"$2\":\([0-9]*\).*/\1/p" "$1"
}

if [[ ! -x "$BIN_USER" || ! -x "$BIN_VIDEO" ]]; then
  echo "找不到 $BIN_USER 或 $BIN_VIDEO，请先跑 app/douyin/build.sh" >&2
  exit 1
fi

mkdir -p "$ROOT/data" "$ROOT/logs"
pick_ports || exit 1
BASE_USER="http://127.0.0.1:$PORT_USER"
BASE_VIDEO="http://127.0.0.1:$PORT_VIDEO"

rm -f "$DB" "$DB-wal" "$DB-shm"
: >"$LOG_USER"
: >"$LOG_VIDEO"

echo "[1/5] 启动 user_service:$PORT_USER + video_service:$PORT_VIDEO（db=$DB）"
if ! start_user; then
  echo "[reliability] FAIL（user_service 未就绪）"
  exit 1
fi
if ! start_video; then
  echo "[reliability] FAIL（video_service 未就绪）"
  exit 1
fi
echo "  user_service  pid=$USER_PID"
echo "  video_service pid=$VIDEO_PID"

echo "[2/5] 注册 alice 并发 2 条视频"
code=$(post "$BASE_USER" /api/user/register \
  '{"name":"alice","email":"alice@reliability.local","password":"secret123"}')
check "注册 alice" 200 "$code"
expect_body "注册响应 ok" '"ok":true'
TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body")
USER_ID=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
[[ -n "$TOKEN" && -n "$USER_ID" ]] || {
  echo "[reliability] 无法从注册响应提取 token/user id：$(head -c 300 "$TMP/body")" >&2
  echo "[reliability] FAIL"
  exit 1
}
echo "  alice user_id=$USER_ID"

for index in 1 2; do
  title="reliability-video-$index"
  code=$(post "$BASE_VIDEO" /api/video \
    "{\"title\":\"$title\",\"url\":\"https://example.com/$title.mp4\"}" "$TOKEN")
  check "发布视频 $index" 200 "$code"
  expect_body "视频 $index 响应含 title" "\"title\":\"$title\""
  video_id=$(sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
  if [[ -z "$video_id" ]]; then
    echo "[reliability] 无法从发布响应提取 video id：$(head -c 300 "$TMP/body")" >&2
    echo "[reliability] FAIL"
    exit 1
  fi
  VIDEO_IDS+=("$video_id")
  echo "  video_$index id=$video_id"
done

echo "[3/5] 重启前 SQLite 快照"
if ! snapshot "$TMP/before.json"; then
  echo "[reliability] 快照失败（python3 sqlite3 读取 $DB）" >&2
  echo "[reliability] FAIL"
  exit 1
fi
BEFORE_USERS=$(snap_get "$TMP/before.json" users)
BEFORE_VIDEOS=$(snap_get "$TMP/before.json" videos)
BEFORE_BYTES=$(snap_get "$TMP/before.json" bytes)
printf '  users=%s videos=%s bytes=%s\n' "$BEFORE_USERS" "$BEFORE_VIDEOS" "$BEFORE_BYTES"
check_eq "重启前 users 行数" 1 "$BEFORE_USERS"
check_eq "重启前 videos 行数" 2 "$BEFORE_VIDEOS"

echo "[4/5] kill -9 两个服务 → 等 1s → 重启"
{
  kill -9 "$USER_PID" "$VIDEO_PID" 2>/dev/null || true
  wait "$USER_PID" 2>/dev/null || true
  wait "$VIDEO_PID" 2>/dev/null || true
} 2>/dev/null
USER_PID=""
VIDEO_PID=""
sleep 1
if ! start_user; then
  echo "[reliability] FAIL（user_service 重启失败）"
  exit 1
fi
if ! start_video; then
  echo "[reliability] FAIL（video_service 重启失败）"
  exit 1
fi
echo "  重启完成 user pid=$USER_PID video pid=$VIDEO_PID"
sleep 0.2

echo "[5/5] 重启后读数据 + SQLite 快照比对"
code=$(get "$BASE_USER" "/api/user/$USER_ID")
check "重启后 GET alice 资料" 200 "$code"
expect_body "资料含 name=alice" '"name":"alice"'

for index in 1 2; do
  video_id="${VIDEO_IDS[$((index - 1))]}"
  title="reliability-video-$index"
  code=$(get "$BASE_VIDEO" "/api/video/$video_id")
  check "重启后 GET 视频 $index 详情" 200 "$code"
  expect_body "视频 $index 作者名为 alice" '"author":"alice"'
  expect_body "视频 $index 标题仍为 $title" "\"title\":\"$title\""
done

if ! snapshot "$TMP/after.json"; then
  echo "[reliability] 重启后快照失败" >&2
  echo "[reliability] FAIL"
  exit 1
fi
AFTER_USERS=$(snap_get "$TMP/after.json" users)
AFTER_VIDEOS=$(snap_get "$TMP/after.json" videos)
AFTER_BYTES=$(snap_get "$TMP/after.json" bytes)
printf '  users=%s videos=%s bytes=%s\n' "$AFTER_USERS" "$AFTER_VIDEOS" "$AFTER_BYTES"
check_eq "重启后 users 行数" "$BEFORE_USERS" "$AFTER_USERS"
check_eq "重启后 videos 行数" "$BEFORE_VIDEOS" "$AFTER_VIDEOS"
check_eq "SQLite 文件字节数（db+wal+shm）" "$BEFORE_BYTES" "$AFTER_BYTES"

echo "---------------------------------------------"
printf '[reliability] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[reliability] PASS"
  exit 0
fi
echo "[reliability] FAIL"
exit 1
