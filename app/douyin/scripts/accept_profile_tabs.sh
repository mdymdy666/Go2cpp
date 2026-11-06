#!/usr/bin/env bash
# 个人主页三 tab（作品 / 喜欢 / 收藏）验收（第 5 轮新增）。
#
# 用例分组：
#   [1] 作品 tab：列表 / 总数 / 倒序 / 作者名 / 分页 / 非法游标 / 404 / 405 / 空主页
#   [2] 喜欢 tab：本人可见全部 + 按点赞时间倒序；他人与匿名 private 空列表（不泄露内容）
#   [3] 收藏 tab：本人可见 + 按收藏时间倒序 + 跨作者 author；他人/匿名 private；分页
#   [4] 拉黑过滤：被拉黑作者不进入 liked/favorites；作者拉黑 viewer 时主页作品为空
#   [5] 前端：GET / 200 且含三 tab 容器 / 加载函数 / 锁文案 / 空状态文案
#   [6] 性能：liked/favorites 各 20 次 TTFB < 200 ms；EXPLAIN 命中 idx_likes_user
#   [7] 回归：/api/feed、/api/video/mine、/api/video/favorites 仍 200
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_profile_tabs.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_profile_tabs.pids"
DB="$ROOT/data/accept_profile_tabs.db"
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

# ---- 断言 -------------------------------------------------------------------
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

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-52s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-52s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

expect_contains() { # name pattern [file]
  local name="$1" pattern="$2" file="${3:-$TMP/body}"
  if grep -q -- "$pattern" "$file"; then
    printf '  %-52s PASS  含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-52s FAIL  缺少 %s\n' "$name" "$pattern"
    FAILED=$((FAILED + 1))
  fi
}

expect_absent() { # name pattern [file]
  local name="$1" pattern="$2" file="${3:-$TMP/body}"
  if grep -q -- "$pattern" "$file"; then
    printf '  %-52s FAIL  不应出现 %s\n' "$name" "$pattern"
    FAILED=$((FAILED + 1))
  else
    printf '  %-52s PASS  不含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  fi
}

