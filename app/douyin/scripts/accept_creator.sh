#!/usr/bin/env bash
# 创作者中心（第 13 轮）验收：overview / trend / top-videos / 页面 / 越权。
#
# 用例分组：
#   [1] 启动 user / video / interaction / creator / gateway（同一个临时库）
#   [2] 造数据：alice 3 条作品，bob+carol 的点赞/评论/关注（含幂等点赞）
#   [3] overview：总播放（估算）/ 点赞 / 评论 / 粉丝 / 作品数，逐项对账
#   [4] trend：7 天与 30 天点数、今天聚合值、非法 days 400
#   [5] top-videos：互动量排序、limit 生效、非法 limit 400
#   [6] 越权：无 token 401、伪造 token 401、查别人的 user_id 403、
#       bob 只看到自己的 0 数据、直连 29108 也必须有 token
#   [7] 页面：GET /creator 200 / HEAD 200 / 关键 DOM 标记齐全
#   [8] 软删除：删作品后作品数下降、已删作品不进入 TOP
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_creator.pids；退出时按该
# 文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_creator.pids"
DB="$ROOT/data/accept_creator.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0

GW_PORT="${DOUYIN_GATEWAY_PORT:-28080}"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
VIDEO_PORT="${DOUYIN_VIDEO_PORT:-29102}"
INTERACTION_PORT="${DOUYIN_INTERACTION_PORT:-29104}"
CREATOR_PORT="${DOUYIN_CREATOR_PORT:-29108}"
GW="http://127.0.0.1:$GW_PORT"
CREATOR="http://127.0.0.1:$CREATOR_PORT"
TODAY="$(date +%Y-%m-%d)"

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
check() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-52s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-52s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-52s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-52s FAIL  实际=%s（期望 %s）\n' "$name" "$actual" "$expected"
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

# ---- HTTP 封装（默认指向网关） ----------------------------------------------
post() { # path json [token]
  local path="$1" payload="$2" token="${3:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$payload" "$GW$path")
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

delete() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X DELETE "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

get_direct() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' "$CREATOR$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p" "$TMP/body" | head -1; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
video_id() { sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }
comment_id() { sed -n 's/.*"comment":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_creator_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_creator_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s db=%s\n' "$name" "$port" "$pid" "$DB"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_creator_"*.log

echo "[1/8] 启动 5 个进程（4 业务 + gateway）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service creator_service "$CREATOR_PORT"
start_service gateway "$GW_PORT"

echo "[2/8] 造数据：3 用户 / 3 作品 / 点赞 / 评论 / 关注"
code=$(post "/api/user/register" \
  '{"name":"alice","email":"alice@creator.test","password":"secret123"}')
check "注册 alice" 200 "$code"
TOKEN_A=$(json_str token)
UID_A=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
code=$(post "/api/user/register" \
  '{"name":"bob","email":"bob@creator.test","password":"secret123"}')
check "注册 bob" 200 "$code"
TOKEN_B=$(json_str token)
UID_B=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
code=$(post "/api/user/register" \
  '{"name":"carol","email":"carol@creator.test","password":"secret123"}')
check "注册 carol" 200 "$code"
TOKEN_C=$(json_str token)
UID_C=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" || -z "$TOKEN_C" || -z "$UID_A" ]]; then
  echo "注册结果解析失败" >&2
  exit 1
fi

V1=""; V2=""; V3=""
for i in 1 2 3; do
  code=$(post "/api/video" \
    "{\"title\":\"creator-v$i\",\"url\":\"http://example.com/creator-$i.mp4\"}" "$TOKEN_A")
  check "alice 发作品 v$i" 200 "$code"
  vid=$(video_id)
  case "$i" in
    1) V1="$vid" ;;
    2) V2="$vid" ;;
    3) V3="$vid" ;;
  esac
done
if [[ -z "$V1" || -z "$V2" || -z "$V3" ]]; then
  echo "视频 id 提取失败（V1=$V1 V2=$V2 V3=$V3）" >&2
  exit 1
fi

