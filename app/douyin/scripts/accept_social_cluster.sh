#!/usr/bin/env bash
# 第 6 轮簇 A 社交三件套验收：@mention / 评论排序（hot|new）/ 评论置顶。
#
# 覆盖：
#   1) @mention：解析中文/英文/数字昵称、写 mention 通知（target=video）、
#      自己 @ 自己不通知、同一条评论重复 @ 只通知一次、邮箱 a@b 不算提及、
#      不存在的昵称不报错也不通知。
#   2) 评论排序：sort 缺省 new；new=时间倒序、hot=点赞数降序（同赞按时间）；
#      limit/cursor 分页在 hot 下正确；非法 sort 400；两种游标不能串用。
#   3) 评论置顶：仅视频作者可置顶/取消（非作者 403、不存在 404、匿名 401、
#      回复 400）；置顶评论在 hot/new 下都排最前且不占分页 limit；重复置顶
#      幂等；取消后恢复原顺序；翻页不重复返回置顶评论。
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_social_cluster.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_social_cluster.pids"
DB="$ROOT/data/accept_social_cluster.db"
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

# ---- JSON 解析（python3 标准库；表达式里 d 是根对象） ----------------------
JSONQ="$TMP/jsonq.py"
cat >"$JSONQ" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
expr = sys.argv[2]
scope = {"d": data, "len": len, "int": int, "str": str, "sum": sum}
value = eval(expr, {"__builtins__": {"len": len, "int": int, "str": str,
                                      "sum": sum}}, scope)
if isinstance(value, bool):
    print("true" if value else "false")
elif isinstance(value, list):
    print(" ".join(str(x) for x in value))
else:
    print(value)
PY
jsonq() { python3 "$JSONQ" "$TMP/body" "$1"; }

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
    printf '  %-52s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-52s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

