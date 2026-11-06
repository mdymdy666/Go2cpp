#!/usr/bin/env bash
# 消息通知中心验收（第 2 轮新增）：启动 7 进程 → 评论/点赞/关注产生通知
# → 未读计数 → 标已读 → 分页游标 → 越权与鉴权 → 前端元素。
#
# 用例分组：
#   [1] 评论通知：bob 评论 alice 视频 → alice 未读 +1，actor_name=bob
#   [2] 自己评自己不通知
#   [3] 回复通知父评论作者（bob 收到 1 条，target_type=comment）
#   [4] 评论点赞通知 + 重复点赞幂等 + 自己赞自己不通知
#   [5] 视频点赞通知 + 重复点赞幂等 + 自己赞自己不通知
#   [6] 未读计数与「全部已读 / 按 id 标记已读」幂等
#   [7] 关注通知 + 重复关注不重复提醒
#   [8] 分页游标：limit=2 翻页覆盖全部、无重复、末页游标为空
#   [9] 越权：别人的通知不可见、不可标已读
#   [10] 鉴权：无 token / 伪造 token 401；方法不允许 405；页面元素
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_notifications.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_notifications.pids"
DB="$ROOT/data/accept_notifications.db"
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

get() { # path [token] [outfile]
  local path="$1" token="${2:-}" out="${3:-$TMP/body}"
  local args=(-s --max-time 5 -o "$out" -w '%{http_code}' "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { grep -o "\"$1\":[0-9]*" "$TMP/body" | head -1 | cut -d: -f2; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
items_count() { grep -o '"kind":"' "$TMP/body" | wc -l | tr -d ' '; }
item_ids() { grep -o '"id":[0-9]*' "$TMP/body" | cut -d: -f2 | tr '\n' ' '; }
first_id() { grep -o '"id":[0-9]*' "$TMP/body" | head -1 | cut -d: -f2; }
first_kind() { grep -o '"kind":"[a-z]*"' "$TMP/body" | head -1 | cut -d: -f2 | tr -d '"'; }
first_actor() { grep -o '"actor_name":"[^"]*"' "$TMP/body" | head -1 | cut -d: -f2 | tr -d '"'; }
first_target_type() { grep -o '"target_type":"[a-z]*"' "$TMP/body" | head -1 | cut -d: -f2 | tr -d '"'; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_notifications_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_notifications_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_notifications_"*.log

echo "[0/10] 启动 7 个进程"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

echo "[1/10] 注册 alice / bob / carol，alice 发视频"
code=$(post "/api/user/register" \
  '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
A_TOKEN="$(json_str token)"
A_UID="$(json_int id)"
code=$(post "/api/user/register" \
  '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
B_TOKEN="$(json_str token)"
B_UID="$(json_int id)"
code=$(post "/api/user/register" \
  '{"name":"carol","email":"carol@example.com","password":"secret123"}')
check "注册 carol" 200 "$code"
C_TOKEN="$(json_str token)"
C_UID="$(json_int id)"
if [[ -z "$A_TOKEN" || -z "$B_TOKEN" || -z "$C_TOKEN" ]]; then
  echo "取 token 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi
code=$(post "/api/video" '{"title":"通知验收视频","url":"/videos/demo1.mp4"}' "$A_TOKEN")
check "alice 发布视频" 200 "$code"
VIDEO_ID="$(json_int id)"
if [[ -z "$VIDEO_ID" ]]; then
  echo "取 video id 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi

echo "[2/10] 评论通知 + 自己评自己不通知"
code=$(post "/api/comment" \
  "{\"video_id\":$VIDEO_ID,\"content\":\"bob 的顶层评论\"}" "$B_TOKEN")
check "bob 评论 alice 的视频" 200 "$code"
BOB_COMMENT="$(json_int id)"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "alice 未读数（被评论）" 1 "$(json_int unread_count)"
code=$(get "/api/notifications?limit=20" "$A_TOKEN")
check "alice 通知列表 items 数" 1 "$(items_count)"
check "  类型 kind=comment" comment "$(first_kind)"
check "  目标 target_type=video" video "$(first_target_type)"
check "  触发者 actor_name=bob" bob "$(first_actor)"
check "  列表带回 unread_count" 1 "$(json_int unread_count)"
code=$(post "/api/comment" \
  "{\"video_id\":$VIDEO_ID,\"content\":\"作者自己评论\"}" "$A_TOKEN")
check "alice 评论自己的视频" 200 "$code"
ALICE_COMMENT="$(json_int id)"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "自己评自己不产生通知" 1 "$(json_int unread_count)"

echo "[3/10] 回复通知父评论作者"
code=$(post "/api/comment" \
  "{\"video_id\":$VIDEO_ID,\"content\":\"回复 bob\",\"parent_id\":$BOB_COMMENT}" \
  "$A_TOKEN")
check "alice 回复 bob 的评论" 200 "$code"
code=$(get "/api/notifications/unread_count" "$B_TOKEN")
check "bob 未读数（被回复）" 1 "$(json_int unread_count)"
code=$(get "/api/notifications?limit=20" "$B_TOKEN")
check "bob 列表 items 数" 1 "$(items_count)"
check "  类型 kind=comment" comment "$(first_kind)"
check "  目标 target_type=comment" comment "$(first_target_type)"
check "  触发者 actor_name=alice" alice "$(first_actor)"

echo "[4/10] 评论点赞通知 + 幂等 + 自己赞自己"
code=$(post "/api/comment/$ALICE_COMMENT/like" '{}' "$C_TOKEN")
check "carol 点赞 alice 的评论" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "alice 未读数（被赞评论）" 2 "$(json_int unread_count)"
code=$(post "/api/comment/$ALICE_COMMENT/like" '{}' "$C_TOKEN")
check "carol 重复点赞（幂等）" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "重复点赞不重复写通知" 2 "$(json_int unread_count)"
code=$(post "/api/comment/$ALICE_COMMENT/like" '{}' "$A_TOKEN")
check "alice 点赞自己的评论" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "自己赞自己不产生通知" 2 "$(json_int unread_count)"
code=$(get "/api/notifications?limit=20" "$A_TOKEN")
check "  最新一条 kind=like" like "$(first_kind)"
check "  最新一条 target_type=comment" comment "$(first_target_type)"
check "  触发者 actor_name=carol" carol "$(first_actor)"

echo "[5/10] 视频点赞通知 + 幂等 + 自己赞自己"
code=$(post "/api/like" "{\"video_id\":$VIDEO_ID}" "$B_TOKEN")
check "bob 点赞 alice 的视频" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "alice 未读数（被赞视频）" 3 "$(json_int unread_count)"
code=$(post "/api/like" "{\"video_id\":$VIDEO_ID}" "$B_TOKEN")
check "bob 重复点赞视频（幂等）" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "重复点赞视频不重复写通知" 3 "$(json_int unread_count)"
code=$(post "/api/like" "{\"video_id\":$VIDEO_ID}" "$A_TOKEN")
check "alice 点赞自己的视频" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "自己赞自己视频不产生通知" 3 "$(json_int unread_count)"
code=$(get "/api/notifications?limit=20" "$A_TOKEN")
check "  最新一条 target_type=video" video "$(first_target_type)"
check "  最新一条 kind=like" like "$(first_kind)"

echo "[6/10] 标记已读：按 id / 全部已读 / 幂等"
FIRST_NOTIF="$(first_id)"
code=$(post "/api/notifications/read" "{\"ids\":[$FIRST_NOTIF]}" "$A_TOKEN")
check "按 id 标记已读" 200 "$code"
check "  updated=1" 1 "$(json_int updated)"
check "  未读数减一" 2 "$(json_int unread_count)"
code=$(post "/api/notifications/read" "{\"ids\":[$FIRST_NOTIF]}" "$A_TOKEN")
check "重复标记同一条（幂等）" 200 "$code"
check "  updated=0" 0 "$(json_int updated)"
code=$(post "/api/notifications/read" '{"all":true}' "$A_TOKEN")
check "全部标记已读" 200 "$code"
check "  updated=2" 2 "$(json_int updated)"
check "  未读数归零" 0 "$(json_int unread_count)"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "再查未读数仍为 0" 0 "$(json_int unread_count)"
code=$(post "/api/notifications/read" '{"all":true}' "$A_TOKEN")
check "全部已读再点一次 updated=0" 0 "$(json_int updated)"

echo "[7/10] 关注通知 + 重复关注不重复提醒"
code=$(post "/api/user/$A_UID/follow" '{}' "$C_TOKEN")
check "carol 关注 alice" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "alice 未读数（新增关注）" 1 "$(json_int unread_count)"
code=$(post "/api/user/$A_UID/follow" '{}' "$C_TOKEN")
check "carol 重复关注（幂等）" 200 "$code"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "重复关注不重复写通知" 1 "$(json_int unread_count)"
code=$(get "/api/notifications?limit=20" "$A_TOKEN")
check "  最新一条 kind=follow" follow "$(first_kind)"
check "  最新一条 target_type=user" user "$(first_target_type)"
check "  触发者 actor_name=carol" carol "$(first_actor)"

echo "[8/10] 分页游标：limit=2 翻页"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"分页评论 1\"}" "$B_TOKEN")
check "bob 再评论 1" 200 "$code"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"分页评论 2\"}" "$B_TOKEN")
check "bob 再评论 2" 200 "$code"
code=$(get "/api/notifications?limit=20" "$A_TOKEN")
TOTAL="$(items_count)"
check "alice 通知总数" 6 "$TOTAL"
check "  未读总数（follow + 2 评论）" 3 "$(json_int unread_count)"
ALL_IDS="$(item_ids)"
PAGE_IDS=""
CURSOR=""
PAGES=0
while :; do
  if [[ -z "$CURSOR" ]]; then
    code=$(get "/api/notifications?limit=2" "$A_TOKEN")
  else
    code=$(get "/api/notifications?limit=2&cursor=$CURSOR" "$A_TOKEN")
  fi
  check "第 $((PAGES + 1)) 页状态码" 200 "$code"
  PAGE_IDS="$PAGE_IDS $(item_ids)"
  CURSOR="$(json_str next_cursor)"
  PAGES=$((PAGES + 1))
  [[ -z "$CURSOR" ]] && break
  [[ "$PAGES" -ge 10 ]] && break
done
check "分页页数（6 条 / 每页 2）" 3 "$PAGES"
check "分页无重复且覆盖全部" "$(printf '%s\n' $ALL_IDS | sort -u | wc -l | tr -d ' ')" \
  "$(printf '%s\n' $PAGE_IDS | sort -u | wc -l | tr -d ' ')"
check "分页 id 集合与首页一致" \
  "$(printf '%s\n' $ALL_IDS | sort | tr '\n' ',')" \
  "$(printf '%s\n' $PAGE_IDS | sort | tr '\n' ',')"
code=$(get "/api/notifications?limit=2&cursor=bad-cursor" "$A_TOKEN")
check "非法 cursor → 400" 400 "$code"
code=$(get "/api/notifications?limit=abc" "$A_TOKEN")
check "非法 limit 回落默认值不报错" 200 "$code"

echo "[9/10] 越权：别人的通知不可见、不可标已读"
code=$(get "/api/notifications?limit=20" "$B_TOKEN")
check "bob 列表能读到自己的通知" 200 "$code"
check "  bob items 数（1 条回复通知）" 1 "$(items_count)"
check "  bob 列表 user_id 只有自己" 1 \
  "$(grep -o '"user_id":[0-9]*' "$TMP/body" | sort -u | wc -l | tr -d ' ')"
check "  bob 列表 user_id = bob" "\"user_id\":$B_UID" \
  "$(grep -o '"user_id":[0-9]*' "$TMP/body" | sort -u | tr -d ' ')"
code=$(post "/api/notifications/read" "{\"ids\":[$FIRST_NOTIF]}" "$B_TOKEN")
check "bob 标记 alice 的通知" 200 "$code"
check "  越权 updated=0" 0 "$(json_int updated)"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "  alice 未读数不受影响" 3 "$(json_int unread_count)"
code=$(post "/api/notifications/read" '{}' "$A_TOKEN")
check "read 缺 ids/all → 400" 400 "$code"

echo "[10/11] 鉴权 / 方法 / 前端元素"
code=$(get "/api/notifications")
check "无 token 列表 → 401" 401 "$code"
code=$(get "/api/notifications/unread_count")
check "无 token 未读数 → 401" 401 "$code"
code=$(post "/api/notifications/read" '{"all":true}')
check "无 token 标已读 → 401" 401 "$code"
code=$(get "/api/notifications" "forged.invalid.token")
check "伪造 token 列表 → 401" 401 "$code"
code=$(get "/api/notifications/read" "$A_TOKEN")
check "GET /read → 405" 405 "$code"
code=$(post "/api/notifications" '{}' "$A_TOKEN")
check "POST /api/notifications → 405" 405 "$code"
code=$(curl -s --max-time 5 -o "$TMP/page" -w '%{http_code}' "$GW/")
check "GET / → 200" 200 "$code"
PAGE_SIZE="$(wc -c <"$TMP/page" | tr -d ' ')"
if [[ "$PAGE_SIZE" -gt 100000 ]]; then
  check "页面大小 > 100KB" yes yes
else
  check "页面大小 > 100KB" yes "$PAGE_SIZE bytes"
fi
for pat in notifBtn notifBadge notifPanel openNotifications renderNotifications notif-line; do
  if grep -q "$pat" "$TMP/page"; then
    check "页面含 $pat" yes yes
  else
    check "页面含 $pat" yes no
  fi
done

echo "[11/11] 性能：查询计划命中索引 + TTFB"
PLAN="$(python3 - "$DB" <<'PY'
import sqlite3
import sys

con = sqlite3.connect(sys.argv[1])
con.execute("PRAGMA query_only=1")
sqls = {
    "unread": "SELECT COUNT(*) FROM notifications WHERE user_id = 1 AND read_at = 0",
    "list": (
        "SELECT n.id, u.name FROM notifications n LEFT JOIN users u"
        " ON u.id = n.actor_id WHERE n.user_id = 1"
        " ORDER BY n.created_at DESC, n.id DESC LIMIT 21"
    ),
}
for name, sql in sqls.items():
    plan = " ".join(str(row[-1]) for row in con.execute("EXPLAIN QUERY PLAN " + sql))
    print(name + "=" + plan)
PY
)"
if printf '%s\n' "$PLAN" | grep -q 'unread=.*idx_notifications_unread'; then
  check "未读计数命中 idx_notifications_unread" yes yes
else
  check "未读计数命中 idx_notifications_unread" yes "$PLAN"
fi
if printf '%s\n' "$PLAN" | grep -q 'list=.*idx_notifications_timeline'; then
  check "通知列表命中 idx_notifications_timeline" yes yes
else
  check "通知列表命中 idx_notifications_timeline" yes "$PLAN"
fi
if printf '%s\n' "$PLAN" | grep -qi 'TEMP B-TREE'; then
  check "通知列表无临时排序（TEMP B-TREE）" no yes
else
  check "通知列表无临时排序（TEMP B-TREE）" no no
fi

# 预热 3 次（SQLite 语句缓存/页缓存），再串行测 20 次；单核低负载下
# 取平均与最大值，需求 TTFB < 200 ms。
for _ in $(seq 1 3); do
  curl -s -o /dev/null -H "Authorization: Bearer $A_TOKEN" \
    "$GW/api/notifications?limit=20" >/dev/null
done
TTFB_SUM=0
TTFB_MAX=0
for _ in $(seq 1 20); do
  t="$(curl -s -o /dev/null -w '%{time_starttransfer}' \
    -H "Authorization: Bearer $A_TOKEN" "$GW/api/notifications?limit=20")"
  ms="$(awk "BEGIN{printf \"%d\", $t * 1000}")"
  TTFB_SUM=$((TTFB_SUM + ms))
  if [[ "$ms" -gt "$TTFB_MAX" ]]; then TTFB_MAX="$ms"; fi
done
TTFB_AVG=$((TTFB_SUM / 20))
if [[ "$TTFB_AVG" -lt 200 ]]; then
  check "通知列表 TTFB 平均 < 200ms" yes yes
else
  check "通知列表 TTFB 平均 < 200ms" yes "${TTFB_AVG}ms"
fi
if [[ "$TTFB_MAX" -lt 200 ]]; then
  check "通知列表 TTFB 最大 < 200ms" yes yes
else
  check "通知列表 TTFB 最大 < 200ms" yes "${TTFB_MAX}ms"
fi
printf '  （TTFB 平均 %dms / 最大 %dms，20 次串行）\n' "$TTFB_AVG" "$TTFB_MAX"

echo "---------------------------------------------"
printf '[accept_notifications] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_notifications] PASS"
  exit 0
fi
echo "[accept_notifications] FAIL"
exit 1
