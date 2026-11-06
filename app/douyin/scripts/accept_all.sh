#!/usr/bin/env bash
# 抖音 C9 统一验收：6 个业务服务 + gateway（7 进程）全部经网关 28080。
#
# 链路：
#   1) 注册 alice / bob（经网关 → user 29101）
#   2) alice 发 3 条视频（经网关 → video 29102）
#   3) feed 分页 limit=1 翻页（经网关 → feed 29103）
#   4) 点赞两次幂等（经网关 → interaction 29104）
#   5) 评论 2 条 + 评论列表（经网关 → interaction 29104）
#   6) 搜索用户/视频（经网关 → search 29106）
#   7) 创建直播间 / 开播 / 进房（经网关 → live 29105）；
#      第 10 轮追加聊天 / 礼物 / 贡献榜 / 余额 / 直播页
#   8) 网关专项：6 条业务路由全 200、无 token 写接口 401、未知路径 404、
#      /api/health/all 聚合 6 项 ok=true、/health 自身 200
#   9) --bench：用 scripts/loadgen.cpp 压测 GET /api/feed 与 GET /api/video/:id
#
# 用法：
#   scripts/accept_all.sh           # 只跑功能验收
#   scripts/accept_all.sh --bench   # 功能验收后追加两轮压测
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_all.pids；退出时按该文件
# 逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_all.pids"
DB="$ROOT/data/accept_all.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0
BENCH=0

for arg in "$@"; do
  case "$arg" in
    --bench) BENCH=1 ;;
    -h|--help)
      sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "未知参数：$arg（支持 --bench）" >&2
      exit 2
      ;;
  esac
done

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

# ---- 进程管理 ---------------------------------------------------------------
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
    printf '  %-42s PASS  HTTP %s %s\n' "$name" "$actual" "$detail"
    PASSED=$((PASSED + 1))
  else
    printf '  %-42s FAIL  HTTP %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-42s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-42s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-42s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-42s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

# ---- HTTP 封装（全部指向网关） ----------------------------------------------
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