# 点赞：v1 由 bob（重复一次，验证幂等不重复计数）、v2 由 bob + carol。
code=$(post "/api/like" "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 点赞 v1" 200 "$code"
code=$(post "/api/like" "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 重复点赞 v1（幂等）" 200 "$code"
code=$(post "/api/like" "{\"video_id\":$V2}" "$TOKEN_B")
check "bob 点赞 v2" 200 "$code"
code=$(post "/api/like" "{\"video_id\":$V2}" "$TOKEN_C")
check "carol 点赞 v2" 200 "$code"

# 评论：v1 三条（bob 两条 + carol 一条），v2 一条；删掉 bob 第二条。
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"bob 评论一\"}" "$TOKEN_B")
check "bob 评论 v1-1" 200 "$code"
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"bob 评论二（稍后删除）\"}" "$TOKEN_B")
check "bob 评论 v1-2" 200 "$code"
DEL_COMMENT=$(comment_id)
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"carol 评论 v1\"}" "$TOKEN_C")
check "carol 评论 v1" 200 "$code"
code=$(post "/api/comment" "{\"video_id\":$V2,\"content\":\"bob 评论 v2\"}" "$TOKEN_B")
check "bob 评论 v2" 200 "$code"
if [[ -z "$DEL_COMMENT" ]]; then
  echo "评论 id 提取失败" >&2
  exit 1
fi
code=$(delete "/api/comment/$DEL_COMMENT" "$TOKEN_B")
check "bob 删除自己的评论" 200 "$code"

# 关注：bob、carol 关注 alice。
code=$(post "/api/user/$UID_A/follow" '{}' "$TOKEN_B")
check "bob 关注 alice" 200 "$code"
code=$(post "/api/user/$UID_A/follow" '{}' "$TOKEN_C")
check "carol 关注 alice" 200 "$code"

echo "[3/8] overview：数字逐项对账"
code=$(get "/api/creator/overview" "$TOKEN_A")
check "GET /api/creator/overview" 200 "$code"
expect_contains "overview 是 JSON" '"ok":true'
expect_eq "总点赞 total_likes" 3 "$(json_int total_likes)"
expect_eq "总评论 total_comments（已删 1 条）" 3 "$(json_int total_comments)"
expect_eq "粉丝数 followers" 2 "$(json_int followers)"
expect_eq "作品数 video_count" 3 "$(json_int video_count)"
expect_eq "总播放量 total_plays（点赞+评论估算）" 6 "$(json_int total_plays)"
expect_contains "播放量标注为估算" '"plays_estimated":true'

echo "[4/8] trend：7 天 / 30 天 / 非法 days"
code=$(get "/api/creator/trend?days=7" "$TOKEN_A")
check "GET /api/creator/trend?days=7" 200 "$code"
expect_eq "7 天点数" 7 "$(grep -o '"date":"' "$TMP/body" | wc -l)"
expect_eq "trend days 字段" 7 "$(json_int days)"
TODAY_LINE=$(grep -o "{\"date\":\"$TODAY\"[^}]*}" "$TMP/body" | head -1)
if [[ -n "$TODAY_LINE" ]]; then
  printf '  %-52s PASS  %s\n' "今天出现在 7 天窗口" "$TODAY_LINE"
  PASSED=$((PASSED + 1))
else
  printf '  %-52s FAIL  今天（%s）不在趋势里\n' "今天出现在 7 天窗口" "$TODAY"
  FAILED=$((FAILED + 1))
fi
TODAY_LIKES=$(printf '%s' "$TODAY_LINE" | sed -n 's/.*"likes":\([0-9]*\).*/\1/p')
TODAY_COMMENTS=$(printf '%s' "$TODAY_LINE" | sed -n 's/.*"comments":\([0-9]*\).*/\1/p')
TODAY_FOLLOWERS=$(printf '%s' "$TODAY_LINE" | sed -n 's/.*"followers":\([0-9]*\).*/\1/p')
expect_eq "今天新增点赞" 3 "$TODAY_LIKES"
expect_eq "今天新增评论（已删 1 条不计）" 3 "$TODAY_COMMENTS"
expect_eq "今天新增粉丝" 2 "$TODAY_FOLLOWERS"
code=$(get "/api/creator/trend?days=30" "$TOKEN_A")
check "GET /api/creator/trend?days=30" 200 "$code"
expect_eq "30 天点数" 30 "$(grep -o '"date":"' "$TMP/body" | wc -l)"
expect_eq "trend 30 days 字段" 30 "$(json_int days)"
code=$(get "/api/creator/trend?days=3" "$TOKEN_A")
check "非法 days=3" 400 "$code"
code=$(get "/api/creator/trend?days=abc" "$TOKEN_A")
check "非法 days=abc" 400 "$code"

