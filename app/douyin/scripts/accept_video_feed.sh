#!/usr/bin/env bash
# 抖音 C3/C4 验收：user(29101) + video(29102) + feed(29103) 三服务共用一个 SQLite。
#
# 覆盖：
#   1) 注册 alice/bob → alice 发 3 条视频
#   2) 视频详情（author / like_count / comment_count）
#   3) 我的视频（created_at 倒序分页）
#   4) 删除权限（非作者 403 / 不存在 404 / 作者 200 / 删后 404）
#   5) feed 分页（limit=1 + cursor 翻页不重不漏）
#   6) 关注流（只含已关注作者；无 token 401）
#   7) 边界（缺 title/url 400、无 token 401、非法 cursor 400）
#   8) limit 夹紧（造 110 条后 limit=1000 → 100、limit=0 → 默认 20）
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
VIDEO_PORT="${DOUYIN_VIDEO_PORT:-29102}"
FEED_PORT="${DOUYIN_FEED_PORT:-29103}"
USER_BASE="http://127.0.0.1:$USER_PORT"
VIDEO_BASE="http://127.0.0.1:$VIDEO_PORT"
FEED_BASE="http://127.0.0.1:$FEED_PORT"
DB="$ROOT/data/accept_video_feed.db"
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
    printf '  %-38s PASS  HTTP %s %s\n' "$name" "$actual" "$detail"
    PASSED=$((PASSED + 1))
  else
    printf '  %-38s FAIL  HTTP %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-38s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-38s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-38s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-38s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
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