json_int() { sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p" "$TMP/body" | head -1; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
video_id() { sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }

# ---- 启动服务（串行等待 health，避免同库并发建表） --------------------------
start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_all_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_all_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s db=%s\n' "$name" "$port" "$pid" "$DB"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_all_"*.log

echo "[0/9] 启动 7 个进程（6 业务 + gateway）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

echo "[1/9] 网关自身健康 + 聚合健康"
code=$(get "/health")
check "GET /health" 200 "$code"
expect_body " /health 含 service=gateway" '"service":"gateway"'
code=$(get "/api/health/all")
check "GET /api/health/all" 200 "$code"
expect_body "聚合顶层 ok=true" '^{"ok":true,"services":{'
expect_eq "聚合 6 个上游均 ok=true" 6 \
  "$(grep -o '"127\.0\.0\.1:2910[1-6]":{"ok":true' "$TMP/body" | wc -l)"

echo "[2/9] 注册 alice / bob（经网关 → user）"
code=$(post "/api/user/register" \
  '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice（经网关）" 200 "$code"
expect_body "alice 响应含 token" '"token":"'
TOKEN_A=$(json_str token)
UID_A=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
code=$(get "/api/user/$UID_A")
check "GET /api/user/$UID_A（公开读）" 200 "$code"
expect_body "用户详情含 alice" '"name":"alice"'
code=$(post "/api/user/register" \
  '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob（经网关）" 200 "$code"
TOKEN_B=$(json_str token)
UID_B=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" || -z "$UID_A" || -z "$UID_B" ]]; then
  echo "token / user id 提取失败（A=$TOKEN_A B=$TOKEN_B）" >&2
  exit 1
fi
expect_eq "alice uid 与 bob uid 不同" yes \
  "$([[ "$UID_A" != "$UID_B" ]] && echo yes || echo no)"

echo "[3/9] alice 发 3 条视频（经网关 → video）"
V1=""; V2=""; V3=""
for i in 1 2 3; do
  code=$(post "/api/video" \
    "{\"title\":\"v$i\",\"url\":\"http://example.com/$i.mp4\"}" "$TOKEN_A")
  check "发视频 v$i（经网关）" 200 "$code"
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
code=$(get "/api/video/$V1")
check "GET /api/video/$V1（公开读）" 200 "$code"
expect_body "视频详情含 author=alice" '"author":"alice"'
expect_body "视频详情含 title=v1" '"title":"v1"'
expect_eq "视频详情初始 like_count" 0 "$(json_int like_count)"

echo "[4/9] feed 分页（经网关 → feed）"
code=$(get "/api/feed?limit=20")
check "GET /api/feed?limit=20" 200 "$code"
expect_body "feed 含 next_cursor" '"next_cursor"'
CURSOR=""
PAGE_IDS=()
for _ in $(seq 1 10); do
  if [[ -z "$CURSOR" ]]; then
    code=$(get "/api/feed?limit=1")
  else
    code=$(get "/api/feed?limit=1&cursor=$CURSOR")
  fi
  [[ "$code" == "200" ]] || break
  PAGE_IDS+=("$(sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)")
  CURSOR=$(json_str next_cursor)
  [[ -z "$CURSOR" ]] && break
done
expect_eq "分页总条数（limit=1 翻页）" 3 "${#PAGE_IDS[@]}"
expect_eq "分页无重复" 3 "$(printf '%s\n' "${PAGE_IDS[@]}" | sort -u | wc -l)"
# 推荐流改成随机序后，翻页不再保证「最新在前」，但必须覆盖全部视频。
expect_eq "分页覆盖全部视频（集合一致）" \
  "$(printf '%s\n' "$V1" "$V2" "$V3" | sort | tr '\n' ',')" \
  "$(printf '%s\n' "${PAGE_IDS[@]}" | sort | tr '\n' ',')"

# 随机推荐：连续 5 次首屏至少出现两种不同顺序（3 条视频共 6 种排列，
# 5 次全同序的概率 = 1/6^4 ≈ 7.7e-4），防止 RANDOM 实现退化成固定排序。
FIRST_ORDER=""
ORDER_DIFF=0
for _ in $(seq 1 5); do
  get "/api/feed?limit=20" >/dev/null
  ORDER="$(grep -o '"video":{"id":[0-9]*' "$TMP/body" | sed 's/.*://' | tr '\n' ',')"
  if [[ -z "$FIRST_ORDER" ]]; then
    FIRST_ORDER="$ORDER"
  elif [[ "$ORDER" != "$FIRST_ORDER" ]]; then
    ORDER_DIFF=1
  fi
done
expect_eq "随机推荐：5 次首屏出现不同顺序" 1 "$ORDER_DIFF"

echo "[5/9] 点赞两次幂等（经网关 → interaction）"
code=$(post "/api/like" "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 首次点赞" 200 "$code"
expect_body "首次点赞 inserted=true" '"inserted":true'
expect_eq "首次点赞 like_count" 1 "$(json_int like_count)"
code=$(post "/api/like" "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 重复点赞" 200 "$code"
expect_body "重复点赞 inserted=false" '"inserted":false'
expect_eq "重复点赞 like_count 仍为 1" 1 "$(json_int like_count)"
code=$(get "/api/video/$V1")
check "视频详情（点赞后）" 200 "$code"
expect_eq "详情 like_count 同步" 1 "$(json_int like_count)"

echo "[6/9] 评论 2 条 + 列表（经网关 → interaction）"
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"第一条评论\"}" "$TOKEN_B")
check "bob 发评论 1" 200 "$code"
expect_body "评论 1 返回内容" '第一条评论'
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"second comment\"}" "$TOKEN_A")
check "alice 发评论 2" 200 "$code"
code=$(get "/api/comment/$V1")
check "GET /api/comment/$V1（公开读）" 200 "$code"
expect_eq "评论列表条数" 2 "$(grep -o '"comment":{' "$TMP/body" | wc -l)"
expect_body "评论列表最新在前" 'second comment'
code=$(get "/api/video/$V1")
check "视频详情（评论后）" 200 "$code"
expect_eq "详情 comment_count" 2 "$(json_int comment_count)"

echo "[7/9] 搜索（经网关 → search）"
code=$(get "/api/search?q=alice&type=user")
check "搜索用户 q=alice" 200 "$code"
expect_body "搜索命中 alice" 'alice'
expect_body "搜索类型 type=user" '"type":"user"'
code=$(get "/api/search?q=v1&type=video")
check "搜索视频 q=v1" 200 "$code"
expect_body "搜索类型 type=video" '"type":"video"'

echo "[8/9] 直播：建播 / 开播 / 进房（经网关 → live）"
code=$(post "/api/live/room" '{"title":"alice 的直播间"}' "$TOKEN_A")
check "alice 创建直播间" 200 "$code"
expect_body "创建后 status=0" '"status":0'
ROOM=$(sed -n 's/.*"room":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
if [[ -z "$ROOM" ]]; then
  echo "房间 id 提取失败" >&2
  exit 1
fi
code=$(post "/api/live/room/$ROOM/start" '{}' "$TOKEN_A")
check "房主开播" 200 "$code"
expect_body "开播后 status=1" '"status":1'
code=$(post "/api/live/room/$ROOM/enter" '{}' "$TOKEN_B")
check "bob 进房" 200 "$code"
expect_body "进房 changed=true" '"changed":true'
expect_body "进房后 viewer_count=1" '"viewer_count":1'
code=$(get "/api/live/rooms?status=1")
check "GET /api/live/rooms?status=1（公开读）" 200 "$code"
expect_eq "直播列表含房间" 1 \
  "$(grep -o "\"room\":{\"id\":$ROOM," "$TMP/body" | wc -l)"
# 第 10 轮：直播间聊天 / 礼物 / 贡献榜（全部经网关 → live 29105）
code=$(post "/api/live/$ROOM/messages" '{"content":"直播聊天验收"}' "$TOKEN_B")
check "bob 发直播间消息" 200 "$code"
expect_body "聊天消息回显" '直播聊天验收'
code=$(get "/api/live/$ROOM/messages?after=0&limit=50")
check "聊天增量拉取" 200 "$code"
expect_body "聊天列表含消息" '直播聊天验收'
code=$(get "/api/live/$ROOM/messages?after=999999")
check "聊天 after 无新消息" 200 "$code"
expect_body "增量空列表" '"count":0'
code=$(post "/api/live/$ROOM/gift" '{"gift_id":2,"count":2}' "$TOKEN_B")
check "bob 送棒棒糖 ×2" 200 "$code"
expect_body "送礼扣款后余额 980" '"coins":980'
code=$(get "/api/live/wallet" "$TOKEN_B")
check "钱包余额" 200 "$code"
expect_body "钱包 980" '"coins":980'
code=$(get "/api/live/$ROOM/ranking?limit=10")
check "贡献榜" 200 "$code"
expect_body "榜单含 bob" 'bob'
expect_body "榜单金额 20" '"coins":20'
code=$(post "/api/live/$ROOM/gift" '{"gift_id":5,"count":99}' "$TOKEN_B")
check "余额不足" 400 "$code"
expect_body "余额不足提示" '余额不足'
code=$(get "/live?room=$ROOM")
check "GET /live 直播页" 200 "$code"
expect_body "直播页聊天区" 'id="chatList"'
expect_body "直播页礼物面板" 'id="giftPanel"'
expect_body "直播页贡献榜" 'id="rankingPanel"'

echo "[9/9] 网关专项：鉴权 / 未知路径 / 6 条路由全 200"
code=$(post "/api/like" "{\"video_id\":$V1}")
check "无 token POST /api/like" 401 "$code"
expect_body "401 body 含 code=401" '"code":401'
code=$(post "/api/video" '{"title":"x","url":"http://example.com/x.mp4"}')
check "无 token POST /api/video" 401 "$code"
code=$(post "/api/live/room" '{"title":"x"}')
check "无 token POST /api/live/room" 401 "$code"
code=$(post "/api/like" "{\"video_id\":$V1}" "forged.invalid.token")
check "伪造 token POST /api/like" 401 "$code"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X DELETE \
  "$GW/api/video/$V1")
check "无 token DELETE /api/video/$V1" 401 "$code"
code=$(get "/api/unknown")
check "未知路径 GET /api/unknown" 404 "$code"
code=$(get "/unknown")
check "未知路径 GET /unknown" 404 "$code"
code=$(get "/api/video/$V1" "$TOKEN_A")
check "带 token 公开读 /api/video/$V1" 200 "$code"
# 六条业务前缀各再来一次，确认经网关全部 200。
code=$(get "/api/user/$UID_A")
check "路由 user   /api/user/:id" 200 "$code"
code=$(get "/api/video/$V1")
check "路由 video  /api/video/:id" 200 "$code"
code=$(get "/api/feed?limit=20")
check "路由 feed   /api/feed" 200 "$code"
code=$(post "/api/like" "{\"video_id\":$V1}" "$TOKEN_B")
check "路由 like   /api/like" 200 "$code"
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"验收第三条\"}" "$TOKEN_B")
check "路由 comment /api/comment" 200 "$code"
code=$(get "/api/live/rooms")
check "路由 live   /api/live/rooms" 200 "$code"
code=$(get "/api/search?q=alice")
check "路由 search /api/search" 200 "$code"

if [[ "$BENCH" -eq 1 ]]; then
  echo "[bench] 压测网关：GET /api/feed?limit=20 与 GET /api/video/$V1"
  LOADGEN="$BIN_DIR/loadgen"
  if [[ ! -x "$LOADGEN" ]]; then
    echo "找不到 $LOADGEN，请先跑 app/douyin/build.sh" >&2
    FAILED=$((FAILED + 1))
  else
    BENCH_LOG="$LOG_DIR/accept_all_loadgen.log"
    : >"$BENCH_LOG"
    "$LOADGEN" --url "$GW/api/feed?limit=20" --concurrency 32 --requests 2000 \
      --method GET 2>&1 | tee -a "$BENCH_LOG"
    FEED_RC=${PIPESTATUS[0]}
    "$LOADGEN" --url "$GW/api/video/$V1" --concurrency 32 --requests 2000 \
      --method GET 2>&1 | tee -a "$BENCH_LOG"
    VIDEO_RC=${PIPESTATUS[0]}
    check "压测 feed 无传输错误" 0 "$FEED_RC"
    check "压测 video 无传输错误" 0 "$VIDEO_RC"
    expect_eq "压测 feed 非 200 数" 0 \
      "$(sed -n 's/.*non200=\([0-9]*\).*/\1/p' "$BENCH_LOG" | sed -n '1p')"
    expect_eq "压测 video 非 200 数" 0 \
      "$(sed -n 's/.*non200=\([0-9]*\).*/\1/p' "$BENCH_LOG" | sed -n '2p')"
  fi
fi

echo "---------------------------------------------"
printf '[accept_all] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_all] PASS"
  exit 0
fi
echo "[accept_all] FAIL"
exit 1