echo "[5/8] top-videos：互动量排序 / limit / 非法 limit"
code=$(get "/api/creator/top-videos?limit=5" "$TOKEN_A")
check "GET /api/creator/top-videos?limit=5" 200 "$code"
expect_eq "TOP 数量" 3 "$(json_int count)"
ORDER=$(grep -o '"id":[0-9]*' "$TMP/body" | sed 's/.*://' | paste -sd,)
expect_eq "TOP 顺序（v1 8 分 > v2 7 分 > v3 0 分）" "$V1,$V2,$V3" "$ORDER"
TOP_SCORE=$(grep -o '"score":[0-9]*' "$TMP/body" | head -1 | sed 's/.*://')
expect_eq "第一条互动分（1×2 + 2×3）" 8 "$TOP_SCORE"
code=$(get "/api/creator/top-videos" "$TOKEN_A")
check "缺省 limit=5" 200 "$code"
expect_eq "缺省返回 3 条" 3 "$(json_int count)"
code=$(get "/api/creator/top-videos?limit=2" "$TOKEN_A")
check "limit=2" 200 "$code"
expect_eq "limit=2 返回 2 条" 2 "$(json_int count)"
code=$(get "/api/creator/top-videos?limit=0" "$TOKEN_A")
check "非法 limit=0" 400 "$code"
code=$(get "/api/creator/top-videos?limit=abc" "$TOKEN_A")
check "非法 limit=abc" 400 "$code"

echo "[6/8] 越权与鉴权"
code=$(get "/api/creator/overview")
check "无 token overview" 401 "$code"
code=$(get "/api/creator/trend?days=7")
check "无 token trend" 401 "$code"
code=$(get "/api/creator/overview" "forged.invalid.token")
check "伪造 token overview" 401 "$code"
code=$(get "/api/creator/overview?user_id=$UID_A" "$TOKEN_B")
check "bob 查 alice 的 overview" 403 "$code"
expect_contains "越权提示" '只能查看自己的数据'
code=$(get "/api/creator/trend?days=7&user_id=$UID_A" "$TOKEN_B")
check "bob 查 alice 的 trend" 403 "$code"
code=$(get "/api/creator/overview" "$TOKEN_B")
check "bob 看自己的 overview" 200 "$code"
expect_eq "bob 作品数（自己的 0）" 0 "$(json_int video_count)"
expect_eq "bob 总点赞（自己的 0）" 0 "$(json_int total_likes)"
code=$(get_direct "/api/creator/overview" "$TOKEN_A")
check "直连 creator overview（带 token）" 200 "$code"
code=$(get_direct "/api/creator/overview")
check "直连 creator overview（无 token）" 401 "$code"

echo "[7/8] 页面 /creator"
code=$(get "/creator")
check "GET /creator" 200 "$code"
expect_contains "4 个数字卡片" 'id="statPlays"'
expect_contains "点赞卡片" 'id="statLikes"'
expect_contains "评论卡片" 'id="statComments"'
expect_contains "粉丝卡片" 'id="statFollowers"'
expect_contains "CSS 柱状图容器" 'id="trendChart"'
expect_contains "TOP5 列表容器" 'id="topVideos"'
expect_contains "柱状图纯 CSS" 'class="bar like"\|\.bar\.like'
expect_contains "7 天切换" 'data-days="7"'
expect_contains "30 天切换" 'data-days="30"'
expect_absent "不引入第三方图表库" 'cdn\.\|echarts\|chart\.js'
code=$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' -I "$GW/creator")
check "HEAD /creator" 200 "$code"

echo "[8/8] 软删除过滤"
code=$(delete "/api/video/$V3" "$TOKEN_A")
check "alice 删除作品 v3" 200 "$code"
code=$(get "/api/creator/overview" "$TOKEN_A")
check "删除后 overview" 200 "$code"
expect_eq "作品数下降为 2" 2 "$(json_int video_count)"
expect_eq "总点赞不变（v3 无互动）" 3 "$(json_int total_likes)"
code=$(get "/api/creator/top-videos?limit=5" "$TOKEN_A")
check "删除后 top-videos" 200 "$code"
expect_eq "TOP 数量下降为 2" 2 "$(json_int count)"
expect_absent "已删作品不在 TOP" "\"id\":$V3,"

echo "---------------------------------------------"
printf '[accept_creator] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_creator] PASS"
  exit 0
fi
echo "[accept_creator] FAIL"
exit 1
