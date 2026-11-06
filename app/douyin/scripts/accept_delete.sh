#!/usr/bin/env bash
# 视频 / 评论软删除验收（第 12 轮）：权限 / 软删后不可见 / 计数一致 / 幂等。
#
# 用例：
#   1) bob 删自己的评论 200、carol 删别人 403
#   2) 视频作者能删他人评论；comment_count 逐次减 1 且与详情一致
#   3) 视频删除仅作者：bob 403、无 token 401、重复删除 404
#   4) 软删后详情/feed/主页/搜索/落地页/评论列表全部 404 或不可见
#   5) DB 行保留（deleted_at 非空），重复删除不改时间戳
#   6) 已删评论不能再点赞；已删视频不能再评论
#   7) 其余视频不受影响；首页有删除入口，/settings 可读
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_delete.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_delete.pids"
DB="$ROOT/data/accept_delete.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0

GW_PORT="${DOUYIN_GATEWAY_PORT:-28080}"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
VIDEO_PORT="${DOUYIN_VIDEO_PORT:-29102}"
FEED_PORT="${DOUYIN_FEED_PORT:-29103}"
INTERACTION_PORT="${DOUYIN_INTERACTION_PORT:-29104}"
LIVE_PORT="${DOUYIN_LIVE_PORT:-29105}"
SEARCH_PORT="${DOUYIN_SEARCH_PORT:-29106}"
GW="http://127.0.0.1:$GW_PORT"

mkdir -p "$LOG_DIR" "$ROOT/data" "$TMP"
: >"$PID_FILE"

stop_all() {
  [[ -f "$PID_FILE" ]] || return 0
  local name pid port expected actual
  while read -r name pid port; do
    [[ -n "${name:-}" && -n "${pid:-}" ]] || continue
    expected="$BIN_DIR/$name"
    actual="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    if [[ -n "$actual" && "$actual" != "$expected" ]]; then
      printf '[stop] 跳过 pid=%s（exe=%s 不是 %s，疑似 pid 复用）\n' \
        "$pid" "$actual" "$expected" >&2
    else
      if [[ -n "$actual" ]]; then
        kill -9 "$pid" 2>/dev/null || true
      fi
      wait "$pid" 2>/dev/null || true
      printf '[stop] %s pid=%s 已停止\n' "$name" "$pid"
    fi
  done <"$PID_FILE"
  : >"$PID_FILE"
}