check_contains() { # name needle haystack
  local name="$1" needle="$2" haystack="$3"
  if [[ "$haystack" == *"$needle"* ]]; then
    printf '  %-52s PASS  含 %s\n' "$name" "$needle"
    PASSED=$((PASSED + 1))
  else
    printf '  %-52s FAIL  缺少 %s\n' "$name" "$needle"
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

ttfb() { # path [token] → 秒（curl time_starttransfer）
  local path="$1" token="${2:-}"
  local args=(-s -o /dev/null -w '%{time_starttransfer}' --max-time 5 "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

comment_ids() { jsonq "[it['comment']['id'] for it in d['items']]"; }
mention_count() { jsonq "sum(1 for n in d['items'] if n['kind'] == 'mention')"; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_social_cluster_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_social_cluster_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_social_cluster_"*.log

echo "[0/9] 启动 7 个进程"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

echo "[1/9] 注册 alice / bob / carol 并发布视频"
code=$(post "/api/user/register" '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
ALICE_TOKEN="$(jsonq "d['token']")"
ALICE_ID="$(jsonq "d['user']['id']")"
code=$(post "/api/user/register" '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
BOB_TOKEN="$(jsonq "d['token']")"
code=$(post "/api/user/register" '{"name":"carol","email":"carol@example.com","password":"secret123"}')
check "注册 carol" 200 "$code"
CAROL_TOKEN="$(jsonq "d['token']")"
code=$(post "/api/video" '{"title":"社交三件套验收 #话题","url":"/videos/demo1.mp4"}' "$ALICE_TOKEN")
check "alice 发布视频" 200 "$code"
VIDEO_ID="$(jsonq "d['video']['id']")"
if [[ -z "$ALICE_TOKEN" || -z "$BOB_TOKEN" || -z "$CAROL_TOKEN" || -z "$VIDEO_ID" ]]; then
  echo "准备数据失败：$(cat "$TMP/body")" >&2
  exit 1
fi
echo "  video_id=$VIDEO_ID alice_id=$ALICE_ID"

echo "[2/9] 三条顶层评论 + 点赞构造热度"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"第一条普通评论\"}" "$BOB_TOKEN")
check "bob 发顶层评论 c1" 200 "$code"
C1="$(jsonq "d['comment']['id']")"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"@carol 自己 @ 自己不该有通知\"}" "$CAROL_TOKEN")
check "carol 发自提及评论 c2" 200 "$code"
C2="$(jsonq "d['comment']['id']")"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"作者的最新评论\"}" "$ALICE_TOKEN")
check "alice 发最新评论 c3" 200 "$code"
C3="$(jsonq "d['comment']['id']")"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"回复 c1\",\"parent_id\":$C1}" "$ALICE_TOKEN")
check "alice 回复 c1" 200 "$code"
REPLY_ID="$(jsonq "d['comment']['id']")"
code=$(post "/api/comment/$C1/like" '{}' "$BOB_TOKEN")
check "bob 点赞 c1（1/2）" 200 "$code"
code=$(post "/api/comment/$C1/like" '{}' "$CAROL_TOKEN")
check "carol 点赞 c1（2/2）" 200 "$code"
code=$(post "/api/comment/$C3/like" '{}' "$BOB_TOKEN")
check "bob 点赞 c3（1）" 200 "$code"
echo "  c1=$C1 c2=$C2 c3=$C3 reply=$REPLY_ID"

echo "[3/9] 评论排序：new / hot / 分页 / 非法值"
code=$(get "/api/comment/$VIDEO_ID")
check "默认排序（new）列表 200" 200 "$code"
check "  默认 = c3 c2 c1" "$C3 $C2 $C1" "$(comment_ids)"
code=$(get "/api/comment/$VIDEO_ID?sort=new")
check "sort=new 列表 200" 200 "$code"
check "  new = c3 c2 c1" "$C3 $C2 $C1" "$(comment_ids)"
check "  sort 字段回显 new" new "$(jsonq "d['sort']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot")
check "sort=hot 列表 200" 200 "$code"
check "  hot = c1 c3 c2（2赞/1赞/0赞）" "$C1 $C3 $C2" "$(comment_ids)"
check "  sort 字段回显 hot" hot "$(jsonq "d['sort']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot" "$BOB_TOKEN")
check "bob 带 token 读 hot 列表" 200 "$code"
check "  bob 视角 c1 liked=true" true "$(jsonq "d['items'][0]['comment']['liked']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot&limit=1")
check "hot limit=1 第一页" 200 "$code"
check "  第一页 = c1" "$C1" "$(comment_ids)"
HOT_CURSOR="$(jsonq "d['next_cursor']")"
check "  next_cursor 非空" yes "$([[ -n "$HOT_CURSOR" ]] && echo yes || echo no)"
code=$(get "/api/comment/$VIDEO_ID?sort=hot&limit=1&cursor=$HOT_CURSOR")
check "hot 第二页" 200 "$code"
check "  第二页 = c3" "$C3" "$(comment_ids)"
HOT_CURSOR2="$(jsonq "d['next_cursor']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot&limit=1&cursor=$HOT_CURSOR2")
check "hot 第三页" 200 "$code"
check "  第三页 = c2" "$C2" "$(comment_ids)"
check "  末页 next_cursor 为空" "" "$(jsonq "d['next_cursor']")"
code=$(get "/api/comment/$VIDEO_ID?sort=foo")
check "非法 sort → 400" 400 "$code"
code=$(get "/api/comment/$VIDEO_ID?sort=new&limit=1")
NEW_CURSOR="$(jsonq "d['next_cursor']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot&cursor=$NEW_CURSOR")
check "new 游标传给 hot → 400" 400 "$code"
code=$(get "/api/comment/$VIDEO_ID?sort=new&cursor=$HOT_CURSOR")
check "hot 游标传给 new → 400" 400 "$code"

echo "[4/9] 评论置顶：权限 / 排序 / 幂等 / 取消"
code=$(post "/api/comment/$C2/pin" '{}' "$BOB_TOKEN")
check "非作者 bob 置顶 → 403" 403 "$code"
code=$(post "/api/comment/$C2/pin" '{}')
check "匿名置顶 → 401" 401 "$code"
code=$(post "/api/comment/999999999/pin" '{}' "$ALICE_TOKEN")
check "不存在的评论 → 404" 404 "$code"
code=$(post "/api/comment/$REPLY_ID/pin" '{}' "$ALICE_TOKEN")
check "回复不可置顶 → 400" 400 "$code"
code=$(post "/api/comment/$C2/pin" '{}' "$ALICE_TOKEN")
check "作者置顶 c2" 200 "$code"
check "  pinned=true" true "$(jsonq "d['pinned']")"
check "  pinned_at>0" yes "$([[ "$(jsonq "d['comment']['pinned_at']")" -gt 0 ]] && echo yes || echo no)"
code=$(get "/api/comment/$VIDEO_ID?sort=new")
check "置顶后 new 第一 = c2" "$C2" "$(jsonq "d['items'][0]['comment']['id']")"
check "  c2 只出现一次" 1 "$(jsonq "sum(1 for it in d['items'] if it['comment']['id'] == $C2)")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot")
check "置顶后 hot 第一 = c2（置顶优先于 sort）" "$C2" "$(jsonq "d['items'][0]['comment']['id']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot&limit=1")
check "置顶不占 limit：第一页 2 条" 2 "$(jsonq "len(d['items'])")"
check "  第一页 = c2 c1" "$C2 $C1" "$(comment_ids)"
PIN_CURSOR="$(jsonq "d['next_cursor']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot&limit=1&cursor=$PIN_CURSOR")
check "翻页不重复返回置顶评论" "$C3" "$(comment_ids)"
code=$(post "/api/comment/$C2/pin" '{}' "$ALICE_TOKEN")
check "重复置顶（幂等）" 200 "$code"
code=$(get "/api/comment/$VIDEO_ID")
check "  重复置顶后仍只有一条 c2" 1 "$(jsonq "sum(1 for it in d['items'] if it['comment']['id'] == $C2)")"
code=$(del "/api/comment/$C2/pin" "$ALICE_TOKEN")
check "作者取消置顶" 200 "$code"
check "  pinned=false" false "$(jsonq "d['pinned']")"
check "  pinned_at=0" 0 "$(jsonq "d['comment']['pinned_at']")"
code=$(get "/api/comment/$VIDEO_ID?sort=hot")
check "取消后 hot 第一 = c1" "$C1" "$(jsonq "d['items'][0]['comment']['id']")"
code=$(get "/api/comment/$VIDEO_ID?sort=new")
check "取消后 new 第一 = c3" "$C3" "$(jsonq "d['items'][0]['comment']['id']")"
code=$(del "/api/comment/$C2/pin" "$ALICE_TOKEN")
check "重复取消（幂等）" 200 "$code"

echo "[5/9] @mention：解析 / 通知 / 自己 @ 自己 / 重复 @ / 邮箱"
code=$(get "/api/notifications?limit=100" "$CAROL_TOKEN")
check "carol 读通知列表" 200 "$code"
CAROL_M0="$(mention_count)"
check "  carol 初始 mention 数" 0 "$CAROL_M0"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"@carol 一起看这个视频\"}" "$BOB_TOKEN")
check "bob 评论 @carol" 200 "$code"
code=$(get "/api/notifications?limit=100" "$CAROL_TOKEN")
check "carol 通知列表" 200 "$code"
check "  carol mention +1" 1 "$(mention_count)"
check "  actor_name=bob" bob "$(jsonq "[n['actor_name'] for n in d['items'] if n['kind'] == 'mention'][0]")"
check "  target_type=video" video "$(jsonq "[n['target_type'] for n in d['items'] if n['kind'] == 'mention'][0]")"
check "  target_id=video_id" "$VIDEO_ID" "$(jsonq "[n['target_id'] for n in d['items'] if n['kind'] == 'mention'][0]")"
check_contains "  通知摘要含正文" "一起看这个视频" "$(jsonq "[n['content'] for n in d['items'] if n['kind'] == 'mention'][0]")"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"@carol 我自己 @ 自己\"}" "$CAROL_TOKEN")
check "carol 自己 @ 自己" 200 "$code"
code=$(get "/api/notifications?limit=100" "$CAROL_TOKEN")
check "  自己不通知（mention 仍 1）" 1 "$(mention_count)"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"@carol @carol 重复提及\"}" "$BOB_TOKEN")
check "bob 重复 @carol 两次" 200 "$code"
code=$(get "/api/notifications?limit=100" "$CAROL_TOKEN")
check "  同一条评论只通知一次（mention=2）" 2 "$(mention_count)"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"test@carol 这不是提及\"}" "$BOB_TOKEN")
check "邮箱形式的 a@b 评论" 200 "$code"
code=$(get "/api/notifications?limit=100" "$CAROL_TOKEN")
check "  邮箱不算提及（mention 仍 2）" 2 "$(mention_count)"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"@nosuchuser123 你好\"}" "$BOB_TOKEN")
check "不存在的昵称不报错" 200 "$code"
code=$(get "/api/notifications?limit=100" "$CAROL_TOKEN")
check "  未匹配昵称不通知（mention 仍 2）" 2 "$(mention_count)"
code=$(get "/api/notifications/unread_count" "$CAROL_TOKEN")
check "carol 未读数 = mention 数" 2 "$(jsonq "d['unread_count']")"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"@bob 谢谢支持\"}" "$ALICE_TOKEN")
check "alice 评论 @bob" 200 "$code"
code=$(get "/api/notifications?limit=100" "$BOB_TOKEN")
check "bob mention 数 = 1" 1 "$(mention_count)"
code=$(post "/api/comment" "{\"video_id\":$VIDEO_ID,\"content\":\"@alice 我自己\"}" "$ALICE_TOKEN")
check "alice 自己 @ 自己" 200 "$code"
code=$(get "/api/notifications?limit=100" "$ALICE_TOKEN")
check "  alice mention 数 = 0" 0 "$(mention_count)"

