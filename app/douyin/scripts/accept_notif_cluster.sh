#!/usr/bin/env bash
# 消息三件套验收（第 9 轮新增）：通知分类 tab + 系统通知 + 单类已读。
#
# 用例分组：
#   [1]  准备：注册 alice(admin,id=1) / bob / carol，alice 发视频
#   [2]  构造 7 条未读：2 评论 + 1 mention + 1 like + 1 follow + 2 system
#   [3]  分类过滤：kind=like/comment/follow/mention/system 各自只返回本类
#   [4]  counts：一次 GROUP BY 出 6 类，数值与列表一致；unread_count 兼容
#   [5]  系统通知权限：无凭证 401 / 普通用户 403 / 错误内部 token 401
#   [6]  系统通知写入：内部 token 201、admin 201、content 回显、actor_id
#   [7]  系统通知校验：接收者 404 / content 空 400 / target_type 非法 400
#   [8]  单类已读：kind=like 只清 like；kind=system 只清 system；越权不影响
#   [9]  全部已读：all=true 清空，counts 全 0
#   [10] 分页：kind 过滤 + cursor 不重不漏；非法 kind 400
#   [11] 鉴权 / 方法 / 前端元素（6 tab + 📢 + counts）
#   [12] 性能：counts 查询计划无临时 B 树 + TTFB < 200ms
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_notif_cluster.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_notif_cluster.pids"
DB="$ROOT/data/accept_notif_cluster.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0
INTERNAL_TOKEN="notif-cluster-internal-token"

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