# ---- HTTP 封装（全部指向网关） ----------------------------------------------
post() { # path json [token] [outfile]
  local path="$1" payload="$2" token="${3:-}" out="${4:-$TMP/body}"
  local args=(-s --max-time 5 -o "$out" -w '%{http_code}' -X POST
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

del() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X DELETE "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { grep -o "\"$1\":[0-9]*" "$TMP/body" | head -1 | cut -d: -f2; }
json_bool() { grep -o "\"$1\":\(true\|false\)" "$TMP/body" | head -1 | cut -d: -f2; }
json_str() {
  grep -o "\"$1\":\"[^\"]*\"" "$TMP/body" | head -1 | \
    sed "s/^\"$1\":\"//; s/\"$//"
}
video_id() { sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }
user_id_of() { sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }
items_count() { grep -o '"video":{"id":[0-9]*' "$TMP/body" | wc -l; }
ids_csv() { grep -o '"video":{"id":[0-9]*' "$TMP/body" | sed 's/.*://' | paste -sd, -; }
titles_csv() {
  grep -o '"title":"[^"]*"' "$TMP/body" | sed 's/^"title":"//; s/"$//' | paste -sd, -
}
uniq_count() { tr ',' '\n' <<<"$1" | sed '/^$/d' | sort -u | wc -l; }

# ---- 启动服务（串行等待 health，避免同库并发建表） --------------------------
start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_profile_tabs_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_profile_tabs_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_profile_tabs_"*.log

echo "[0/7] 启动 7 个进程（6 业务 + gateway）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

# ---- 准备数据：4 个用户 / 5 条视频 / 3 次点赞 / 2 次收藏 ---------------------
register() { # name email -> token via $TMP/reg
  post "/api/user/register" \
    "{\"name\":\"$1\",\"email\":\"$2\",\"password\":\"secret123\"}" >/dev/null
  json_str token
}

echo "[1/7] 准备数据（注册 alice/bob/carol/dave，发 5 条视频，点赞/收藏）"
TOKEN_A="$(register alice alice@example.com)"; UID_A="$(user_id_of)"
TOKEN_B="$(register bob bob@example.com)"; UID_B="$(user_id_of)"
TOKEN_C="$(register carol carol@example.com)"; UID_C="$(user_id_of)"
TOKEN_D="$(register dave dave@example.com)"; UID_D="$(user_id_of)"
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" || -z "$TOKEN_C" || -z "$TOKEN_D" ]]; then
  echo "注册失败（A=$TOKEN_A B=$TOKEN_B C=$TOKEN_C D=$TOKEN_D）" >&2
  exit 1
fi
expect_eq "四个用户 id 互不相同" 4 \
  "$(printf '%s\n' "$UID_A" "$UID_B" "$UID_C" "$UID_D" | sort -u | wc -l)"

mkvideo() { # token title
  post "/api/video" \
    "{\"title\":\"$2\",\"url\":\"http://example.com/$2.mp4\"}" "$1" >/dev/null
  video_id
}
V_A1="$(mkvideo "$TOKEN_A" a1)"
V_A2="$(mkvideo "$TOKEN_A" a2)"
V_A3="$(mkvideo "$TOKEN_A" a3)"
V_B1="$(mkvideo "$TOKEN_B" b1)"
V_C1="$(mkvideo "$TOKEN_C" c1)"
if [[ -z "$V_A1" || -z "$V_A2" || -z "$V_A3" || -z "$V_B1" || -z "$V_C1" ]]; then
  echo "发视频失败" >&2
  exit 1
fi
expect_eq "alice 3 条 + bob/carol 各 1 条" 5 \
  "$(printf '%s\n' "$V_A1" "$V_A2" "$V_A3" "$V_B1" "$V_C1" | sort -u | wc -l)"

like() { # token video
  post "/api/like" "{\"video_id\":$2}" "$1" >/dev/null
}
favorite() { # token video（切换）
  post "/api/video/$2/favorite" '{}' "$1" >/dev/null
}
# 点赞/收藏之间隔 1.1 s，让 created_at 落在不同秒，才能验证「按动作时间倒序」。
like "$TOKEN_A" "$V_A1"; sleep 1.1
like "$TOKEN_A" "$V_C1"; sleep 1.1
like "$TOKEN_A" "$V_B1"; sleep 1.1
favorite "$TOKEN_A" "$V_A2"; sleep 1.1
favorite "$TOKEN_A" "$V_C1"
like "$TOKEN_B" "$V_A1"
echo "      视频：alice a1=$V_A1 a2=$V_A2 a3=$V_A3，bob b1=$V_B1，carol c1=$V_C1"

# ---- [2/7] 作品 tab ---------------------------------------------------------
echo "[2/7] 作品 tab"
code=$(get "/api/user/$UID_A/videos")
check "匿名 GET alice/works" 200 "$code"
expect_eq "works tab 名" works "$(json_str tab)"
expect_eq "works total=3" 3 "$(json_int total)"
expect_eq "works items=3" 3 "$(items_count)"
expect_eq "works 按发布时间倒序（同秒按 id 倒序）" "a3,a2,a1" "$(titles_csv)"
expect_eq "works 首个作者=alice" alice \
  "$(grep -o '"author":"[^"]*"' "$TMP/body" | head -1 | cut -d'"' -f4)"

code=$(get "/api/user/$UID_A/videos?limit=2")
check "works limit=2 第一页" 200 "$code"
CUR_A="$(json_str next_cursor)"
expect_eq "works 第一页 2 条" 2 "$(items_count)"
expect_eq "works 第一页有下一页游标" yes "$([[ -n "$CUR_A" ]] && echo yes || echo no)"
PAGE1_IDS="$(ids_csv)"
code=$(get "/api/user/$UID_A/videos?limit=2&cursor=$CUR_A")
check "works 第二页（带游标）" 200 "$code"
expect_eq "works 第二页 1 条" 1 "$(items_count)"
expect_eq "works 第二页无下一页" "" "$(json_str next_cursor)"

# limit=1 逐页翻完，验证不漏不重。
CUR=""; PAGES=0; ALL_IDS=""
while :; do
  if [[ -z "$CUR" ]]; then
    code=$(get "/api/user/$UID_A/videos?limit=1")
  else
    code=$(get "/api/user/$UID_A/videos?limit=1&cursor=$CUR")
  fi
  [[ "$code" == "200" ]] || break
  ALL_IDS="$ALL_IDS$(ids_csv),"
  CUR="$(json_str next_cursor)"
  PAGES=$((PAGES + 1))
  [[ -z "$CUR" || "$PAGES" -ge 10 ]] && break
done
expect_eq "works limit=1 翻页页数" 3 "$PAGES"
expect_eq "works 翻页去重后 3 条" 3 "$(uniq_count "$ALL_IDS")"

code=$(get "/api/user/$UID_A/videos" "$TOKEN_B")
check "bob 看 alice/works（公开）" 200 "$code"
expect_eq "他人视角 works total=3" 3 "$(json_int total)"
expect_eq "他人视角 works private=false" false "$(json_bool private)"

code=$(get "/api/user/$UID_D/videos" "$TOKEN_D")
check "dave 空主页 works" 200 "$code"
expect_eq "空主页 total=0" 0 "$(json_int total)"
expect_eq "空主页 items=0" 0 "$(items_count)"

code=$(get "/api/user/999999/videos")
check "不存在的用户 works" 404 "$code"
code=$(get "/api/user/$UID_A/videos?cursor=abc")
check "works 非法游标" 400 "$code"
code=$(post "/api/user/$UID_A/videos" '{}')
check "POST works" 405 "$code"
code=$(post "/api/user/$UID_A/videos" '{}' "$TOKEN_A")
check "带 token POST works" 405 "$code"

# ---- [3/7] 喜欢 tab（隐私 + 排序 + 分页） -----------------------------------
echo "[3/7] 喜欢 tab"
code=$(get "/api/user/$UID_A/liked" "$TOKEN_A")
check "本人 GET liked" 200 "$code"
expect_eq "liked tab 名" liked "$(json_str tab)"
expect_eq "本人 liked private=false" false "$(json_bool private)"
expect_eq "本人 liked total=3" 3 "$(json_int total)"
expect_eq "本人 liked items=3" 3 "$(items_count)"
expect_eq "liked 按点赞时间倒序" "b1,c1,a1" "$(titles_csv)"
expect_eq "liked 首条作者=bob" bob \
  "$(grep -o '"author":"[^"]*"' "$TMP/body" | head -1 | cut -d'"' -f4)"

code=$(get "/api/user/$UID_A/liked" "$TOKEN_B")
check "bob 看 alice/liked" 200 "$code"
expect_eq "他人 liked private=true" true "$(json_bool private)"
expect_eq "他人 liked total=0" 0 "$(json_int total)"
expect_eq "他人 liked items=0" 0 "$(items_count)"
expect_contains "他人 liked 带提示" "喜欢列表仅本人可见"
expect_absent "他人 liked 不泄露视频对象" '"video":'

code=$(get "/api/user/$UID_A/liked")
check "匿名看 alice/liked" 200 "$code"
expect_eq "匿名 liked private=true" true "$(json_bool private)"
expect_eq "匿名 liked items=0" 0 "$(items_count)"
expect_absent "匿名 liked 不泄露视频对象" '"video":'

CUR=""; PAGES=0; ALL_IDS=""
while :; do
  if [[ -z "$CUR" ]]; then
    code=$(get "/api/user/$UID_A/liked?limit=1" "$TOKEN_A")
  else
    code=$(get "/api/user/$UID_A/liked?limit=1&cursor=$CUR" "$TOKEN_A")
  fi
  [[ "$code" == "200" ]] || break
  ALL_IDS="$ALL_IDS$(ids_csv),"
  CUR="$(json_str next_cursor)"
  PAGES=$((PAGES + 1))
  [[ -z "$CUR" || "$PAGES" -ge 10 ]] && break
done
expect_eq "liked limit=1 翻页页数" 3 "$PAGES"
expect_eq "liked 翻页去重后 3 条" 3 "$(uniq_count "$ALL_IDS")"

# 游标串用：works 的 `w` 前缀游标不能拿去查 liked。
code=$(get "/api/user/$UID_A/videos?limit=1")
CUR_W="$(json_str next_cursor)"
code=$(get "/api/user/$UID_A/liked?cursor=$CUR_W" "$TOKEN_A")
check "liked 复用 works 游标" 400 "$code"
code=$(get "/api/user/$UID_A/liked?cursor=abc" "$TOKEN_A")
check "liked 非法游标" 400 "$code"
code=$(post "/api/user/$UID_A/liked" '{}' "$TOKEN_A")
check "POST liked" 405 "$code"

# ---- [4/7] 收藏 tab ---------------------------------------------------------
echo "[4/7] 收藏 tab"
code=$(get "/api/user/$UID_A/favorites" "$TOKEN_A")
check "本人 GET favorites" 200 "$code"
expect_eq "favorites tab 名" favorites "$(json_str tab)"
expect_eq "本人 favorites private=false" false "$(json_bool private)"
expect_eq "本人 favorites total=2" 2 "$(json_int total)"
expect_eq "favorites 按收藏时间倒序" "c1,a2" "$(titles_csv)"
expect_eq "favorites 首条作者=carol" carol \
  "$(grep -o '"author":"[^"]*"' "$TMP/body" | head -1 | cut -d'"' -f4)"

code=$(get "/api/user/$UID_A/favorites" "$TOKEN_B")
check "bob 看 alice/favorites" 200 "$code"
expect_eq "他人 favorites private=true" true "$(json_bool private)"
expect_eq "他人 favorites items=0" 0 "$(items_count)"
expect_contains "他人 favorites 带提示" "收藏列表仅本人可见"
expect_absent "他人 favorites 不泄露视频对象" '"video":'

code=$(get "/api/user/$UID_A/favorites")
check "匿名看 alice/favorites" 200 "$code"
expect_eq "匿名 favorites private=true" true "$(json_bool private)"

CUR=""; PAGES=0; ALL_IDS=""
while :; do
  if [[ -z "$CUR" ]]; then
    code=$(get "/api/user/$UID_A/favorites?limit=1" "$TOKEN_A")
  else
    code=$(get "/api/user/$UID_A/favorites?limit=1&cursor=$CUR" "$TOKEN_A")
  fi
  [[ "$code" == "200" ]] || break
  ALL_IDS="$ALL_IDS$(ids_csv),"
  CUR="$(json_str next_cursor)"
  PAGES=$((PAGES + 1))
  [[ -z "$CUR" || "$PAGES" -ge 10 ]] && break
done
expect_eq "favorites limit=1 翻页页数" 2 "$PAGES"
expect_eq "favorites 翻页去重后 2 条" 2 "$(uniq_count "$ALL_IDS")"

# 取消收藏 → 列表少一条；再收藏 → 恢复。
favorite "$TOKEN_A" "$V_C1"
code=$(get "/api/user/$UID_A/favorites" "$TOKEN_A")
check "取消收藏后 GET favorites" 200 "$code"
expect_eq "取消收藏后 total=1" 1 "$(json_int total)"
favorite "$TOKEN_A" "$V_C1"
code=$(get "/api/user/$UID_A/favorites" "$TOKEN_A")
expect_eq "重新收藏后 total=2" 2 "$(json_int total)"

code=$(get "/api/user/$UID_D/favorites" "$TOKEN_D")
check "dave 空收藏" 200 "$code"
expect_eq "空收藏 total=0" 0 "$(json_int total)"
code=$(post "/api/user/$UID_A/favorites" '{}' "$TOKEN_A")
check "POST favorites" 405 "$code"

# ---- [5/7] 拉黑过滤 ---------------------------------------------------------
echo "[5/7] 拉黑过滤"
code=$(post "/api/user/$UID_C/block" '{}' "$TOKEN_A")
check "alice 拉黑 carol" 200 "$code"
code=$(get "/api/user/$UID_A/liked" "$TOKEN_A")
check "拉黑后本人 liked" 200 "$code"
expect_eq "拉黑后 liked total=2" 2 "$(json_int total)"
expect_absent "拉黑后 liked 无 carol 视频" "\"title\":\"c1\""
code=$(get "/api/user/$UID_A/favorites" "$TOKEN_A")
check "拉黑后本人 favorites" 200 "$code"
expect_eq "拉黑后 favorites total=1" 1 "$(json_int total)"
expect_absent "拉黑后 favorites 无 carol 视频" "\"title\":\"c1\""
expect_absent "拉黑后 favorites 无 c1 id" "\"id\":$V_C1,"

code=$(del "/api/user/$UID_C/block" "$TOKEN_A")
check "alice 取消拉黑 carol" 200 "$code"
code=$(get "/api/user/$UID_A/liked" "$TOKEN_A")
expect_eq "取消拉黑后 liked total=3" 3 "$(json_int total)"
code=$(get "/api/user/$UID_A/favorites" "$TOKEN_A")
expect_eq "取消拉黑后 favorites total=2" 2 "$(json_int total)"

# 作者拉黑 viewer：bob 拉黑 alice 后，alice 看 bob 主页作品为空 + blocked。
code=$(post "/api/user/$UID_A/block" '{}' "$TOKEN_B")
check "bob 拉黑 alice" 200 "$code"
code=$(get "/api/user/$UID_B/videos" "$TOKEN_A")
check "被拉黑者看 bob/works" 200 "$code"
expect_eq "被拉黑者 works blocked=true" true "$(json_bool blocked)"
expect_eq "被拉黑者 works items=0" 0 "$(items_count)"
expect_eq "被拉黑者 works total=0" 0 "$(json_int total)"
code=$(get "/api/user/$UID_B/videos")
check "匿名看 bob/works 不受影响" 200 "$code"
expect_eq "匿名 works total=1" 1 "$(json_int total)"
code=$(del "/api/user/$UID_A/block" "$TOKEN_B")
check "bob 取消拉黑 alice" 200 "$code"
code=$(get "/api/user/$UID_B/videos" "$TOKEN_A")
expect_eq "取消拉黑后 works total=1" 1 "$(json_int total)"

# ---- [6/7] 前端元素 ---------------------------------------------------------
echo "[6/7] 前端元素"
code=$(get "/")
check "GET /" 200 "$code"
PAGE_FILE="$TMP/page.html"
cp "$TMP/body" "$PAGE_FILE"
PAGE_BYTES=$(wc -c <"$PAGE_FILE")
expect_eq "页面 > 100 KB" yes \
  "$([[ "$PAGE_BYTES" -gt 102400 ]] && echo yes || echo no)"
expect_contains "三 tab 容器" 'p-tabs' "$PAGE_FILE"
expect_contains "tab 按钮 data-tab" "data-tab" "$PAGE_FILE"
expect_contains "tab 状态工厂" "profileTabState" "$PAGE_FILE"
expect_contains "tab 加载函数" "loadProfileTab" "$PAGE_FILE"
expect_contains "tab 面板重绘" "paintProfilePanel" "$PAGE_FILE"
expect_contains "作品/喜欢/收藏接口" "'/videos'" "$PAGE_FILE"
expect_contains "喜欢接口路径" "'/liked'" "$PAGE_FILE"
expect_contains "收藏接口路径" "'/favorites'" "$PAGE_FILE"
expect_contains "喜欢锁文案" "喜欢列表仅本人可见" "$PAGE_FILE"
expect_contains "收藏锁文案" "收藏列表仅本人可见" "$PAGE_FILE"
expect_contains "空状态文案（喜欢）" "你还没有点赞过视频" "$PAGE_FILE"
expect_contains "空状态文案（收藏）" "你还没有收藏视频" "$PAGE_FILE"
expect_contains "加载更多按钮" "加载更多" "$PAGE_FILE"
expect_contains "滚动加载阈值" "scrollHeight" "$PAGE_FILE"

# ---- [7/7] 性能 + 回归 ------------------------------------------------------
echo "[7/7] 性能 + 回归"
measure_avg() { # path token
  local sum=0 t
  for _ in $(seq 1 20); do
    t=$(curl -s -o /dev/null -w '%{time_starttransfer}' \
      -H "Authorization: Bearer $2" "$GW$1")
    sum=$(awk -v a="$sum" -v b="$t" 'BEGIN{print a+b}')
  done
  awk -v s="$sum" 'BEGIN{printf "%.1f", s*1000/20}'
}
AVG_LIKED=$(measure_avg "/api/user/$UID_A/liked?limit=20" "$TOKEN_A")
AVG_FAV=$(measure_avg "/api/user/$UID_A/favorites?limit=20" "$TOKEN_A")
expect_eq "liked TTFB < 200 ms（20 次均值）" yes \
  "$(awk -v v="$AVG_LIKED" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       liked avg=%s ms\n' "$AVG_LIKED"
expect_eq "favorites TTFB < 200 ms（20 次均值）" yes \
  "$(awk -v v="$AVG_FAV" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       favorites avg=%s ms\n' "$AVG_FAV"

EXPLAIN_OUT="$(DB="$DB" UID_A="$UID_A" python3 - <<'PY'
import os
import sqlite3

con = sqlite3.connect(os.environ["DB"])
uid = int(os.environ["UID_A"])
sql = (
    "SELECT v.id FROM likes a JOIN videos v ON v.id = a.video_id "
    "WHERE a.user_id = ? AND v.user_id NOT IN "
    "(SELECT blocked_id FROM blocks WHERE user_id = ?) "
    "AND NOT EXISTS (SELECT 1 FROM blocks b WHERE b.user_id = v.user_id "
    "AND b.blocked_id = ?) ORDER BY a.created_at DESC, v.id DESC LIMIT 20"
)
for row in con.execute("EXPLAIN QUERY PLAN " + sql, (uid, uid, uid)):
    print(row[3])
PY
)"
if grep -q "idx_likes_user" <<<"$EXPLAIN_OUT"; then
  printf '  %-52s PASS  用时索引 idx_likes_user\n' "liked 查询计划"
  PASSED=$((PASSED + 1))
else
  printf '  %-52s FAIL  未命中 idx_likes_user（计划：%s）\n' "liked 查询计划" \
    "$(tr '\n' ';' <<<"$EXPLAIN_OUT")"
  FAILED=$((FAILED + 1))
fi

code=$(get "/api/video/mine?limit=10" "$TOKEN_A")
check "回归 /api/video/mine" 200 "$code"
expect_eq "回归 mine 返回 3 条" 3 "$(items_count)"
code=$(get "/api/video/favorites?limit=10" "$TOKEN_A")
check "回归 /api/video/favorites" 200 "$code"
code=$(get "/api/feed?limit=20")
check "回归 /api/feed" 200 "$code"
expect_contains "回归 feed 有视频" '"video":'
code=$(get "/api/user/$UID_A")
check "回归 /api/user/:id" 200 "$code"
expect_contains "资料页仍有拉黑标记" '"is_blocked":'

echo "---------------------------------------------"
printf '[accept_profile_tabs] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_profile_tabs] PASS"
  exit 0
fi
echo "[accept_profile_tabs] FAIL"
exit 1
