#!/usr/bin/env bash
# 搜索增强验收（第 11 轮新增）：搜索历史 + 热搜榜 + 搜索建议。
#
# 覆盖：
#   [1]  准备：启动 user(29101) / video(29102) / search(29106) / gateway(28080)，
#        注册 3 个用户、发布 4 条带话题视频、检查 search_history 表与索引
#   [2]  历史：401（读/删）、搜索自动记录、倒序、同词去重按最近、
#        用户隔离、单条删除、重复删除幂等、清空、limit
#   [3]  热搜：公开可读、跨用户聚合、排序、limit、7 天过滤、索引命中、
#        单条 GROUP BY、count 与 SQL 一致
#   [4]  建议：用户昵称 / 热搜词 / 话题三个来源、前缀严格、并集去重、
#        limit 上限、空 q、`%` 字面量、匿名可用
#   [5]  前端：GET / 200、历史/热搜/建议元素、函数与 200ms 防抖、JS 括号平衡
#   [6]  性能：热搜 / 历史 / 建议 TTFB 各 20 次均值 < 200ms、首页 200
#
# 进程管理：pids 写入 logs/accept_search_cluster.pids，退出时校验 exe 后 kill。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_search_cluster.pids"
DB="$ROOT/data/accept_search_cluster.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0

GW_PORT="${DOUYIN_GATEWAY_PORT:-28080}"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
VIDEO_PORT="${DOUYIN_VIDEO_PORT:-29102}"
SEARCH_PORT="${DOUYIN_SEARCH_PORT:-29106}"
GW="http://127.0.0.1:$GW_PORT"
USER="http://127.0.0.1:$USER_PORT"
VIDEO="http://127.0.0.1:$VIDEO_PORT"
SEARCH="http://127.0.0.1:$SEARCH_PORT"

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
    printf '  %-46s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-46s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -qF -- "$pattern" "$TMP/body"; then
    printf '  %-46s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-46s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

expect_no_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -qF -- "$pattern" "$TMP/body"; then
    printf '  %-46s FAIL  响应不应含 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  else
    printf '  %-46s PASS  响应不含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  fi
}

