#!/usr/bin/env bash
# 评论点赞验收（第 1 轮新增）：启动 7 进程 → 点赞/取消/幂等/列表状态/鉴权/404。
#
# 用例：
#   1) bob 给顶层评论点赞 → like_count=1、liked=true
#   2) 重复点赞 → 计数不变（幂等）
#   3) 匿名列表 → like_count=1、liked=false（不泄露他人状态）
#   4) 带 token 列表 → liked=true（本人视角）
#   5) alice 也点赞 → like_count=2
#   6) bob 取消 → like_count=1、liked=false
#   7) 回复也能点赞/取消（二级评论同表）
#   8) 不存在的评论 → 404；无 token → 401
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_comment_like.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_comment_like.pids"
DB="$ROOT/data/accept_comment_like.db"
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
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_comment_like_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_comment_like_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_comment_like_"*.log

echo "[0/8] 启动 7 个进程"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

echo "[1/8] 注册 alice / bob 并发布视频"
code=$(post "/api/user/register" '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
ALICE_TOKEN="$(json_str token)"
code=$(post "/api/user/register" '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
BOB_TOKEN="$(json_str token)"
if [[ -z "$ALICE_TOKEN" || -z "$BOB_TOKEN" ]]; then
  echo "取 token 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi
code=$(post "/api/video" '{"title":"评论点赞验收 #话题","url":"/videos/demo1.mp4"}' "$ALICE_TOKEN")
check "alice 发布视频" 200 "$code"
VIDEO_ID="$(json_int id)"
if [[ -z "$VIDEO_ID" ]]; then
  echo "取 video id 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi

echo "[2/8] bob 发评论 + alice 回复"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"这条视频不错\"}" "$BOB_TOKEN")
check "bob 发顶层评论" 200 "$code"
COMMENT_ID="$(json_int id)"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"谢谢支持\",\"parent_id\":$COMMENT_ID}" "$ALICE_TOKEN")
check "alice 发二级回复" 200 "$code"
REPLY_ID="$(json_int id)"
if [[ -z "$COMMENT_ID" || -z "$REPLY_ID" ]]; then
  echo "取评论 id 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi

echo "[3/8] 顶层评论点赞 / 幂等"
code=$(post "/api/comment/$COMMENT_ID/like" '{}' "$BOB_TOKEN")
check "bob 点赞顶层评论" 200 "$code"
check "  点赞数" 1 "$(json_int like_count)"
check "  本人状态 liked" true "$(json_field liked)"
code=$(post "/api/comment/$COMMENT_ID/like" '{}' "$BOB_TOKEN")
check "重复点赞（幂等）" 200 "$code"
check "  计数不变" 1 "$(json_int like_count)"

echo "[4/8] 列表视角：匿名看不到他人状态，本人能看到"
code=$(get "/api/comment/$VIDEO_ID")
check "匿名读评论列表" 200 "$code"
check "  顶层评论 like_count" 1 "$(nth_like_count 1)"
check "  匿名 liked=false" false "$(nth_liked 1)"
code=$(get "/api/comment/$VIDEO_ID" "$BOB_TOKEN")
check "bob 读评论列表" 200 "$code"
check "  本人 liked=true" true "$(nth_liked 1)"
code=$(get "/api/comment/$VIDEO_ID" "$ALICE_TOKEN")
check "alice 读评论列表" 200 "$code"
check "  他人视角 liked=false" false "$(nth_liked 1)"

echo "[5/8] 第二个人点赞 + 取消"
code=$(post "/api/comment/$COMMENT_ID/like" '{}' "$ALICE_TOKEN")
check "alice 也点赞" 200 "$code"
check "  计数累加" 2 "$(json_int like_count)"
code=$(del "/api/comment/$COMMENT_ID/like" "$BOB_TOKEN")
check "bob 取消点赞" 200 "$code"
check "  计数回落" 1 "$(json_int like_count)"
check "  状态 liked=false" false "$(json_field liked)"
code=$(del "/api/comment/$COMMENT_ID/like" "$BOB_TOKEN")
check "重复取消（幂等）" 200 "$code"
check "  计数仍为 1" 1 "$(json_int like_count)"

echo "[6/8] 二级回复同样可点赞"
code=$(post "/api/comment/$REPLY_ID/like" '{}' "$ALICE_TOKEN")
check "给回复点赞" 200 "$code"
check "  回复 like_count" 1 "$(json_int like_count)"
code=$(get "/api/comment/$VIDEO_ID" "$ALICE_TOKEN")
check "列表里回复 liked=true" true "$(nth_liked 2)"
check "  回复 like_count" 1 "$(nth_like_count 2)"
code=$(del "/api/comment/$REPLY_ID/like" "$ALICE_TOKEN")
check "取消回复点赞" 200 "$code"
check "  回复计数归零" 0 "$(json_int like_count)"

echo "[7/8] 错误路径"
code=$(post "/api/comment/99999999/like" '{}' "$BOB_TOKEN")
check "不存在的评论 → 404" 404 "$code"
code=$(post "/api/comment/$COMMENT_ID/like" '{}')
check "无 token → 401" 401 "$code"
code=$(get "/api/comment/$COMMENT_ID/like")
check "GET /like → 405" 405 "$code"

echo "[8/8] 结果"
if [[ "$FAILED" -eq 0 ]]; then
  echo "PASS：$PASSED 项全部通过"
  exit 0
fi
echo "FAIL：$PASSED 通过 / $FAILED 失败"
exit 1
