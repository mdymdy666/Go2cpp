#!/usr/bin/env bash
# 第 13 轮：关注 / 粉丝列表 + 关注动态验收。
#
# 覆盖：
#   [1] 关注列表：字段（id/name/avatar_url/bio/followed_by_me）、关注时间倒序、
#       keyset 分页（limit=1 逐页不重不漏）、非法游标 400、用户不存在 404、405
#   [2] 粉丝列表：反向关系、倒序、分页、匿名（followed_by_me 全 false）
#   [3] 拉黑过滤：我拉黑的人不出现；拉黑我的人也不出现（双向）；取消拉黑恢复
#   [4] 空状态：没人关注/没粉丝 → items=[] + next_cursor=""
#   [5] 关注动态：只含已关注作者、按发布时间倒序、分页、拉黑过滤、未登录 401、
#       非法游标 400、空关注流
#   [6] 前端：GET / 含关注/粉丝抽屉、可点统计、关注按钮、空状态文案；关注 tab
#       仍走 api/feed/following
#   [7] 性能：两个列表 + 关注流各 20 次 TTFB < 200 ms；EXPLAIN 命中
#       idx_follows_follower_time / idx_follows_followee_time
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_follow_lists.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_follow_lists.pids"
DB="$ROOT/data/accept_follow_lists.db"
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
      if kill -0 "$pid" 2>/dev/null; then
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
    printf '  %-56s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-56s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
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