echo "[6/9] 新路径别名 /api/video/:id/comments"
code=$(get "/api/video/$VIDEO_ID/comments?sort=hot")
check "别名路径 200" 200 "$code"
ALIAS_IDS="$(comment_ids)"
code=$(get "/api/comment/$VIDEO_ID?sort=hot")
check "  别名与旧路径顺序一致" "$(comment_ids)" "$ALIAS_IDS"
code=$(get "/api/video/$VIDEO_ID/comments?sort=foo")
check "别名路径非法 sort → 400" 400 "$code"
code=$(post "/api/video/$VIDEO_ID/comments" '{}' "$ALICE_TOKEN")
check "别名路径 POST → 405" 405 "$code"
code=$(get "/api/video/999999999/comments")
check "别名路径不存在视频 → 404" 404 "$code"

echo "[7/9] 前端资源"
code=$(get "/")
check "GET / 200" 200 "$code"
BODY="$(cat "$TMP/body")"
check_contains "评论排序切换" 'id="cpSort"' "$BODY"
check_contains "最热按钮" 'data-sort="hot"' "$BODY"
check_contains "mention 下拉" 'id="mentionMenu"' "$BODY"
check_contains "通知文案" "在评论中提到了你" "$BODY"
check_contains "置顶标签样式" '.cp-pin' "$BODY"
check_contains "置顶请求" "api/comment/' + c.id + '/pin" "$BODY"
check_contains "排序请求带 sort" "&sort=' +" "$BODY"

