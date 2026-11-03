#!/usr/bin/env bash
# 抖音 C5/C6 验收：interaction(29104) + search(29106)，与 user/video 共用 SQLite。
#
# 覆盖：
#   1) 注册 alice/bob + alice 发视频
#   2) 点赞幂等（两次 → like_count 只 +1，且 video 详情同步）
#   3) 取消赞幂等（两次 → like_count 0）
#   4) 评论 2 条 + 列表倒序 + 条数
#   5) 边界：空评论 400、501 字 400、不存在视频 404、无 token 401、
#      非法 cursor/limit、type 非法
#   6) 搜索：5 用户/5 视频，前缀/包含/大小写、type=user 只回用户、
#      type=live 空、空 q 400、limit 分页
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
VIDEO_PORT="${DOUYIN_VIDEO_PORT:-29102}"
INTER_PORT="${DOUYIN_INTERACTION_PORT:-29104}"
SEARCH_PORT="${DOUYIN_SEARCH_PORT:-29106}"
USER_BASE="http://127.0.0.1:$USER_PORT"
VIDEO_BASE="http://127.0.0.1:$VIDEO_PORT"
INTER_BASE="http://127.0.0.1:$INTER_PORT"
SEARCH_BASE="http://127.0.0.1:$SEARCH_PORT"
DB="$ROOT/data/accept_interaction_search.db"
LOG_DIR="$ROOT/logs"
TMP="$(mktemp -d)"
PIDS=()
PASSED=0
FAILED=0