post() { # base path json [token]
  local base="$1" path="$2" payload="$3" token="${4:-}"
  local args=(-s --max-time 10 -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$payload" "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

get() { # base path [token]
  local base="$1" path="$2" token="${3:-}"
  local args=(-s --max-time 10 -o "$TMP/body" -w '%{http_code}' "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

del() { # base path [token]
  local base="$1" path="$2" token="${3:-}"
  local args=(-s --max-time 10 -o "$TMP/body" -w '%{http_code}' -X DELETE
              "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { # field
  grep -o "\"$1\":-\?[0-9]*" "$TMP/body" | head -1 | grep -o -- '-\?[0-9]*$'
}

json_str() { # field
  sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1
}

hist_count() { grep -o '"keyword":"[^"]*"' "$TMP/body" | wc -l | tr -d ' '; }
hist_keywords() { grep -o '"keyword":"[^"]*"' "$TMP/body" | sed 's/^"keyword":"//;s/"$//'; }
hot_keywords() { grep -o '"keyword":"[^"]*"' "$TMP/body" | sed 's/^"keyword":"//;s/"$//'; }
suggest_count() {
  python3 - "$TMP/body" <<'PY'
import json,sys
try:
    data = json.load(open(sys.argv[1], encoding="utf-8"))
    print(len(data.get("items", [])))
except Exception:
    print(-1)
PY
}
suggest_words() {
  python3 - "$TMP/body" <<'PY'
import json,sys
try:
    data = json.load(open(sys.argv[1], encoding="utf-8"))
    print("\n".join(data.get("items", [])))
except Exception:
    pass
PY
}
search_q() { # token keyword -> 200
  get "$GW" "/api/search?q=$(python3 -c 'import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))' "$2")" "$1"
}

sql_value() { # sql
  python3 -c 'import sqlite3,sys
con = sqlite3.connect(sys.argv[1], timeout=5)
row = con.execute(sys.argv[2]).fetchone()
print(row[0] if row and row[0] is not None else "")' "$DB" "$1"
}

sql_exec() { # sql
  python3 -c 'import sqlite3,sys
con = sqlite3.connect(sys.argv[1], timeout=5)
con.execute(sys.argv[2])
con.commit()' "$DB" "$1"
}

sql_plan() { # sql
  python3 -c 'import sqlite3,sys
con = sqlite3.connect(sys.argv[1], timeout=5)
for row in con.execute("EXPLAIN QUERY PLAN " + sys.argv[2]):
    print(row[3])' "$DB" "$1" 2>/dev/null
}

wait_ready() { # base name
  local base="$1" name="$2"
  for _ in $(seq 1 200); do
    if curl -fsS --max-time 1 "$base/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  echo "$name 启动失败，日志：" >&2
  tail -20 "$LOG_DIR/accept_search_cluster_$name.log" >&2
  return 1
}

start_service() { # bin port base name
  local bin="$1" port="$2" base="$3" name="$4"
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_search_cluster_$name.log" 2>&1 &
  local pid=$!
  printf '%s %s %s\n' "$name" "$pid" "$port" >>"$PID_FILE"
  wait_ready "$base" "$name" || exit 1
  printf '[start] %-16s %s pid=%s\n' "$name" "$base" "$pid"
}

declare -A TOKENS=()
declare -A IDS=()

register() { # name
  local name="$1" code
  code=$(post "$USER" /api/user/register \
    "{\"name\":\"$name\",\"email\":\"$name@search11.com\",\"password\":\"secret123\"}")
  if [[ "$code" != "200" ]]; then
    printf '注册 %s 失败 HTTP %s：%s\n' "$name" "$code" "$(head -c 200 "$TMP/body")" >&2
    exit 1
  fi
  TOKENS[$name]=$(json_str token)
  IDS[$name]=$(json_int id)
  if [[ -z "${TOKENS[$name]}" || -z "${IDS[$name]}" ]]; then
    echo "token/user id 提取失败：$name" >&2
    exit 1
  fi
}

publish() { # token title
  local code
  code=$(post "$GW" /api/video "{\"title\":\"$2\",\"url\":\"videos/demo1.mp4\"}" "$1")
  if [[ "$code" != "200" && "$code" != "201" ]]; then
    printf '发布视频失败 HTTP %s：%s\n' "$code" "$(head -c 200 "$TMP/body")" >&2
    exit 1
  fi
}

echo "[1/6] 准备：启动 user/video/search/gateway，注册用户并发布视频"
for bin in user_service video_service search_service gateway; do
  if [[ ! -x "$BIN_DIR/$bin" ]]; then
    echo "找不到 $BIN_DIR/$bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
done
rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_search_cluster_"*.log
for base in "$USER" "$VIDEO" "$SEARCH" "$GW"; do
  if curl -fsS --max-time 1 "$base/health" >/dev/null 2>&1; then
    echo "端口已被占用：$base（请先停止旧服务再跑本脚本）" >&2
    exit 1
  fi
done
start_service "$BIN_DIR/user_service" "$USER_PORT" "$USER" user_service
start_service "$BIN_DIR/video_service" "$VIDEO_PORT" "$VIDEO" video_service
start_service "$BIN_DIR/search_service" "$SEARCH_PORT" "$SEARCH" search_service
start_service "$BIN_DIR/gateway" "$GW_PORT" "$GW" gateway

for user in alice bob carol; do
  register "$user"
done
check "注册用户数" 3 "${#TOKENS[@]}"

publish "${TOKENS[alice]}" "周末吃什么 #美食"
publish "${TOKENS[alice]}" "山间徒步 #旅行"
publish "${TOKENS[alice]}" "深夜食堂 #美食"
publish "${TOKENS[bob]}" "海边日落 #旅行"
VIDEOS=$(sql_value "SELECT COUNT(*) FROM videos")
check "视频条数" 4 "$VIDEOS"
check "search_history 表已建" 1 \
  "$(sql_value "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='search_history'")"
check "历史索引 idx_search_history_user" 1 \
  "$(sql_value "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_search_history_user'")"
check "热搜索引 idx_search_history_created" 1 \
  "$(sql_value "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_search_history_created'")"

echo "[2/6] 搜索历史：记录 / 去重 / 隔离 / 删除 / 清空"
code=$(get "$GW" /api/search/history)
check "无 token 读历史" 401 "$code"
code=$(del "$GW" /api/search/history)
check "无 token 清空历史" 401 "$code"
code=$(search_q "${TOKENS[alice]}" "美食")
check "alice 搜索 美食" 200 "$code"
code=$(get "$GW" /api/search/history "${TOKENS[alice]}")
check "搜索后自动记录历史" 1 "$(hist_count)"
check "历史词正确" "美食" "$(hist_keywords | head -1)"
check "历史 created_at > 0" 1 \
  "$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(1 if d["items"][0]["created_at"] > 0 else 0)' "$TMP/body")"
code=$(search_q "${TOKENS[alice]}" "旅行")
check "alice 再搜 旅行" 200 "$code"
code=$(get "$GW" /api/search/history "${TOKENS[alice]}")
check "历史 2 条" 2 "$(hist_count)"
check "最近搜索排最前" "旅行" "$(hist_keywords | head -1)"
code=$(search_q "${TOKENS[alice]}" "美食")
code=$(get "$GW" /api/search/history "${TOKENS[alice]}")
check "同词去重仍 2 条" 2 "$(hist_count)"
check "重复搜索提到最前" "美食" "$(hist_keywords | head -1)"
code=$(search_q "${TOKENS[bob]}" "美食")
code=$(get "$GW" /api/search/history "${TOKENS[bob]}")
check "bob 历史 1 条" 1 "$(hist_count)"
code=$(get "$GW" /api/search/history "${TOKENS[alice]}")
check "alice 历史不受 bob 影响" 2 "$(hist_count)"
code=$(get "$GW" /api/search/history "${TOKENS[carol]}")
check "carol 未搜索历史为空" 0 "$(hist_count)"
code=$(del "$GW" "/api/search/history?keyword=$(python3 -c 'import urllib.parse;print(urllib.parse.quote("美食"))')" "${TOKENS[alice]}")
check "删除单条历史" 200 "$code"
check "删除条数 1" 1 "$(json_int deleted)"
code=$(get "$GW" /api/search/history "${TOKENS[alice]}")
check "删除后剩 1 条" 1 "$(hist_count)"
check "剩下的是旅行" "旅行" "$(hist_keywords | head -1)"
code=$(del "$GW" "/api/search/history?keyword=$(python3 -c 'import urllib.parse;print(urllib.parse.quote("美食"))')" "${TOKENS[alice]}")
check "重复删除不报错" 200 "$code"
check "重复删除条数 0" 0 "$(json_int deleted)"
code=$(search_q "${TOKENS[alice]}" "美食")
code=$(del "$GW" /api/search/history "${TOKENS[alice]}")
check "清空历史" 200 "$code"
code=$(get "$GW" /api/search/history "${TOKENS[alice]}")
check "清空后 0 条" 0 "$(hist_count)"
code=$(search_q "${TOKENS[carol]}" "美食")
code=$(search_q "${TOKENS[carol]}" "旅行")
code=$(get "$GW" "/api/search/history?limit=1" "${TOKENS[carol]}")
check "limit=1 只回 1 条" 1 "$(hist_count)"
code=$(get "$GW" "/api/search/history?limit=abc" "${TOKENS[carol]}")
check "limit 非法回退默认" 200 "$code"

echo "[3/6] 热搜：跨用户聚合 / 排序 / 7 天过滤 / 索引"
# alice 的历史在 [2] 末尾被清空过；重新搜一次，让「美食」有 3 个不同用户。
code=$(search_q "${TOKENS[alice]}" "美食")
code=$(get "$GW" "/api/search/hot?limit=10")
check "热搜公开可读" 200 "$code"
code=$(search_q "${TOKENS[carol]}" "美食")
code=$(get "$GW" "/api/search/hot?limit=10")
HOT1=$(hot_keywords | head -1)
check "热搜第一是美食" "美食" "$HOT1"
check "美食 count=3（3 个用户各 1 次）" 3 \
  "$(grep -o '"keyword":"美食","count":[0-9]*' "$TMP/body" | head -1 | grep -o '[0-9]*$')"
code=$(get "$GW" "/api/search/hot?limit=1")
check "热搜 limit=1" 1 "$(hot_keywords | wc -l | tr -d ' ')"
code=$(search_q "${TOKENS[bob]}" "冷门词")
code=$(get "$GW" "/api/search/hot?limit=10")
check "冷门词上榜" 1 "$(hot_keywords | grep -c '冷门词' | tr -d ' ')"
WINDOW_SQL="SELECT COUNT(*) FROM search_history WHERE created_at >= strftime('%s','now') - 7*86400"
check "热搜 SQL 与 7 天窗口 count 一致" \
  "$(sql_value "$WINDOW_SQL")" \
  "$(sql_value "SELECT COALESCE(SUM(c),0) FROM (SELECT COUNT(*) AS c FROM search_history WHERE created_at >= strftime('%s','now') - 7*86400 GROUP BY keyword)")"
sql_exec "UPDATE search_history SET created_at = strftime('%s','now') - 8*86400 WHERE keyword = '冷门词'"
code=$(get "$GW" "/api/search/hot?limit=10")
check "8 天前的搜索不进热搜" 0 "$(hot_keywords | grep -c '冷门词' || true)"
PLAN=$(sql_plan "SELECT keyword, COUNT(*) FROM search_history WHERE created_at >= 1 GROUP BY keyword ORDER BY COUNT(*) DESC LIMIT 10")
if echo "$PLAN" | grep -qE 'idx_search_history_created|USING COVERING INDEX'; then
  printf '  %-46s PASS  %s\n' "热搜查询命中时间索引" "$(echo "$PLAN" | head -1)"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  计划：%s\n' "热搜查询命中时间索引" "$(echo "$PLAN" | tr '\n' ' ')"
  FAILED=$((FAILED + 1))
fi
if grep -q "GROUP BY keyword" "$ROOT/src/douyin_store.cpp" 2>/dev/null; then
  printf '  %-46s PASS  存储层单条 GROUP BY\n' "热搜聚合实现"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  存储层缺少 GROUP BY\n' "热搜聚合实现"
  FAILED=$((FAILED + 1))
fi
code=$(get "$GW" "/api/search/hot?limit=999")
check "热搜 limit 夹紧 20" 200 "$code"
code=$(post "$GW" /api/search/hot '{}')
check "热搜 POST 405" 405 "$code"

echo "[4/6] 搜索建议：三源并集 / 前缀 / 去重 / 边界"
code=$(get "$GW" "/api/search/suggest?q=ali&limit=8")
check "建议匿名可用" 200 "$code"
SUG=$(suggest_words)
if echo "$SUG" | grep -qx 'alice'; then
  printf '  %-46s PASS  建议含用户 alice\n' "用户昵称前缀匹配"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  建议：%s\n' "用户昵称前缀匹配" "$(echo "$SUG" | tr '\n' ' ')"
  FAILED=$((FAILED + 1))
fi
code=$(get "$GW" "/api/search/suggest?q=$(python3 -c 'import urllib.parse;print(urllib.parse.quote("美"))')")
SUG=$(suggest_words)
if echo "$SUG" | grep -qx '美食'; then
  printf '  %-46s PASS  建议含热搜词 美食\n' "热搜词前缀匹配"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  建议：%s\n' "热搜词前缀匹配" "$(echo "$SUG" | tr '\n' ' ')"
  FAILED=$((FAILED + 1))
fi
code=$(get "$GW" "/api/search/suggest?q=$(python3 -c 'import urllib.parse;print(urllib.parse.quote("旅"))')")
SUG=$(suggest_words)
if echo "$SUG" | grep -qx '旅行'; then
  printf '  %-46s PASS  建议含话题 旅行\n' "话题前缀匹配"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  建议：%s\n' "话题前缀匹配" "$(echo "$SUG" | tr '\n' ' ')"
  FAILED=$((FAILED + 1))
fi
# 去重：注册昵称「旅行」的用户并搜索「旅行」，建议中「旅行」只能出现一次。
register "旅行"
code=$(search_q "${TOKENS[旅行]}" "旅行")
code=$(get "$GW" "/api/search/suggest?q=$(python3 -c 'import urllib.parse;print(urllib.parse.quote("旅"))')")
COUNT=$(suggest_words | grep -cx '旅行')
check "三源并集去重（旅行只 1 次）" 1 "$COUNT"
code=$(get "$GW" "/api/search/suggest?q=$(python3 -c 'import urllib.parse;print(urllib.parse.quote("食"))')")
suggest_words >"$TMP/sug"
if grep -qx '美食' "$TMP/sug"; then
  printf '  %-46s FAIL  前缀严格（食 不应命中 美食）\n' "前缀严格匹配"
  FAILED=$((FAILED + 1))
else
  printf '  %-46s PASS\n' "前缀严格匹配（食 不命中 美食）"
  PASSED=$((PASSED + 1))
fi
code=$(get "$GW" "/api/search/suggest?q=")
check "空 q 返回空列表" 0 "$(suggest_count)"
code=$(get "$GW" "/api/search/suggest?q=%25")
check "% 按字面量转义" 0 "$(suggest_count)"
code=$(get "$GW" "/api/search/suggest?q=ali&limit=1")
check "建议 limit=1" 1 "$(suggest_count)"
code=$(get "$GW" "/api/search/suggest?q=ali&limit=999")
check "建议 limit 夹紧 20" 200 "$code"
code=$(post "$GW" "/api/search/suggest?q=ali" '{}')
check "建议 POST 405" 405 "$code"

echo "[5/6] 前端元素与 JS 检查"
code=$(get "$GW" /)
check "GET /" 200 "$code"
curl -s --max-time 5 -o "$TMP/page.html" "$GW/"
for needle in 'id="searchDiscover"' 'id="shClear"' 'id="shList"' 'id="hotList"' \
              'id="searchSuggest"' 'function loadSearchHistory' 'function loadHotSearch' \
              'function requestSuggest' 'function deleteHistory' 'function pickSearch' \
              'function clearHistory' '🕘 搜索历史' '🔥 热搜'; do
  if grep -qF -- "$needle" "$TMP/page.html"; then
    printf '  %-46s PASS  %s\n' "页面含" "$needle"
    PASSED=$((PASSED + 1))
  else
    printf '  %-46s FAIL  页面缺少 %s\n' "页面含" "$needle"
    FAILED=$((FAILED + 1))
  fi
done
if grep -qF '}, 200);' "$TMP/page.html"; then
  printf '  %-46s PASS\n' "建议 200ms 防抖"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL\n' "建议 200ms 防抖"
  FAILED=$((FAILED + 1))
fi
JSCHECK_RC=0
python3 - "$TMP/page.html" >"$TMP/jscheck" 2>&1 <<'PY' || JSCHECK_RC=1
import re
import sys

REGEX_PREV = set("([{,;:=!&|?+-*%<>~^")
REGEX_WORDS = {"return", "case", "typeof", "delete", "void", "in", "of", "new", "do", "else"}
html = open(sys.argv[1], encoding="utf-8", errors="replace").read()
scripts = re.findall(r"<script[^>]*>(.*?)</script>", html, re.S)
if not scripts:
    print("没有找到 <script>")
    sys.exit(1)


def blank(s):
    return "".join("\n" if ch == "\n" else " " for ch in s)


def strip(src):
    out = []
    i = 0
    n = len(src)
    prev = ""
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(src[i:j]))
            i = j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(src[i:j]))
            i = j
            continue
        if c in ("'", '"', "`"):
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    j += 1
                    break
                j += 1
            out.append(blank(src[i:j]))
            i = j
            prev = "x"
            continue
        if c == "/":
            k = i - 1
            while k >= 0 and src[k].isspace():
                k -= 1
            e = k + 1
            while k >= 0 and (src[k].isalnum() or src[k] in "_$"):
                k -= 1
            word = src[k + 1:e]
            if prev in REGEX_PREV or word in REGEX_WORDS:
                j = i + 1
                in_class = False
                while j < n:
                    ch = src[j]
                    if ch == "\\":
                        j += 2
                        continue
                    if ch == "[":
                        in_class = True
                    elif ch == "]":
                        in_class = False
                    elif ch == "/" and not in_class:
                        j += 1
                        while j < n and src[j].isalpha():
                            j += 1
                        break
                    elif ch == "\n":
                        break
                    j += 1
                out.append(blank(src[i:j]))
                i = j
                prev = "x"
                continue
        out.append(c)
        if not c.isspace():
            prev = c
        i += 1
    return "".join(out)


pairs = {")": "(", "]": "[", "}": "{"}
for idx, s in enumerate(scripts):
    stack = []
    clean = strip(s)
    for pos, ch in enumerate(clean):
        if ch in "([{":
            stack.append((ch, pos))
        elif ch in ")]}":
            if not stack or stack[-1][0] != pairs[ch]:
                print(f"script[{idx}] 位置 {pos} 的 {ch} 不匹配")
                sys.exit(1)
            stack.pop()
    if stack:
        print(f"script[{idx}] 还有 {len(stack)} 个未闭合括号，最后 {stack[-1]}")
        sys.exit(1)
print(f"JSCHECK ok scripts={len(scripts)}")
PY
check "内联 JS 括号平衡" 0 "$JSCHECK_RC"
cat "$TMP/jscheck"

echo "[6/6] 性能：TTFB 20 次均值"
ttfb() { # url [token]
  local total=0 i code t
  for i in $(seq 1 20); do
    if [[ -n "${2:-}" ]]; then
      t=$(curl -s -o /dev/null --max-time 5 -H "Authorization: Bearer $2" \
            -w '%{time_starttransfer}' "$1")
    else
      t=$(curl -s -o /dev/null --max-time 5 -w '%{time_starttransfer}' "$1")
    fi
    total=$(python3 -c "print($total + $t)")
  done
  python3 -c "print(round($total / 20 * 1000, 2))"
}
HOT_MS=$(ttfb "$GW/api/search/hot?limit=10")
check "热搜 TTFB 均值 < 200ms" 1 "$(python3 -c "print(1 if $HOT_MS < 200 else 0)")"
printf '    热搜 TTFB = %s ms\n' "$HOT_MS"
HIST_MS=$(ttfb "$GW/api/search/history?limit=10" "${TOKENS[carol]}")
check "历史 TTFB 均值 < 200ms" 1 "$(python3 -c "print(1 if $HIST_MS < 200 else 0)")"
printf '    历史 TTFB = %s ms\n' "$HIST_MS"
SUG_MS=$(ttfb "$GW/api/search/suggest?q=ali&limit=8")
check "建议 TTFB 均值 < 200ms" 1 "$(python3 -c "print(1 if $SUG_MS < 200 else 0)")"
printf '    建议 TTFB = %s ms\n' "$SUG_MS"
PAGE_MS=$(ttfb "$GW/")
check "首页 TTFB 均值 < 200ms" 1 "$(python3 -c "print(1 if $PAGE_MS < 200 else 0)")"
printf '    首页 TTFB = %s ms\n' "$PAGE_MS"

echo
printf '结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
[[ "$FAILED" -eq 0 ]]