cleanup() {
  stop_all
  rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check() { # name expected actual [detail]
  local name="$1" expected="$2" actual="$3" detail="${4:-}"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-46s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-46s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

post() { # path json [token]
  local path="$1" payload="$2" token="${3:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$payload" "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

del() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X DELETE "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

get() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { grep -o "\"$1\":[0-9]*" "$TMP/body" | head -1 | cut -d: -f2; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
json_field() { grep -o "\"$1\":\(true\|false\)" "$TMP/body" | head -1 | cut -d: -f2; }
nth_like_count() { grep -o '"like_count":[0-9]*' "$TMP/body" | sed -n "${1}p" | cut -d: -f2; }
nth_liked() { grep -o '"liked":\(true\|false\)' "$TMP/body" | sed -n "${1}p" | cut -d: -f2; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_delete_$name.log" 2>&1 &
  local pid=$!
  echo "$name $pid $port" >>"$PID_FILE"
  local ready=0
  for _ in $(seq 1 200); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "$name 启动失败，日志：" >&2
    cat "$LOG_DIR/accept_delete_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}


contains() {
  local name="$1" needle="$2" file="$3"
  if grep -qF -- "$needle" "$file"; then
    printf '  %-48s PASS  含 %s\n' "$name" "$needle"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  未找到 %s\n' "$name" "$needle"
    FAILED=$((FAILED + 1))
  fi
}

start_all() {
  rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_delete_"*.log
  start_service user_service "$USER_PORT"
  start_service video_service "$VIDEO_PORT"
  start_service feed_service "$FEED_PORT"
  start_service interaction_service "$INTERACTION_PORT"
  start_service live_service "$LIVE_PORT"
  start_service search_service "$SEARCH_PORT"
  start_service gateway "$GW_PORT"
}

finish() {
  echo "---------------------------------------------"
  if [[ "$FAILED" -eq 0 ]]; then
    echo "[accept_delete] 结果：$PASSED PASS / 0 FAIL"
    echo "[accept_delete] PASS"
    exit 0
  fi
  echo "[accept_delete] 结果：$PASSED PASS / $FAILED FAIL"
  echo "[accept_delete] FAIL"
  exit 1
}

db_scalar() { # sql
  python3 - "$DB" "$1" <<'PY'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
try:
    row = con.execute(sys.argv[2]).fetchone()
    print("" if row is None or row[0] is None else row[0])
finally:
    con.close()
PY
}

echo "[0/8] 启动 7 个进程"
start_all

echo "[1/8] 注册 alice / bob / carol，alice 发 3 条视频"
code=$(post "/api/user/register" '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
ALICE_TOKEN="$(json_str token)"
ALICE_ID="$(json_int id)"
code=$(post "/api/user/register" '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
BOB_TOKEN="$(json_str token)"
code=$(post "/api/user/register" '{"name":"carol","email":"carol@example.com","password":"secret123"}')
check "注册 carol" 200 "$code"
CAROL_TOKEN="$(json_str token)"
for i in 1 2 3; do
  code=$(post "/api/video" "{\"title\":\"删除验收视频 $i #del\",\"url\":\"/videos/$i.mp4\"}" "$ALICE_TOKEN")
  check "alice 发布视频 $i" 200 "$code"
done
V1=1; V2=2; V3=3

echo "[2/8] 评论两条，详情 comment_count=2"
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"第一条评论\"}" "$BOB_TOKEN")
check "bob 评论 1" 200 "$code"
C1="$(json_int id)"
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"第二条评论\"}" "$BOB_TOKEN")
check "bob 评论 2" 200 "$code"
C2="$(json_int id)"
code=$(get "/api/video/$V1")
check "详情读取" 200 "$code"
check "  comment_count=2" 2 "$(json_int comment_count)"

echo "[3/8] 评论删除权限"
code=$(del "/api/comment/$C1" "$CAROL_TOKEN")
check "carol 删别人评论 → 403" 403 "$code"
code=$(del "/api/comment/$C1")
check "无 token 删评论 → 401" 401 "$code"
code=$(del "/api/comment/$C1" "$BOB_TOKEN")
check "bob 删自己的评论 → 200" 200 "$code"
check "  返回 comment_count=1" 1 "$(json_int comment_count)"
code=$(get "/api/comment/$V1")
check "列表仍可读" 200 "$code"
if grep -qF "\"id\":$C1," "$TMP/body"; then
  check "已删评论不在列表" absent present
else
  check "已删评论不在列表" absent absent
fi
code=$(del "/api/comment/$C1" "$BOB_TOKEN")
check "重复删评论 → 404（幂等）" 404 "$code"
code=$(post "/api/comment/$C1/like" '{}' "$BOB_TOKEN")
check "已删评论点赞 → 404" 404 "$code"

echo "[4/8] 视频作者也能删评论 + 计数一致"
code=$(del "/api/comment/$C2" "$ALICE_TOKEN")
check "alice（视频作者）删 bob 评论 → 200" 200 "$code"
check "  comment_count=0" 0 "$(json_int comment_count)"
code=$(get "/api/video/$V1")
check "详情 comment_count 一致" 0 "$(json_int comment_count)"

echo "[5/8] 视频删除权限与软删"
code=$(del "/api/video/$V2" "$BOB_TOKEN")
check "bob 删 alice 视频 → 403" 403 "$code"
code=$(del "/api/video/$V2")
check "无 token 删视频 → 401" 401 "$code"
code=$(del "/api/video/$V2" "$ALICE_TOKEN")
check "alice 删自己的视频 → 200" 200 "$code"
code=$(get "/api/video/$V2")
check "软删后详情 → 404" 404 "$code"
code=$(del "/api/video/$V2" "$ALICE_TOKEN")
check "重复删视频 → 404（幂等）" 404 "$code"
T_AFTER="$(db_scalar "SELECT deleted_at FROM videos WHERE id=$V2")"
if [[ -n "$T_AFTER" ]]; then
  check "DB 行保留且 deleted_at 非空" yes yes
else
  check "DB 行保留且 deleted_at 非空" yes no
fi
T_FIRST="$T_AFTER"
code=$(del "/api/video/$V2" "$ALICE_TOKEN")
sleep 1
code=$(del "/api/video/$V2" "$ALICE_TOKEN")
T_SECOND="$(db_scalar "SELECT deleted_at FROM videos WHERE id=$V2")"
check "重复删除不改 deleted_at" "$T_FIRST" "$T_SECOND"

echo "[6/8] 软删后 feed / 主页 / 搜索 / 落地页全部不可见"
code=$(get "/api/feed?limit=100")
check "feed 可读" 200 "$code"
if grep -qF "\"video\":{\"id\":$V2," "$TMP/body"; then
  check "feed 不含已删视频" absent present
else
  check "feed 不含已删视频" absent absent
fi
code=$(get "/api/user/$ALICE_ID/videos?limit=100")
check "主页作品可读" 200 "$code"
if grep -qF "\"video\":{\"id\":$V2," "$TMP/body"; then
  check "主页作品不含已删视频" absent present
else
  check "主页作品不含已删视频" absent absent
fi
code=$(get "/api/search?q=删除验收视频%202")
check "搜索可读" 200 "$code"
if grep -qF "\"id\":$V2," "$TMP/body"; then
  check "搜索不含已删视频" absent present
else
  check "搜索不含已删视频" absent absent
fi
code=$(get "/v/$V2")
check "分享落地页 → 404" 404 "$code"
code=$(get "/api/video/$V2/comments")
check "已删视频评论列表 → 404" 404 "$code"
code=$(post "/api/comment" "{\"video_id\":$V2,\"content\":\"还能评论吗\"}" "$BOB_TOKEN")
check "已删视频不能评论 → 404" 404 "$code"

echo "[7/8] 其余视频不受影响"
code=$(get "/api/video/$V3")
check "视频 3 详情仍 200" 200 "$code"
code=$(del "/api/video/$V3" "$CAROL_TOKEN")
check "carol 删视频 3 → 403" 403 "$code"
code=$(get "/api/video/$V1")
check "视频 1 详情仍 200" 200 "$code"

echo "[8/8] 前端删除入口与页面"
code=$(get "/")
check "首页可读" 200 "$code"
contains "  首页含删除视频入口" "删除视频" "$TMP/body"
contains "  首页含删除评论入口" "删除评论" "$TMP/body"
contains "  首页含设置入口" "whoAvatar" "$TMP/body"
code=$(get "/settings")
check "设置页可读" 200 "$code"

finish