cleanup() {
  for pid in "${PIDS[@]:-}"; do
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
    printf '  %-40s PASS  HTTP %s %s\n' "$name" "$actual" "$detail"
    PASSED=$((PASSED + 1))
  else
    printf '  %-40s FAIL  HTTP %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-40s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-40s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-40s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-40s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

post() { # base path json [token]
  local base="$1" path="$2" payload="$3" token="${4:-}"
  local args=(-s -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$payload" "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

del() { # base path json [token]
  local base="$1" path="$2" payload="$3" token="${4:-}"
  local args=(-s -o "$TMP/body" -w '%{http_code}' -X DELETE
              -H 'Content-Type: application/json' -d "$payload" "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

get() { # base path [token]
  local base="$1" path="$2" token="${3:-}"
  local args=(-s -o "$TMP/body" -w '%{http_code}' "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p" "$TMP/body" | head -1; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }

start_service() { # bin port base name
  local bin="$1" port="$2" base="$3" name="$4"
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_interaction_search_$name.log" 2>&1 &
  PIDS+=("$!")
  local ready=0
  for _ in $(seq 1 200); do
    if curl -fsS "$base/health" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "$name 启动失败，日志：" >&2
    cat "$LOG_DIR/accept_interaction_search_$name.log" >&2
    exit 1
  fi
  printf '[start] %s %s（db=%s pid=%s）\n' "$name" "$base" "$DB" "$!"
}

register() { # name email -> sets TOKEN
  post "$USER_BASE" /api/user/register \
    "{\"name\":\"$1\",\"email\":\"$2\",\"password\":\"secret123\"}" >/dev/null
  TOKEN=$(json_str token)
}

publish() { # title [token] -> sets VIDEO_ID PUB_CODE
  local token="${2:-$TOKEN_A}"
  PUB_CODE=$(post "$VIDEO_BASE" /api/video \
    "{\"title\":\"$1\",\"url\":\"http://example.com/$1.mp4\"}" "$token")
  VIDEO_ID=$(json_int id)
}

rm -f "$DB" "$DB-wal" "$DB-shm"
mkdir -p "$LOG_DIR" "$TMP" "$ROOT/data"

echo "[0/6] 启动服务"
start_service "$BIN_DIR/user_service"        "$USER_PORT"  "$USER_BASE"  user
start_service "$BIN_DIR/video_service"       "$VIDEO_PORT" "$VIDEO_BASE" video
start_service "$BIN_DIR/interaction_service" "$INTER_PORT" "$INTER_BASE" interaction
start_service "$BIN_DIR/search_service"      "$SEARCH_PORT" "$SEARCH_BASE" search

echo "[1/6] 注册 + 发视频"
code=$(post "$USER_BASE" /api/user/register \
  '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
TOKEN_A=$(json_str token)
expect_eq "alice token 非空" "yes" "$([[ -n $TOKEN_A ]] && echo yes || echo no)"
code=$(post "$USER_BASE" /api/user/register \
  '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
TOKEN_B=$(json_str token)
publish "alice-v1"
V1="$VIDEO_ID"
check "发布 alice-v1" 200 "$PUB_CODE"
expect_eq "视频 id 有效" "yes" "$([[ -n $V1 && $V1 -gt 0 ]] && echo yes || echo no)"

echo "[2/6] 点赞幂等"
code=$(post "$INTER_BASE" /api/like "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 首次点赞" 200 "$code"
expect_body "首次点赞 inserted=true" '"inserted":true'
expect_eq "首次点赞 like_count" "1" "$(json_int like_count)"

code=$(post "$INTER_BASE" /api/like "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 重复点赞" 200 "$code"
expect_body "重复点赞 inserted=false" '"inserted":false'
expect_eq "重复点赞 like_count 不变" "1" "$(json_int like_count)"

code=$(get "$VIDEO_BASE" "/api/video/$V1")
check "视频详情" 200 "$code"
expect_eq "详情 like_count 同步" "1" "$(json_int like_count)"

code=$(post "$INTER_BASE" /api/like "{\"video_id\":$V1}" "$TOKEN_A")
check "alice 点赞（第二人）" 200 "$code"
expect_eq "两人点赞 like_count" "2" "$(json_int like_count)"

echo "[3/6] 取消赞幂等"
code=$(del "$INTER_BASE" /api/like "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 取消赞" 200 "$code"
expect_eq "取消后 like_count" "1" "$(json_int like_count)"
code=$(del "$INTER_BASE" /api/like "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 重复取消" 200 "$code"
expect_eq "重复取消 like_count 不变" "1" "$(json_int like_count)"
code=$(get "$VIDEO_BASE" "/api/video/$V1")
expect_eq "详情 like_count 同步（取消后）" "1" "$(json_int like_count)"

echo "[4/6] 评论 + 列表"
code=$(post "$INTER_BASE" /api/comment \
  "{\"video_id\":$V1,\"content\":\"第一条评论\"}" "$TOKEN_B")
check "bob 发第 1 条评论" 200 "$code"
C1=$(json_int id)
expect_body "评论返回 content" '第一条评论'
code=$(post "$INTER_BASE" /api/comment \
  "{\"video_id\":$V1,\"content\":\"second comment\"}" "$TOKEN_A")
check "alice 发第 2 条评论" 200 "$code"
C2=$(json_int id)
expect_eq "两条评论 id 不同" "yes" "$([[ "$C1" != "$C2" ]] && echo yes || echo no)"

code=$(get "$VIDEO_BASE" "/api/video/$V1")
check "视频详情（评论后）" 200 "$code"
expect_eq "详情 comment_count" "2" "$(json_int comment_count)"

code=$(get "$INTER_BASE" "/api/comment/$V1")
check "评论列表" 200 "$code"
expect_eq "评论条数" "2" "$(grep -o '"comment":{' "$TMP/body" | wc -l)"
FIRST_ID=$(grep -o '"comment":{"id":[0-9]*' "$TMP/body" | head -1 | grep -o '[0-9]*$')
expect_eq "列表倒序（最新在前）" "$C2" "$FIRST_ID"

code=$(get "$INTER_BASE" "/api/comment/$V1?limit=1")
check "评论分页 limit=1" 200 "$code"
expect_eq "第一页 1 条" "1" "$(grep -o '"comment":{' "$TMP/body" | wc -l)"
CURSOR=$(json_str next_cursor)
expect_eq "next_cursor 非空" "yes" "$([[ -n $CURSOR ]] && echo yes || echo no)"
code=$(get "$INTER_BASE" "/api/comment/$V1?limit=1&cursor=$CURSOR")
check "评论第二页" 200 "$code"
SECOND_ID=$(grep -o '"comment":{"id":[0-9]*' "$TMP/body" | head -1 | grep -o '[0-9]*$')
expect_eq "第二页 id" "$C1" "$SECOND_ID"
expect_eq "next_cursor 为空" "" "$(json_str next_cursor)"

echo "[5/6] 边界"
code=$(post "$INTER_BASE" /api/comment "{\"video_id\":$V1,\"content\":\"\"}" "$TOKEN_B")
check "空评论" 400 "$code"
LONG=$(printf 'x%.0s' $(seq 1 501))
code=$(post "$INTER_BASE" /api/comment \
  "{\"video_id\":$V1,\"content\":\"$LONG\"}" "$TOKEN_B")
check "501 字评论" 400 "$code"
code=$(post "$INTER_BASE" /api/comment \
  '{"video_id":999999,"content":"x"}' "$TOKEN_B")
check "不存在视频评论" 404 "$code"
code=$(get "$INTER_BASE" "/api/comment/999999")
check "不存在视频评论列表" 404 "$code"
code=$(post "$INTER_BASE" /api/like "{\"video_id\":$V1}")
check "点赞无 token" 401 "$code"
code=$(del "$INTER_BASE" /api/like "{\"video_id\":$V1}")
check "取消赞无 token" 401 "$code"
code=$(post "$INTER_BASE" /api/like 'not-json' "$TOKEN_B")
check "点赞缺 video_id" 400 "$code"
code=$(get "$INTER_BASE" "/api/comment/$V1?cursor=bad")
check "评论非法 cursor" 400 "$code"
code=$(get "$INTER_BASE" "/api/comment/$V1?limit=0")
check "评论 limit=0 用默认" 200 "$code"

echo "[6/6] 搜索"
# 造 5 个用户（名字前缀 searchuser）+ 5 个视频（标题 GoLang Tutorial）。
for i in $(seq 1 5); do
  code=$(post "$USER_BASE" /api/user/register \
    "{\"name\":\"searchuser$i\",\"email\":\"search$i@example.com\",\"password\":\"secret123\"}")
  check "注册 searchuser$i" 200 "$code"
  TOKEN_S=$(json_str token)
  publish "GoLang Tutorial $i" "$TOKEN_S"
  check "发布 GoLang Tutorial $i" 200 "$PUB_CODE"
done

code=$(get "$SEARCH_BASE" "/api/search?q=searchuser&type=user&limit=100")
check "搜索用户（前缀）" 200 "$code"
expect_eq "前缀命中 5 用户" "5" "$(grep -o '"type":"user"' "$TMP/body" | wc -l)"
code=$(get "$SEARCH_BASE" "/api/search?q=user3&type=user")
check "搜索用户（包含）" 200 "$code"
expect_eq "包含命中 1 用户" "1" "$(grep -o '"type":"user"' "$TMP/body" | wc -l)"
expect_body "包含命中 searchuser3" 'searchuser3'
code=$(get "$SEARCH_BASE" "/api/search?q=SEARCHUSER&type=user&limit=100")
check "搜索用户（大小写）" 200 "$code"
expect_eq "大小写命中 5 用户" "5" "$(grep -o '"type":"user"' "$TMP/body" | wc -l)"
code=$(get "$SEARCH_BASE" "/api/search?q=golang&type=video&limit=100")
check "搜索视频（大小写）" 200 "$code"
expect_eq "视频命中 5" "5" "$(grep -o '"type":"video"' "$TMP/body" | wc -l)"
code=$(get "$SEARCH_BASE" "/api/search?q=LANG&type=video&limit=100")
check "搜索视频（包含）" 200 "$code"
expect_eq "包含命中 5 视频" "5" "$(grep -o '"type":"video"' "$TMP/body" | wc -l)"
code=$(get "$SEARCH_BASE" "/api/search?q=searchuser&type=user&limit=2")
check "搜索分页 limit=2" 200 "$code"
expect_eq "第一页 2 条" "2" "$(grep -o '"type":"user"' "$TMP/body" | wc -l)"
CURSOR=$(json_str next_cursor)
expect_eq "搜索 next_cursor 非空" "yes" "$([[ -n $CURSOR ]] && echo yes || echo no)"
code=$(get "$SEARCH_BASE" "/api/search?q=searchuser&type=user&limit=2&cursor=$CURSOR")
check "搜索第二页" 200 "$code"
expect_eq "第二页 2 条" "2" "$(grep -o '"type":"user"' "$TMP/body" | wc -l)"
code=$(get "$SEARCH_BASE" "/api/search?q=searchuser&type=all&limit=100")
check "综合搜索" 200 "$code"
expect_eq "综合含 5 用户" "5" "$(grep -o '"type":"user"' "$TMP/body" | wc -l)"
code=$(get "$SEARCH_BASE" "/api/search?q=GoLang&type=live")
check "type=live" 200 "$code"
expect_eq "live 为空" "0" "$(grep -o '"id":' "$TMP/body" | wc -l)"
code=$(get "$SEARCH_BASE" "/api/search?q=&type=all")
check "空 q" 400 "$code"
code=$(get "$SEARCH_BASE" "/api/search?q=%20&type=all")
check "空白 q" 400 "$code"
code=$(get "$SEARCH_BASE" "/api/search?q=x&type=bad")
check "非法 type" 400 "$code"
code=$(get "$SEARCH_BASE" "/api/search?q=searchuser&cursor=bad")
check "非法 cursor" 400 "$code"
code=$(get "$SEARCH_BASE" "/api/search?q=searchuser&limit=1000")
check "limit=1000 夹紧" 200 "$code"
expect_eq "limit=1000 最多 100" "5" "$(grep -o '"type":"user"' "$TMP/body" | wc -l)"

echo "---------------------------------------------"
printf '[accept_interaction_search] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_interaction_search] PASS"
  exit 0
fi
echo "[accept_interaction_search] FAIL"
exit 1