post_internal() { # path json
  local path="$1" payload="$2"
  curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' -H "X-Internal-Token: $INTERNAL_TOKEN" \
    -d "$payload" "$GW$path"
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
first_kind() { grep -o '"kind":"[a-z]*"' "$TMP/body" | head -1 | cut -d: -f2 | tr -d '"'; }
kinds_all() { grep -o '"kind":"[a-z]*"' "$TMP/body" | cut -d: -f2 | tr -d '"' | sort -u | tr '\n' ',' | sed 's/,$//' ; }
count_of() { grep -o "\"$1\":[0-9]*" "$TMP/body" | head -1 | cut -d: -f2; }
items_read() { grep -o '"read_at":[0-9]*' "$TMP/body" | cut -d: -f2 | tr '\n' ' '; }
all_read() { # 所有 read_at 均 > 0 时输出 yes
  local vals n
  vals=$(items_read)
  n=$(printf '%s\n' $vals | awk '$1==0{c++} END{print c+0}')
  [[ "$n" -eq 0 ]] && echo yes || echo no
}
unread_items() { grep -o '"read_at":0' "$TMP/body" | wc -l | tr -d ' '; }
occurrences() { grep -o "$1" "$TMP/body" | wc -l | tr -d ' '; }

start_service() { # name port [extra_env]
  local name="$1" port="$2" extra_env="${3:-}"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  if [[ -n "$extra_env" ]]; then
    env DOUYIN_DB="$DB" "$extra_env" "$bin" "$port" \
      >"$LOG_DIR/accept_notif_cluster_$name.log" 2>&1 &
  else
    DOUYIN_DB="$DB" "$bin" "$port" \
      >"$LOG_DIR/accept_notif_cluster_$name.log" 2>&1 &
  fi
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
    cat "$LOG_DIR/accept_notif_cluster_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_notif_cluster_"*.log

echo "[1/12] 启动 7 个进程（interaction 带内部 token）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT" \
  "DOUYIN_INTERNAL_TOKEN=$INTERNAL_TOKEN"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

echo "[2/12] 注册 alice / bob / carol，alice 发视频并造 7 条未读"
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
check "alice 是默认管理员（id=1）" 1 "$A_UID"
if [[ -z "$A_TOKEN" || -z "$B_TOKEN" || -z "$C_TOKEN" ]]; then
  echo "取 token 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi
code=$(post "/api/video" '{"title":"通知分类验收视频","url":"/videos/demo1.mp4"}' "$A_TOKEN")
check "alice 发布视频" 200 "$code"
VIDEO_ID="$(json_int id)"
code=$(post "/api/comment" \
  "{\"video_id\":$VIDEO_ID,\"content\":\"bob 的普通评论\"}" "$B_TOKEN")
check "bob 评论 alice 视频" 200 "$code"
code=$(post "/api/comment" \
  "{\"video_id\":$VIDEO_ID,\"content\":\"@alice 这条会同时产生 comment+mention\"}" \
  "$B_TOKEN")
check "bob @alice 评论" 200 "$code"
code=$(post "/api/like" "{\"video_id\":$VIDEO_ID}" "$C_TOKEN")
check "carol 点赞视频" 200 "$code"
code=$(post "/api/user/$A_UID/follow" '{}' "$C_TOKEN")
check "carol 关注 alice" 200 "$code"
code=$(post_internal "/api/notifications/system" \
  "{\"user_id\":$A_UID,\"content\":\"欢迎加入抖音\"}")
check "内部 token 写系统通知" 201 "$code"
code=$(post "/api/notifications/system" \
  "{\"user_id\":$A_UID,\"content\":\"你的视频上热门了\",\"target_type\":\"video\",\"target_id\":$VIDEO_ID}" \
  "$A_TOKEN")
check "admin 写系统通知" 201 "$code"

echo "[3/12] 分类过滤：每个 kind 只返回本类"
code=$(get "/api/notifications?limit=20" "$A_TOKEN")
check "全部列表状态码" 200 "$code"
check "  全部 items=7" 7 "$(items_count)"
check "  counts.all=7" 7 "$(count_of all)"
check "  counts.like=1" 1 "$(count_of like)"
check "  counts.comment=2" 2 "$(count_of comment)"
check "  counts.follow=1" 1 "$(count_of follow)"
check "  counts.mention=1" 1 "$(count_of mention)"
check "  counts.system=2" 2 "$(count_of system)"
check "  unread_count=counts.all" 7 "$(json_int unread_count)"
code=$(get "/api/notifications?kind=like&limit=20" "$A_TOKEN")
check "kind=like 状态码" 200 "$code"
check "  like items=1" 1 "$(items_count)"
check "  like 列表只有 like" like "$(kinds_all)"
code=$(get "/api/notifications?kind=comment&limit=20" "$A_TOKEN")
check "kind=comment items=2" 2 "$(items_count)"
check "  comment 列表只有 comment" comment "$(kinds_all)"
code=$(get "/api/notifications?kind=follow&limit=20" "$A_TOKEN")
check "kind=follow items=1" 1 "$(items_count)"
check "  follow 列表只有 follow" follow "$(kinds_all)"
code=$(get "/api/notifications?kind=mention&limit=20" "$A_TOKEN")
check "kind=mention items=1" 1 "$(items_count)"
check "  mention 列表只有 mention" mention "$(kinds_all)"
code=$(get "/api/notifications?kind=system&limit=20" "$A_TOKEN")
check "kind=system items=2" 2 "$(items_count)"
check "  system 列表只有 system" system "$(kinds_all)"
check "  系统通知里含 target_type=system 的记录" 1 "$(occurrences '"target_type":"system"')"
code=$(get "/api/notifications?kind=bad" "$A_TOKEN")
check "非法 kind → 400" 400 "$code"

echo "[4/12] 系统通知权限"
code=$(post "/api/notifications/system" \
  "{\"user_id\":$B_UID,\"content\":\"匿名写入\"}")
check "无凭证写系统通知 → 401" 401 "$code"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST \
  -H 'Content-Type: application/json' -H 'X-Internal-Token: wrong-token' \
  -d "{\"user_id\":$B_UID,\"content\":\"错误内部 token\"}" \
  "$GW/api/notifications/system")
check "错误内部 token → 401" 401 "$code"
code=$(post "/api/notifications/system" \
  "{\"user_id\":$B_UID,\"content\":\"普通用户越权\"}" "$B_TOKEN")
check "普通用户写系统通知 → 403" 403 "$code"
code=$(post "/api/notifications/system" \
  "{\"user_id\":999999,\"content\":\"幽灵用户\"}" "$A_TOKEN")
check "接收者不存在 → 404" 404 "$code"
code=$(post "/api/notifications/system" \
  "{\"user_id\":$B_UID,\"content\":\"\"}" "$A_TOKEN")
check "content 为空 → 400" 400 "$code"
code=$(post "/api/notifications/system" \
  "{\"user_id\":$B_UID,\"content\":\"坏目标\",\"target_type\":\"comment\"}" \
  "$A_TOKEN")
check "target_type=comment 非法 → 400" 400 "$code"

echo "[5/12] 系统通知写入与展示（bob 收件箱）"
code=$(post "/api/notifications/system" \
  "{\"user_id\":$B_UID,\"content\":\"bob 的欢迎通知\"}" "$A_TOKEN")
check "admin 给 bob 写系统通知" 201 "$code"
code=$(get "/api/notifications?kind=system&limit=20" "$B_TOKEN")
check "bob 系统通知列表状态码" 200 "$code"
check "  bob system items=1" 1 "$(items_count)"
check "  bob 系统通知正文" "bob 的欢迎通知" "$(json_str content)"
check "  bob 系统通知 kind=system" system "$(first_kind)"
code=$(get "/api/notifications?kind=system&limit=20" "$A_TOKEN")
check "alice 系统通知 2 条（内部 + admin）" 2 "$(items_count)"
check "  alice counts.system=2" 2 "$(count_of system)"

echo "[6/12] 单类已读：kind=like 只清 like"
code=$(get "/api/notifications?kind=like&limit=20" "$A_TOKEN")
LIKE_IDS="$(item_ids)"
code=$(post "/api/notifications/read" '{"kind":"like"}' "$A_TOKEN")
check "kind=like 全部已读" 200 "$code"
check "  updated=1" 1 "$(json_int updated)"
check "  未读数 7→6" 6 "$(json_int unread_count)"
code=$(get "/api/notifications?kind=like&limit=20" "$A_TOKEN")
check "  like 列表 read_at 全 > 0" yes "$(all_read)"
check "  counts.like=0" 0 "$(count_of like)"
check "  counts.comment 不变=2" 2 "$(count_of comment)"
code=$(post "/api/notifications/read" '{"kind":"like"}' "$A_TOKEN")
check "重复 kind=like 已读（幂等）" 200 "$code"
check "  updated=0" 0 "$(json_int updated)"
code=$(post "/api/notifications/read" '{"kind":"bad"}' "$A_TOKEN")
check "kind=bad 已读 → 400" 400 "$code"
code=$(get "/api/notifications?kind=like&limit=20" "$A_TOKEN")
check "  like 记录数仍为 1（未删除）" 1 "$(items_count)"

echo "[7/12] 单类已读：kind=system 只清 system"
code=$(post "/api/notifications/read" '{"kind":"system"}' "$A_TOKEN")
check "kind=system 全部已读" 200 "$code"
check "  updated=2" 2 "$(json_int updated)"
check "  未读数 6→4" 4 "$(json_int unread_count)"
code=$(get "/api/notifications?kind=system&limit=20" "$A_TOKEN")
check "  system 列表 read_at 全 > 0" yes "$(all_read)"
check "  counts.system=0" 0 "$(count_of system)"
check "  counts.mention 仍为 1" 1 "$(count_of mention)"

echo "[8/12] 越权：bob 的单类已读不影响 alice"
code=$(post "/api/notifications/read" '{"kind":"comment"}' "$B_TOKEN")
check "bob 单类已读自己" 200 "$code"
check "  bob updated=0（无 comment 通知）" 0 "$(json_int updated)"
code=$(get "/api/notifications?kind=comment&limit=20" "$A_TOKEN")
check "  alice comment 仍未读 2 条" 2 "$(unread_items)"
check "  alice 未读数仍为 4" 4 "$(json_int unread_count)"

echo "[9/12] 全部已读"
code=$(post "/api/notifications/read" '{"all":true}' "$A_TOKEN")
check "全部已读" 200 "$code"
check "  updated=4（comment2 + mention1 + follow1）" 4 "$(json_int updated)"
check "  未读数归零" 0 "$(json_int unread_count)"
code=$(get "/api/notifications?limit=20" "$A_TOKEN")
check "  counts 全 0（all）" 0 "$(count_of all)"
check "  counts.like=0" 0 "$(count_of like)"
check "  counts.comment=0" 0 "$(count_of comment)"
check "  counts.follow=0" 0 "$(count_of follow)"
check "  counts.mention=0" 0 "$(count_of mention)"
check "  counts.system=0" 0 "$(count_of system)"
check "  列表 7 条记录仍在" 7 "$(items_count)"
check "  列表 read_at 全 > 0" yes "$(all_read)"
code=$(post "/api/notifications/read" '{"all":true}' "$A_TOKEN")
check "重复全部已读 updated=0" 0 "$(json_int updated)"

echo "[10/12] 分页：kind 过滤 + cursor 不重不漏"
# 重新造 3 条 comment 便于分页：bob 再评论 3 次。
for i in 1 2 3; do
  code=$(post "/api/comment" \
    "{\"video_id\":$VIDEO_ID,\"content\":\"分页评论 $i\"}" "$B_TOKEN")
  check "bob 分页评论 $i" 200 "$code"
done
code=$(get "/api/notifications?kind=comment&limit=20" "$A_TOKEN")
COMMENT_TOTAL="$(items_count)"
check "comment 列表总数（2+3）" 5 "$COMMENT_TOTAL"
COMMENT_ALL_IDS="$(item_ids)"
PAGE_IDS=""
CURSOR=""
PAGES=0
while :; do
  if [[ -z "$CURSOR" ]]; then
    code=$(get "/api/notifications?kind=comment&limit=2" "$A_TOKEN")
  else
    code=$(get "/api/notifications?kind=comment&limit=2&cursor=$CURSOR" "$A_TOKEN")
  fi
  check "第 $((PAGES + 1)) 页状态码" 200 "$code"
  check "  第 $((PAGES + 1)) 页只有 comment" comment "$(kinds_all | sed 's/,$//')"
  PAGE_IDS="$PAGE_IDS $(item_ids)"
  CURSOR="$(json_str next_cursor)"
  PAGES=$((PAGES + 1))
  [[ -z "$CURSOR" ]] && break
  [[ "$PAGES" -ge 10 ]] && break
done
check "分页页数（5 条 / 每页 2）" 3 "$PAGES"
check "分页无重复" \
  "$(printf '%s\n' $PAGE_IDS | sort -u | wc -l | tr -d ' ')" \
  "$(printf '%s\n' $PAGE_IDS | wc -l | tr -d ' ')"
check "分页 id 集合与首页一致" \
  "$(printf '%s\n' $COMMENT_ALL_IDS | sort | tr '\n' ',')" \
  "$(printf '%s\n' $PAGE_IDS | sort | tr '\n' ',')"

echo "[11/12] 鉴权 / 方法 / 前端元素"
code=$(get "/api/notifications")
check "无 token 列表 → 401" 401 "$code"
code=$(get "/api/notifications?kind=like")
check "无 token 分类列表 → 401" 401 "$code"
code=$(post "/api/notifications/read" '{"kind":"like"}')
check "无 token 单类已读 → 401" 401 "$code"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' \
  -X GET "$GW/api/notifications/system")
check "GET /system → 405" 405 "$code"
code=$(get "/api/notifications/unread_count" "$B_TOKEN")
check "bob 未读数接口仍可用" 200 "$code"
code=$(curl -s --max-time 5 -o "$TMP/page" -w '%{http_code}' "$GW/")
check "GET / → 200" 200 "$code"
PAGE_SIZE="$(wc -c <"$TMP/page" | tr -d ' ')"
if [[ "$PAGE_SIZE" -gt 100000 ]]; then
  check "页面大小 > 100KB" yes yes
else
  check "页面大小 > 100KB" yes "$PAGE_SIZE bytes"
fi
for pat in notifTabs notif-tab notif-tab-badge 'data-kind="system"' 📢 \
           paintNotifTabs setNotifKind notifCountFor; do
  if grep -q "$pat" "$TMP/page"; then
    check "页面含 $pat" yes yes
  else
    check "页面含 $pat" yes no
  fi
done

echo "[12/12] 性能：counts 查询计划 + TTFB"
PLAN="$(python3 - "$DB" <<'PY'
import sqlite3
import sys

con = sqlite3.connect(sys.argv[1])
con.execute("PRAGMA query_only=1")
sqls = {
    "counts": (
        "SELECT kind, COUNT(*) FROM notifications"
        " WHERE user_id = 1 AND read_at = 0"
        " AND actor_id NOT IN (SELECT blocked_id FROM blocks WHERE user_id = 1)"
        " GROUP BY kind"
    ),
    "list": (
        "SELECT n.id, u.name FROM notifications n LEFT JOIN users u"
        " ON u.id = n.actor_id WHERE n.user_id = 1 AND n.kind = 'like'"
        " ORDER BY n.created_at DESC, n.id DESC LIMIT 21"
    ),
}
for name, sql in sqls.items():
    plan = " ".join(str(row[-1]) for row in con.execute("EXPLAIN QUERY PLAN " + sql))
    print(name + "=" + plan)
PY
)"
if printf '%s\n' "$PLAN" | grep -q 'counts=.*idx_notifications_kind'; then
  check "分类计数命中 idx_notifications_kind" yes yes