ttfb() { # path [token] → 秒
  local path="$1" token="${2:-}"
  local args=(-s -o /dev/null -w '%{time_starttransfer}' --max-time 5 "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

JSONQ="$TMP/jsonq.py"
cat >"$JSONQ" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
expr = sys.argv[2]
scope = {"d": data, "len": len, "int": int, "str": str, "bool": bool,
         "sum": sum, "sorted": sorted, "set": set, "any": any, "all": all}
value = eval(expr, {"__builtins__": {"len": len, "int": int, "str": str,
                                      "bool": bool, "sum": sum,
                                      "sorted": sorted, "set": set,
                                      "any": any, "all": all}}, scope)
if isinstance(value, bool):
    print("true" if value else "false")
elif isinstance(value, list):
    print(" ".join(str(x) for x in value))
else:
    print(value)
PY
jsonq() { python3 "$JSONQ" "$TMP/body" "$1"; }
item_names() { jsonq "','.join(it['name'] for it in d['items'])"; }
item_ids() { jsonq "','.join(str(it['id']) for it in d['items'])"; }
item_flags() { jsonq "','.join('1' if it['followed_by_me'] else '0' for it in d['items'])"; }
item_count() { jsonq "len(d['items'])"; }
feed_ids() { jsonq "','.join(str(it['video']['id']) for it in d['items'])"; }
feed_count() { jsonq "len(d['items'])"; }
feed_authors() { jsonq "','.join(it['author'] for it in d['items'])"; }
json_str() { jsonq "d.get('$1','')"; }
json_int() { jsonq "d.get('$1',0)"; }
json_bool() { jsonq "bool(d.get('$1'))"; }
uniq_count() { tr ',' '\n' <<<"$1" | sed '/^$/d' | sort -u | wc -l; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_follow_lists_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_follow_lists_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_follow_lists_"*.log

echo "[0/7] 启动 7 个进程（6 业务 + gateway）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

register() { # name email -> token
  post "/api/user/register" \
    "{\"name\":\"$1\",\"email\":\"$2\",\"password\":\"secret123\"}" >/dev/null
  json_str token
}
user_id_of() { jsonq "d['user']['id']"; }
follow() { # token target
  post "/api/user/$2/follow" '{}' "$1" >/dev/null
}
unfollow() { # token target
  del "/api/user/$2/follow" "$1" >/dev/null
}
mkvideo() { # token title -> id
  post "/api/video" \
    "{\"title\":\"$2\",\"url\":\"http://example.com/$2.mp4\"}" "$1" >/dev/null
  jsonq "d['video']['id']"
}

echo "[1/7] 准备数据（5 用户 / 4 视频 / 关注关系）"
TOKEN_A="$(register alice alice@example.com)"; UID_A="$(user_id_of)"
TOKEN_B="$(register bob bob@example.com)"; UID_B="$(user_id_of)"
TOKEN_C="$(register carol carol@example.com)"; UID_C="$(user_id_of)"
TOKEN_D="$(register dave dave@example.com)"; UID_D="$(user_id_of)"
TOKEN_E="$(register eve eve@example.com)"; UID_E="$(user_id_of)"
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" || -z "$TOKEN_C" || -z "$TOKEN_D" || -z "$TOKEN_E" ]]; then
  echo "注册失败（A=$TOKEN_A B=$TOKEN_B C=$TOKEN_C D=$TOKEN_D E=$TOKEN_E）" >&2
  exit 1
fi
check "五个用户 id 互不相同" 5 \
  "$(printf '%s\n' "$UID_A" "$UID_B" "$UID_C" "$UID_D" "$UID_E" | sort -u | wc -l)"

# 视频：b1 < a1 < c1 < b2（发布时间严格递增，供倒序断言）。
V_B1="$(mkvideo "$TOKEN_B" b1)"; sleep 1.1
V_A1="$(mkvideo "$TOKEN_A" a1)"; sleep 1.1
V_C1="$(mkvideo "$TOKEN_C" c1)"; sleep 1.1
V_B2="$(mkvideo "$TOKEN_B" b2)"
check "四条视频 id 互不相同" 4 \
  "$(printf '%s\n' "$V_B1" "$V_A1" "$V_C1" "$V_B2" | sort -u | wc -l)"

# 关注时间严格递增：alice→bob、alice→carol、alice→dave；bob→carol；eve→alice。
follow "$TOKEN_A" "$UID_B"; sleep 1.1
follow "$TOKEN_A" "$UID_C"; sleep 1.1
follow "$TOKEN_A" "$UID_D"
follow "$TOKEN_B" "$UID_C"
follow "$TOKEN_E" "$UID_A"
check "alice 关注 bob" true "$(json_bool following)"

# ---- [2/7] 关注列表 ---------------------------------------------------------
echo "[2/7] 关注列表"
code=$(get "/api/user/$UID_A/following" "$TOKEN_A")
check "alice 看自己 following" 200 "$code"
check "kind=following" following "$(json_str kind)"
check "following 3 条" 3 "$(item_count)"
check "按关注时间倒序（dave/carol/bob）" "dave,carol,bob" "$(item_names)"
check "自己视角 followed_by_me 全 true" "1,1,1" "$(item_flags)"
check "条目含 id/name/avatar_url/bio" "3" "$(jsonq "sum(1 for it in d['items'] if it['id'] and 'name' in it and 'avatar_url' in it and 'bio' in it)")"
check "alice 的 following 不含 alice 自己" no \
  "$(jsonq "'yes' if any(it['id'] == $UID_A for it in d['items']) else 'no'")"

# 他人视角：bob 只关注 carol → carol=true，dave=false。
code=$(get "/api/user/$UID_A/following" "$TOKEN_B")
check "bob 看 alice following" 200 "$code"
check "bob 视角 followed_by_me（dave,carol,bob 顺序）" "0,1,0" "$(item_flags)"

# 匿名：全部 false。
code=$(get "/api/user/$UID_A/following")
check "匿名看 alice following" 200 "$code"
check "匿名 followed_by_me 全 false" "0,0,0" "$(item_flags)"

# 分页：limit=2 → [dave,carol] + cursor；第二页 [bob]。
code=$(get "/api/user/$UID_A/following?limit=2" "$TOKEN_A")
check "following limit=2 第一页" 200 "$code"
check "第一页 2 条" 2 "$(item_count)"
CUR_F="$(json_str next_cursor)"
check "第一页有 next_cursor" yes "$([[ -n "$CUR_F" ]] && echo yes || echo no)"
check "第一页顺序 dave,carol" "dave,carol" "$(item_names)"
code=$(get "/api/user/$UID_A/following?limit=2&cursor=$CUR_F" "$TOKEN_A")
check "following 第二页" 200 "$code"
check "第二页 1 条" 1 "$(item_count)"
check "第二页顺序 bob" "bob" "$(item_names)"
check "第二页 next_cursor 空" "" "$(json_str next_cursor)"

# limit=1 逐页翻完：3 页、3 个唯一 id。
CUR=""; PAGES=0; ALL_IDS=""
while :; do
  if [[ -z "$CUR" ]]; then
    code=$(get "/api/user/$UID_A/following?limit=1" "$TOKEN_A")
  else
    code=$(get "/api/user/$UID_A/following?limit=1&cursor=$CUR" "$TOKEN_A")
  fi
  [[ "$code" == "200" ]] || break
  ALL_IDS="$ALL_IDS$(item_ids),"
  CUR="$(json_str next_cursor)"
  PAGES=$((PAGES + 1))
  [[ -z "$CUR" || "$PAGES" -ge 10 ]] && break
done
check "following limit=1 翻页页数" 3 "$PAGES"
check "following 翻页去重后 3 个 id" 3 "$(uniq_count "$ALL_IDS")"

code=$(get "/api/user/$UID_A/following?cursor=abc" "$TOKEN_A")
check "非法 cursor → 400" 400 "$code"
code=$(get "/api/user/999999/following" "$TOKEN_A")
check "用户不存在 → 404" 404 "$code"
code=$(post "/api/user/$UID_A/following" '{}' "$TOKEN_A")
check "POST following → 404（未匹配路由）" 404 "$code"

# ---- [3/7] 粉丝列表 ---------------------------------------------------------
echo "[3/7] 粉丝列表"
code=$(get "/api/user/$UID_A/followers" "$TOKEN_A")
check "alice 的 followers" 200 "$code"
check "alice 粉丝只有 eve" "eve" "$(item_names)"
code=$(get "/api/user/$UID_C/followers" "$TOKEN_A")
check "carol 的 followers（alice/bob）" 200 "$code"
check "按关注时间倒序 bob,alice" "bob,alice" "$(item_names)"
check "alice 视角 followed_by_me（bob,alice）" "1,0" "$(item_flags)"
code=$(get "/api/user/$UID_C/followers?limit=1" "$TOKEN_A")
check "followers limit=1" 200 "$code"
check "followers 第一页 1 条" 1 "$(item_count)"
CUR_W="$(json_str next_cursor)"
code=$(get "/api/user/$UID_C/followers?limit=1&cursor=$CUR_W" "$TOKEN_A")
check "followers 第二页" 200 "$code"
check "followers 第二页 alice" "alice" "$(item_names)"

# ---- [4/7] 空状态 -----------------------------------------------------------
echo "[4/7] 空状态"
code=$(get "/api/user/$UID_D/following" "$TOKEN_D")
check "dave 没关注任何人" 200 "$code"
check "dave following items=[]" 0 "$(item_count)"
check "dave following next_cursor 空" "" "$(json_str next_cursor)"
code=$(get "/api/user/$UID_D/followers" "$TOKEN_D")
check "dave 没有粉丝" 200 "$code"
check "dave followers 只有 alice" "alice" "$(item_names)"
code=$(get "/api/user/$UID_E/following" "$TOKEN_E")
check "eve 关注了 alice（非空反向验证）" "alice" "$(item_names)"

# ---- [5/7] 拉黑过滤（双向） -------------------------------------------------
echo "[5/7] 拉黑过滤"
code=$(post "/api/user/$UID_C/block" '{}' "$TOKEN_A")
check "alice 拉黑 carol" 200 "$code"
code=$(get "/api/user/$UID_A/following" "$TOKEN_A")
check "拉黑后 following 不含 carol" "dave,bob" "$(item_names)"
check "拉黑后 following 少 1 条" 2 "$(item_count)"
code=$(get "/api/user/$UID_C/followers" "$TOKEN_A")
check "拉黑后 carol 粉丝列表（alice 视角）" 200 "$code"

code=$(post "/api/user/$UID_A/block" '{}' "$TOKEN_C")
check "carol 反拉黑 alice" 200 "$code"
code=$(get "/api/user/$UID_C/followers" "$TOKEN_A")
check "双向拉黑后 alice 看不到 carol 粉丝列表" 200 "$code"
check "双向拉黑后 carol 粉丝（bob + 查看者 alice 自己）" "bob,alice" "$(item_names)"
code=$(del "/api/user/$UID_C/block" "$TOKEN_A")
check "alice 取消拉黑 carol" 200 "$code"
code=$(del "/api/user/$UID_A/block" "$TOKEN_C")
check "carol 取消拉黑 alice" 200 "$code"
code=$(get "/api/user/$UID_A/following" "$TOKEN_A")
check "取消拉黑后恢复 3 条" "dave,carol,bob" "$(item_names)"

# ---- [6/7] 关注动态 ---------------------------------------------------------
echo "[6/7] 关注动态"
code=$(get "/api/feed/following")
check "未登录关注流 → 401" 401 "$code"
code=$(get "/api/feed/following" "$TOKEN_A")
check "alice 关注流" 200 "$code"
check "只含已关注作者作品（bob/carol 共 3 条）" 3 "$(feed_count)"
check "按发布时间倒序 b2,c1,b1" "$V_B2,$V_C1,$V_B1" "$(feed_ids)"
check "作者序列 bob,carol,bob" "bob,carol,bob" "$(feed_authors)"
check "不含 alice 自己的视频" no \
  "$(jsonq "'yes' if any(it['video']['id'] == $V_A1 for it in d['items']) else 'no'")"

code=$(get "/api/feed/following?limit=1" "$TOKEN_A")
check "关注流 limit=1" 200 "$code"
check "第一页 1 条" 1 "$(feed_count)"
CUR_G="$(json_str next_cursor)"
code=$(get "/api/feed/following?limit=1&cursor=$CUR_G" "$TOKEN_A")
check "关注流第二页" 200 "$code"
check "第二页是 c1" "$V_C1" "$(feed_ids)"

CUR=""; PAGES=0; ALL_FEED=""
while :; do
  if [[ -z "$CUR" ]]; then
    code=$(get "/api/feed/following?limit=1" "$TOKEN_A")
  else
    code=$(get "/api/feed/following?limit=1&cursor=$CUR" "$TOKEN_A")
  fi
  [[ "$code" == "200" ]] || break
  ALL_FEED="$ALL_FEED$(feed_ids),"
  CUR="$(json_str next_cursor)"
  PAGES=$((PAGES + 1))
  [[ -z "$CUR" || "$PAGES" -ge 10 ]] && break
done
check "关注流 limit=1 翻页页数" 3 "$PAGES"
check "关注流翻页去重后 3 条" 3 "$(uniq_count "$ALL_FEED")"

code=$(get "/api/feed/following?cursor=abc" "$TOKEN_A")
check "关注流非法 cursor → 400" 400 "$code"
code=$(get "/api/feed/following" "$TOKEN_D")
check "dave 空关注流" 200 "$code"
check "dave 关注流 0 条" 0 "$(feed_count)"
code=$(get "/api/feed/following" "$TOKEN_E")
check "eve 关注流只有 alice 的 a1" 1 "$(feed_count)"

code=$(post "/api/user/$UID_B/block" '{}' "$TOKEN_A")
check "alice 拉黑 bob" 200 "$code"
code=$(get "/api/feed/following" "$TOKEN_A")
check "拉黑后关注流只剩 carol 1 条" 1 "$(feed_count)"
check "拉黑后关注流是 c1" "$V_C1" "$(feed_ids)"
code=$(del "/api/user/$UID_B/block" "$TOKEN_A")
check "取消拉黑 bob" 200 "$code"
code=$(get "/api/feed/following" "$TOKEN_A")
check "恢复后关注流 3 条" 3 "$(feed_count)"

# ---- 前端 ------------------------------------------------------------------
echo "[7/7] 前端与性能"
code=$(get "/")
check "GET /" 200 "$code"
for kw in 'id="followPanel"' 'function openFollowList' 'class="p-stat-link"' \
          'follow-btn-sm' '你还没有关注任何人' 'TA 还没有粉丝' \
          'api/feed/following?limit=20' '关注的人还没有作品'; do
  check "首页包含 $kw" yes "$(grep -q -- "$kw" "$TMP/body" && echo yes || echo no)"
done

max_ttfb() { # path token
  local path="$1" token="${2:-}" max=0 v
  for _ in $(seq 1 20); do
    v="$(ttfb "$path" "$token")"
    awk -v a="$v" -v b="$max" 'BEGIN{exit !(a>b)}' && max="$v"
  done
  awk -v v="$max" 'BEGIN{printf "%d", v*1000}'
}
MS_FOLLOW="$(max_ttfb "/api/user/$UID_A/following?limit=20" "$TOKEN_A")"
MS_FANS="$(max_ttfb "/api/user/$UID_C/followers?limit=20" "$TOKEN_A")"
MS_FEED="$(max_ttfb "/api/feed/following?limit=10" "$TOKEN_A")"
check "following TTFB < 200ms（实测 ${MS_FOLLOW}ms）" yes \
  "$([[ "$MS_FOLLOW" -lt 200 ]] && echo yes || echo no)"
check "followers TTFB < 200ms（实测 ${MS_FANS}ms）" yes \
  "$([[ "$MS_FANS" -lt 200 ]] && echo yes || echo no)"
check "关注流 TTFB < 200ms（实测 ${MS_FEED}ms）" yes \
  "$([[ "$MS_FEED" -lt 200 ]] && echo yes || echo no)"

EXPLAIN_OUT="$(python3 - "$DB" <<'PY'
import sqlite3
import sys

con = sqlite3.connect(sys.argv[1])
sqls = [
    ("following", "SELECT u.id, u.name, u.avatar_url, u.bio, f.created_at "
     "FROM follows f JOIN users u ON u.id = f.followee_id "
     "WHERE f.follower_id = ? AND u.id NOT IN "
     "(SELECT blocked_id FROM blocks WHERE user_id = ?) "
     "AND u.id NOT IN (SELECT user_id FROM blocks WHERE blocked_id = ?) "
     "ORDER BY f.created_at DESC, u.id DESC LIMIT 21"),
    ("followers", "SELECT u.id, u.name, u.avatar_url, u.bio, f.created_at "
     "FROM follows f JOIN users u ON u.id = f.follower_id "
     "WHERE f.followee_id = ? AND u.id NOT IN "
     "(SELECT blocked_id FROM blocks WHERE user_id = ?) "
     "AND u.id NOT IN (SELECT user_id FROM blocks WHERE blocked_id = ?) "
     "ORDER BY f.created_at DESC, u.id DESC LIMIT 21"),
]
for name, sql in sqls:
    print("[" + name + "]")
    for row in con.execute("EXPLAIN QUERY PLAN " + sql, (1, 1, 1)):
        print(row[3])
PY
)"
check "following 计划命中 idx_follows_follower_time" yes \
  "$(grep -q "idx_follows_follower_time" <<<"$EXPLAIN_OUT" && echo yes || echo no)"
check "followers 计划命中 idx_follows_followee_time" yes \
  "$(grep -q "idx_follows_followee_time" <<<"$EXPLAIN_OUT" && echo yes || echo no)"
check "列表计划不出现全表扫描 follows" no \
  "$(grep -q "SCAN follows" <<<"$EXPLAIN_OUT" && echo yes || echo no)"

code=$(get "/api/user/$UID_A")
check "回归 /api/user/:id" 200 "$code"
check "资料页关注数=3" 3 "$(jsonq "d['user']['following']")"
check "资料页粉丝数=1" 1 "$(jsonq "d['user']['followers']")"
code=$(get "/api/feed?limit=10")
check "回归 /api/feed" 200 "$code"

echo "---------------------------------------------"
printf '[accept_follow_lists] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_follow_lists] PASS"
  exit 0
fi
echo "[accept_follow_lists] FAIL"
exit 1
