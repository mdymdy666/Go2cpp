#!/usr/bin/env bash
# 用户资料编辑验收（第 12 轮）：改名 / 重名 / 非法字符 / 简介超长 / 头像白名单 / 昵称同步。
#
# # 用例：
#   1) GET /api/user/me 返回本人资料（含 bio/avatar_url 空串）
#   2) PATCH 改名 + 简介 + 头像 → 200，GET 回读一致
#   3) 重名 409；1/21 字、含 @/空白 400
#   4) 简介 80 字 200、81 字 400
#   5) 头像白名单：javascript:/目录穿越 400、http(s)/videos 200
#   6) 未登录 PATCH 401；只改简介时昵称保持不变
#   7) 改名后通知里的 actor_name 跟着变（JOIN users）
#   8) 前端 /settings 页 + 首页头像入口 + 缓存头
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_profile_edit.pids；退出时按该
# 文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_profile_edit.pids"
DB="$ROOT/data/accept_profile_edit.db"
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
      continue
    fi
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.02
      done
      kill -9 "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      printf '[stop] %s pid=%s 已停止\n' "$name" "$pid"
    fi
  done <"$PID_FILE"
  : >"$PID_FILE"
}

cleanup() { stop_all; rm -rf "$TMP"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check() { # name expected actual [detail]
  local name="$1" expected="$2" actual="$3" detail="${4:-}"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-48s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

contains() { # name expected-substr file
  local name="$1" needle="$2" file="$3"
  if grep -qF -- "$needle" "$file"; then
    printf '  %-48s PASS  含 %s\n' "$name" "$needle"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  未找到 %s\n' "$name" "$needle"
    FAILED=$((FAILED + 1))
  fi
}

get() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
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

patch() { # path json [token]
  local path="$1" payload="$2" token="${3:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X PATCH
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

json_int() { grep -o "\"$1\":[0-9]*" "$TMP/body" | head -1 | cut -d: -f2; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
json_has() { grep -qF -- "\"$1\":\"$2\"" "$TMP/body" && echo yes || echo no; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_profile_edit_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_profile_edit_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

start_all() {
  rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_profile_edit_"*.log
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
    echo "[accept_profile_edit] 结果：$PASSED PASS / 0 FAIL"
    echo "[accept_profile_edit] PASS"
    exit 0
  fi
  echo "[accept_profile_edit] 结果：$PASSED PASS / $FAILED FAIL"
  echo "[accept_profile_edit] FAIL"
  exit 1
}


start_all() {
  rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_profile_edit_"*.log
  start_service user_service "$USER_PORT"
  start_service video_service "$VIDEO_PORT"
  start_service feed_service "$FEED_PORT"
  start_service interaction_service "$INTERACTION_PORT"
  start_service live_service "$LIVE_PORT"
  start_service search_service "$SEARCH_PORT"
  start_service gateway "$GW_PORT"
}

echo "[0/8] 启动 7 个进程"
start_all

echo "[1/8] 注册 alice / bob，alice 发视频，bob 评论"
code=$(post "/api/user/register" '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
ALICE_TOKEN="$(json_str token)"
ALICE_ID="$(json_int id)"
code=$(post "/api/user/register" '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
BOB_TOKEN="$(json_str token)"
BOB_ID="$(json_int id)"
if [[ -z "$ALICE_TOKEN" || -z "$BOB_TOKEN" ]]; then
  echo "取 token 失败：$(cat "$TMP/body")" >&2
  exit 1
fi
code=$(post "/api/video" '{"title":"资料编辑验收 #t","url":"/videos/1.mp4"}' "$ALICE_TOKEN")
check "alice 发布视频" 200 "$code"
VIDEO_ID="$(json_int id)"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"沙发\"}" "$BOB_TOKEN")
check "bob 评论（产生通知）" 200 "$code"

echo "[2/8] GET /api/user/me + 初始字段"
code=$(get "/api/user/me" "$ALICE_TOKEN")
check "GET /api/user/me" 200 "$code"
check "  昵称" alice "$(json_str name)"
check "  简介默认空" "" "$(json_str bio)"
check "  头像默认空" "" "$(json_str avatar_url)"
check "  未登录 → 401" 401 "$(get "/api/user/me")"

echo "[3/8] 改名 + 简介 + 头像"
code=$(patch "/api/user/me" '{"name":"alice_new","bio":"热爱生活，记录美好","avatar_url":"https://example.com/a.png"}' "$ALICE_TOKEN")
check "PATCH 三项" 200 "$code"
check "  响应昵称" alice_new "$(json_str name)"
check "  响应简介" "热爱生活，记录美好" "$(json_str bio)"
code=$(get "/api/user/me" "$ALICE_TOKEN")
check "回读昵称" alice_new "$(json_str name)"
check "回读简介" "热爱生活，记录美好" "$(json_str bio)"
code=$(get "/api/user/$ALICE_ID")
check "公开资料同步" 200 "$code"
check "  公开昵称" alice_new "$(json_str name)"

echo "[4/8] 重名与非法昵称"
code=$(patch "/api/user/me" '{"name":"alice_new"}' "$BOB_TOKEN")
check "bob 改成 alice_new → 409" 409 "$code"
code=$(patch "/api/user/me" '{"name":"alice_new"}' "$ALICE_TOKEN")
check "自己改成自己 → 200（排除自己）" 200 "$code"
code=$(patch "/api/user/me" '{"name":"a"}' "$ALICE_TOKEN")
check "1 字昵称 → 400" 400 "$code"
code=$(patch "/api/user/me" '{"name":"abcdefghijklmnopqrstu"}' "$ALICE_TOKEN")
check "21 字昵称 → 400" 400 "$code"
code=$(patch "/api/user/me" '{"name":"bad@name"}' "$ALICE_TOKEN")
check "含 @ → 400" 400 "$code"
code=$(patch "/api/user/me" '{"name":"bad name"}' "$ALICE_TOKEN")
check "含空格 → 400" 400 "$code"
code=$(patch "/api/user/me" '{"name":"中文昵称"}' "$ALICE_TOKEN")
check "2 个中文字 → 200" 200 "$code"

echo "[5/8] 简介边界"
BIO_80="$(printf 'b%.0s' $(seq 1 80))"
BIO_81="$(printf 'b%.0s' $(seq 1 81))"
code=$(patch "/api/user/me" "{\"bio\":\"$BIO_80\"}" "$ALICE_TOKEN")
check "简介 80 字 → 200" 200 "$code"
code=$(patch "/api/user/me" "{\"bio\":\"$BIO_81\"}" "$ALICE_TOKEN")
check "简介 81 字 → 400" 400 "$code"

echo "[6/8] 头像白名单"
code=$(patch "/api/user/me" '{"avatar_url":"javascript:alert(1)"}' "$ALICE_TOKEN")
check "javascript: → 400" 400 "$code"
code=$(patch "/api/user/me" '{"avatar_url":"videos/../../etc/passwd"}' "$ALICE_TOKEN")
check "目录穿越 → 400" 400 "$code"
code=$(patch "/api/user/me" '{"avatar_url":"videos/1.jpg"}' "$ALICE_TOKEN")
check "videos/1.jpg → 200" 200 "$code"
code=$(patch "/api/user/me" '{"avatar_url":"http://cdn.example.com/x.png"}' "$ALICE_TOKEN")
check "http(s) → 200" 200 "$code"
code=$(patch "/api/user/me" '{"avatar_url":"videos/evil.sh"}' "$ALICE_TOKEN")
check "非图片扩展名 → 400" 400 "$code"
code=$(patch "/api/user/me" '{}' "$ALICE_TOKEN")
check "空 body → 400" 400 "$code"
code=$(patch "/api/user/me" '{"name":"bob_new"}' "")
check "未登录 PATCH → 401" 401 "$code"

echo "[7/8] 改名后通知里的昵称同步（JOIN users）"
code=$(del "/api/user/me" "$ALICE_TOKEN")
check "DELETE /api/user/me → 405" 405 "$code"
code=$(patch "/api/user/me" '{"name":"bob_new"}' "$BOB_TOKEN")
check "bob 改名" 200 "$code"
code=$(get "/api/notifications" "$ALICE_TOKEN")
check "alice 读通知" 200 "$code"
check "  通知 actor_name 已更新" bob_new "$(json_str actor_name)"
code=$(get "/api/search/suggest?q=bob&limit=5")
check "搜索建议昵称同步" 200 "$code"
contains "  建议里有 bob_new" "bob_new" "$TMP/body"

echo "[8/8] 前端页面与缓存头"
code=$(get "/settings")
check "GET /settings" 200 "$code"
contains "  设置页含编辑资料" "编辑资料" "$TMP/body"
contains "  设置页调用 PATCH 接口" "api/user/me" "$TMP/body"
code=$(get "/")
check "GET / 首页" 200 "$code"
contains "  首页含头像入口" "whoAvatar" "$TMP/body"
cache_home="$(curl -sS -I --max-time 5 "$GW/" | tr -d '\r' | grep -i '^Cache-Control:' | head -1 | cut -d' ' -f2-)"
check "首页 Cache-Control" no-cache "$cache_home"
cache_settings="$(curl -sS -I --max-time 5 "$GW/settings" | tr -d '\r' | grep -i '^Cache-Control:' | head -1 | cut -d' ' -f2-)"
check "设置页 Cache-Control" no-cache "$cache_settings"
cache_video="$(curl -sS -I --max-time 5 "$GW/videos/1.mp4" | tr -d '\r' | grep -i '^Cache-Control:' | head -1 | cut -d' ' -f2-)"
check "视频静态资源 Cache-Control" "public, max-age=86400" "$cache_video"
cache_cover="$(curl -sS -I --max-time 5 "$GW/covers/1.jpg" | tr -d '\r' | grep -i '^Cache-Control:' | head -1 | cut -d' ' -f2-)"
check "/covers 缓存头" "public, max-age=86400" "$cache_cover"
cache_static="$(curl -sS -I --max-time 5 "$GW/static/robots.txt" | tr -d '\r' | grep -i '^Cache-Control:' | head -1 | cut -d' ' -f2-)"
check "/static 缓存头" "public, max-age=86400" "$cache_static"
ctype_static="$(curl -sS -I --max-time 5 "$GW/static/robots.txt" | tr -d '\r' | grep -i '^Content-Type:' | head -1 | cut -d' ' -f2-)"
check "/static Content-Type" "text/plain; charset=utf-8" "$ctype_static"

echo "[9/9] TTFB（阈值 200 ms）"
ttfb() { # path
  curl -sS -o /dev/null --max-time 5 -w '%{time_starttransfer}' "$GW$1"
}
for path in "/" "/settings" "/api/feed?limit=5" "/api/user/me"; do
  total=0
  for _ in $(seq 1 5); do
    v="$(ttfb "$path")"
    total=$(awk -v a="$total" -v b="$v" 'BEGIN { printf "%.6f", a + b }')
  done
  avg=$(awk -v a="$total" 'BEGIN { printf "%.4f", a / 5 }')
  ok=$(awk -v a="$avg" 'BEGIN { print (a < 0.2) ? "yes" : "no" }')
  check "TTFB $path < 200ms（5 次均值 ${avg}s）" yes "$ok"
done

finish