echo "[8/9] 性能：评论 hot 列表 TTFB（20 次串行，阈值 200 ms）"
SUM=0
MAX=0
for _ in $(seq 1 20); do
  T="$(ttfb "/api/comment/$VIDEO_ID?sort=hot&limit=50")"
  SUM="$(awk -v a="$SUM" -v b="$T" 'BEGIN { printf "%.6f", a + b }')"
  MAX="$(awk -v a="$MAX" -v b="$T" 'BEGIN { printf "%.6f", (a > b ? a : b) }')"
done
AVG="$(awk -v s="$SUM" 'BEGIN { printf "%.3f", s / 20 * 1000 }')"
MAXMS="$(awk -v m="$MAX" 'BEGIN { printf "%.3f", m * 1000 }')"
echo "  TTFB avg=${AVG} ms max=${MAXMS} ms"
if awk -v m="$MAX" 'BEGIN { exit !(m < 0.200) }'; then
  check "hot 列表 TTFB < 200 ms" yes yes
else
  check "hot 列表 TTFB < 200 ms" yes no "max=${MAXMS} ms"
fi

echo "[9/9] 结果"
if [[ "$FAILED" -eq 0 ]]; then
  echo "PASS：$PASSED 项全部通过"
  exit 0
fi
echo "FAIL：$PASSED 通过 / $FAILED 失败"
exit 1