else
  check "分类计数命中 idx_notifications_kind" yes "$PLAN"
fi
if printf '%s\n' "$PLAN" | grep -qi 'TEMP B-TREE'; then
  check "分类计数无临时 B 树" no yes
else
  check "分类计数无临时 B 树" no no
fi
if printf '%s\n' "$PLAN" | grep -q 'list=.*idx_notifications_timeline'; then
  check "分类列表命中 idx_notifications_timeline" yes yes
else
  check "分类列表命中 idx_notifications_timeline" yes "$PLAN"
fi

# 预热 3 次，再串行测 20 次（单核低负载，取平均与最大值）。
for _ in $(seq 1 3); do
  curl -s -o /dev/null -H "Authorization: Bearer $A_TOKEN" \
    "$GW/api/notifications?kind=like&limit=20" >/dev/null
done
TTFB_SUM=0
TTFB_MAX=0
for _ in $(seq 1 20); do
  t="$(curl -s -o /dev/null -w '%{time_starttransfer}' \
    -H "Authorization: Bearer $A_TOKEN" "$GW/api/notifications?kind=like&limit=20")"
  ms="$(awk "BEGIN{printf \"%d\", $t * 1000}")"
  TTFB_SUM=$((TTFB_SUM + ms))
  if [[ "$ms" -gt "$TTFB_MAX" ]]; then TTFB_MAX="$ms"; fi
done
TTFB_AVG=$((TTFB_SUM / 20))
if [[ "$TTFB_AVG" -lt 200 ]]; then
  check "分类通知 TTFB 平均 < 200ms" yes yes
else
  check "分类通知 TTFB 平均 < 200ms" yes "${TTFB_AVG}ms"
fi
if [[ "$TTFB_MAX" -lt 200 ]]; then
  check "分类通知 TTFB 最大 < 200ms" yes yes
else
  check "分类通知 TTFB 最大 < 200ms" yes "${TTFB_MAX}ms"
fi
printf '  （TTFB 平均 %dms / 最大 %dms，20 次串行；列表固定 2 条 SQL）\n' \
  "$TTFB_AVG" "$TTFB_MAX"

echo "---------------------------------------------"
printf '[accept_notif_cluster] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_notif_cluster] PASS"
  exit 0
fi
echo "[accept_notif_cluster] FAIL"
exit 1