get() { # base path [token]
  local base="$1" path="$2" token="${3:-}"
  local args=(-s -o "$TMP/body" -w '%{http_code}' "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

del() { # base path token
  curl -s -o "$TMP/body" -w '%{http_code}' -X DELETE \
       -H "Authorization: Bearer $3" "$1$2"
}

count_items() { grep -o '"video":{"id"' "$TMP/body" | wc -l; }
item_ids() { grep -o '"video":{"id":[0-9]*' "$TMP/body" | grep -o '[0-9]*$'; }

start_service() { # bin port base name
  local bin="$1" port="$2" base="$3" name="$4"
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_video_feed_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_video_feed_$name.log" >&2
    exit 1
  fi
  printf '[start] %s %s（db=%s pid=%s）\n' "$name" "$base" "$DB" "$!"
}

mkdir -p "$ROOT/data" "$LOG_DIR"
rm -f "$DB" "$DB-wal" "$DB-shm"

for bin in user_service video_service feed_service; do
  if [[ ! -x "$BIN_DIR/$bin" ]]; then
    echo "找不到 $BIN_DIR/$bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
done

start_service "$BIN_DIR/user_service"  "$USER_PORT"  "$USER_BASE"  user_service
start_service "$BIN_DIR/video_service" "$VIDEO_PORT" "$VIDEO_BASE" video_service
start_service "$BIN_DIR/feed_service"  "$FEED_PORT"  "$FEED_BASE"  feed_service

echo "[1/8] 注册 alice / bob"
code=$(post "$USER_BASE" /api/user/register \
  '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
TOKEN_A=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body")
ID_A=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
code=$(post "$USER_BASE" /api/user/register \
  '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
TOKEN_B=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body")
ID_B=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
[[ -n "$TOKEN_A" && -n "$TOKEN_B" && -n "$ID_A" && -n "$ID_B" ]] || {
  echo "token/user id 提取失败" >&2
  exit 1
}

echo "[2/8] alice 发 3 条视频"
V1=""; V2=""; V3=""
for i in 1 2 3; do
  code=$(post "$VIDEO_BASE" /api/video \
    "{\"title\":\"v$i\",\"url\":\"http://example.com/$i.mp4\"}" "$TOKEN_A")
  check "发视频 v$i" 200 "$code"
  expect_body "v$i 响应含 ok/video" '"ok":true,"video":'
  eval "V$i=\$(sed -n 's/.*\"video\":{\"id\":\\([0-9]*\\).*/\\1/p' \"\$TMP/body\")"
done
[[ -n "$V1" && -n "$V2" && -n "$V3" ]] || {
  echo "视频 id 提取失败（V1=$V1 V2=$V2 V3=$V3）" >&2
  exit 1
}

echo "[3/8] 详情 / 我的视频"
code=$(get "$VIDEO_BASE" "/api/video/$V1")
check "详情 v1" 200 "$code"
expect_body "详情含 author alice" '"author":"alice"'
expect_body "详情含 title v1" '"title":"v1"'
expect_body "详情含 like_count 0" '"like_count":0'
expect_body "详情含 comment_count 0" '"comment_count":0'

code=$(get "$VIDEO_BASE" "/api/video/mine" "$TOKEN_A")
check "我的视频" 200 "$code"
expect_eq "我的视频条数" 3 "$(count_items)"
MINE_IDS=$(item_ids | paste -sd, -)
expect_eq "我的视频按 id 倒序" "$V3,$V2,$V1" "$MINE_IDS"
code=$(get "$VIDEO_BASE" "/api/video/mine?limit=1" "$TOKEN_A")
expect_eq "我的视频 limit=1" 1 "$(count_items)"
code=$(get "$VIDEO_BASE" "/api/video/mine?limit=1&cursor=abc" "$TOKEN_A")
check "我的视频非法 cursor" 400 "$code"
code=$(get "$VIDEO_BASE" "/api/video/mine")
check "我的视频无 token" 401 "$code"

echo "[4/8] 删除权限"
code=$(del "$VIDEO_BASE" "/api/video/$V1" "$TOKEN_B")
check "bob 删 alice 视频" 403 "$code"
expect_body "403 响应含 code" '"code":403'
code=$(del "$VIDEO_BASE" "/api/video/999999" "$TOKEN_A")
check "删不存在视频" 404 "$code"
code=$(del "$VIDEO_BASE" "/api/video/$V1" "$TOKEN_A")
check "alice 删自己视频" 200 "$code"
expect_body "删除响应含 deleted" '"deleted":true'
code=$(get "$VIDEO_BASE" "/api/video/$V1")
check "删后详情" 404 "$code"
code=$(get "$VIDEO_BASE" "/api/video/mine" "$TOKEN_A")
expect_eq "删后我的视频条数" 2 "$(count_items)"

echo "[5/8] 边界"
code=$(post "$VIDEO_BASE" /api/video '{"title":"no-url"}' "$TOKEN_A")
check "缺 url" 400 "$code"
code=$(post "$VIDEO_BASE" /api/video '{"url":"http://example.com/x.mp4"}' "$TOKEN_A")
check "缺 title" 400 "$code"
code=$(post "$VIDEO_BASE" /api/video '{"title":"t","url":"u"}')
check "发视频无 token" 401 "$code"
code=$(get "$VIDEO_BASE" "/api/video/999999")
check "不存在详情" 404 "$code"
code=$(get "$VIDEO_BASE" "/api/video/not-a-number")
check "非法 id 路径" 404 "$code"
code=$(get "$FEED_BASE" "/api/feed?cursor=bad-cursor")
check "feed 非法 cursor" 400 "$code"

echo "[6/8] 关注流"
code=$(post "$VIDEO_BASE" /api/video \
  '{"title":"b1","url":"http://example.com/b1.mp4"}' "$TOKEN_B")
check "bob 发视频 b1" 200 "$code"
B1=$(sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
code=$(post "$VIDEO_BASE" /api/video \
  '{"title":"b2","url":"http://example.com/b2.mp4"}' "$TOKEN_B")
check "bob 发视频 b2" 200 "$code"
B2=$(sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body")

code=$(get "$FEED_BASE" "/api/feed/following" "$TOKEN_A")
check "关注流（未关注任何作者）" 200 "$code"
expect_eq "未关注时条数" 0 "$(count_items)"

code=$(post "$USER_BASE" "/api/user/$ID_B/follow" '{}' "$TOKEN_A")
check "alice 关注 bob" 200 "$code"
code=$(get "$FEED_BASE" "/api/feed/following" "$TOKEN_A")
check "关注流（已关注 bob）" 200 "$code"
expect_eq "关注流条数" 2 "$(count_items)"
if grep -q '"author":"alice"' "$TMP/body"; then
  printf '  %-38s FAIL  关注流混入 alice 视频\n' "关注流作者过滤"
  FAILED=$((FAILED + 1))
else
  printf '  %-38s PASS  关注流只含 bob\n' "关注流作者过滤"
  PASSED=$((PASSED + 1))
fi
FOLLOW_IDS=$(item_ids | paste -sd, -)
expect_eq "关注流 id 集合" "$B2,$B1" "$FOLLOW_IDS"
code=$(get "$FEED_BASE" "/api/feed/following")
check "关注流无 token" 401 "$code"

echo "[7/8] feed 分页（limit=1 翻页）"
CURSOR=""
PAGE_IDS=()
PAGES=0
while :; do
  if [[ -z "$CURSOR" ]]; then
    code=$(get "$FEED_BASE" "/api/feed?limit=1")
  else
    code=$(get "$FEED_BASE" "/api/feed?limit=1&cursor=$CURSOR")
  fi
  if [[ "$code" != "200" ]]; then
    printf '  %-38s FAIL  第 %d 页 HTTP %s\n' "feed 翻页" "$((PAGES + 1))" "$code"
    FAILED=$((FAILED + 1))
    break
  fi
  ID=$(item_ids | head -1)
  PAGE_IDS+=("$ID")
  PAGES=$((PAGES + 1))
  CURSOR=$(sed -n 's/.*"next_cursor":"\([^"]*\)".*/\1/p' "$TMP/body")
  [[ -z "$CURSOR" ]] && break
  [[ "$PAGES" -gt 20 ]] && break
done
EXPECT_TOTAL=4  # V2 V3 B1 B2（V1 已删）
TOTAL=${#PAGE_IDS[@]}
UNIQ=$(printf '%s\n' "${PAGE_IDS[@]}" | sort -u | wc -l)
expect_eq "分页总条数" "$EXPECT_TOTAL" "$TOTAL"
expect_eq "分页无重复" "$EXPECT_TOTAL" "$UNIQ"
if printf '%s\n' "${PAGE_IDS[@]}" | grep -qx "$B2" &&
   printf '%s\n' "${PAGE_IDS[@]}" | grep -qx "$B1" &&
   printf '%s\n' "${PAGE_IDS[@]}" | grep -qx "$V2" &&
   printf '%s\n' "${PAGE_IDS[@]}" | grep -qx "$V3"; then
  printf '  %-38s PASS  含 V2/V3/B1/B2\n' "分页覆盖"
  PASSED=$((PASSED + 1))
else
  printf '  %-38s FAIL  页内 id 缺失（%s）\n' "分页覆盖" "${PAGE_IDS[*]}"
  FAILED=$((FAILED + 1))
fi

echo "[8/8] limit 夹紧（造 110 条）"
BULK_FAIL=0
for i in $(seq 1 110); do
  code=$(post "$VIDEO_BASE" /api/video \
    "{\"title\":\"bulk-$i\",\"url\":\"http://example.com/bulk-$i.mp4\"}" "$TOKEN_A")
  [[ "$code" == "200" ]] || BULK_FAIL=$((BULK_FAIL + 1))
done
expect_eq "批量发布失败数" 0 "$BULK_FAIL"
code=$(get "$FEED_BASE" "/api/feed?limit=1000")
check "limit=1000" 200 "$code"
expect_eq "limit=1000 夹紧到 100" 100 "$(count_items)"
code=$(get "$FEED_BASE" "/api/feed?limit=0")
check "limit=0" 200 "$code"
expect_eq "limit=0 用默认 20" 20 "$(count_items)"
code=$(get "$FEED_BASE" "/api/feed?limit=abc")
check "limit=abc" 200 "$code"
expect_eq "limit=abc 用默认 20" 20 "$(count_items)"

echo "---------------------------------------------"
printf '[accept_video_feed] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_video_feed] PASS"
  exit 0
fi
echo "[accept_video_feed] FAIL"
exit 1
