#!/usr/bin/env bash
# 分享落地页 `/v/:id` 验收（第 4 轮新增）：启动 7 进程 → 建号/发视频 →
# SSR 页面与 OG 标签 → 评论分页 → 404 / 已删除 / 拉黑过滤 → 关注/点赞态 →
# 回归（首页与 API）→ TTFB。
#
# 用例分组：
#   [1] 页面本体：200 + text/html + 体积 + <video>/作者/关注/点赞/评论/下载/分享
#   [2] OG / Twitter：og:title|description|image|video|url|type + twitter:card
#   [3] 安全：标题里的 <script> 被转义（页面与 OG 都不得出现原始标签）
#   [4] 评论区：SSR 条数、分页游标、第二页接口
#   [5] 404：不存在 id / 非数字 id / 0 / 已删除视频
#   [6] 拉黑：viewer 拉黑作者 404、取消恢复 200、作者拉黑 viewer 404、匿名不受影响
#   [7] 身份态：已关注 / 已点赞 只对本人渲染；匿名页面无 on 态
#   [8] 回归：首页 `/` 仍 200、`/api/video/:id` 仍 200、HEAD 可用
#   [9] 性能：20 次串行 GET /v/:id 的 TTFB（avg / max）
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_share_landing.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_share_landing.pids"
DB="$ROOT/data/accept_share_landing.db"
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
      wait "$pid" 2>/dev/null || true
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
    printf '  %-48s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-48s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() { # name pattern [file]
  local name="$1" pattern="$2" file="${3:-$TMP/body}"
  if grep -q "$pattern" "$file"; then
    printf '  %-48s PASS  含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  缺少 %s\n' "$name" "$pattern"
    FAILED=$((FAILED + 1))
  fi
}

expect_absent() { # name pattern [file]
  local name="$1" pattern="$2" file="${3:-$TMP/body}"
  if grep -q "$pattern" "$file"; then
    printf '  %-48s FAIL  不应出现 %s\n' "$name" "$pattern"
    FAILED=$((FAILED + 1))
  else
    printf '  %-48s PASS  不含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  fi
}

# ---- HTTP 封装（全部经网关 28080） -----------------------------------------
get() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 10 -o "$TMP/body" -w '%{http_code}' "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

head_code() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 10 -o /dev/null -w '%{http_code}' -I "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

post() { # path json [token]
  local path="$1" payload="$2" token="${3:-}"
  local args=(-s --max-time 10 -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$payload" "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

delete_path() { # path [json] [token]
  local path="$1" payload="${2:-}" token="${3:-}"
  local args=(-s --max-time 10 -o "$TMP/body" -w '%{http_code}' -X DELETE "$GW$path")
  if [[ -n "$payload" ]]; then
    args+=(-H 'Content-Type: application/json' -d "$payload")
  fi
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
video_id() { sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }

# ---- 启动服务（串行等待 health，避免同库并发建表） -------------------------
start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_share_landing_$name.log" 2>&1 &
  local pid=$!
  echo "$name $pid $port" >>"$PID_FILE"
  local ready=0
  for _ in $(seq 1 300); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "$name 启动失败，日志：" >&2
    cat "$LOG_DIR/accept_share_landing_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_share_landing_"*.log

echo "[0/9] 启动 7 个进程（6 业务 + gateway）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

echo "[1/9] 建号 + 发视频（经网关）"
code=$(post "/api/user/register" \
  '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
TOKEN_A=$(json_str token)
UID_A=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
code=$(post "/api/user/register" \
  '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
TOKEN_B=$(json_str token)
UID_B=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
code=$(post "/api/user/register" \
  '{"name":"carol","email":"carol@example.com","password":"secret123"}')
check "注册 carol" 200 "$code"
TOKEN_C=$(json_str token)
UID_C=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" || -z "$TOKEN_C" ]]; then
  echo "token 提取失败（A=$TOKEN_A B=$TOKEN_B C=$TOKEN_C）" >&2
  exit 1
fi

code=$(post "/api/video" \
  '{"title":"分享测试视频 #1","url":"videos/demo1.mp4"}' "$TOKEN_A")
check "alice 发视频 v1" 200 "$code"
V1=$(video_id)
code=$(post "/api/video" \
  '{"title":"待删除视频 #2","url":"videos/demo2.mp4"}' "$TOKEN_A")
check "alice 发视频 v2" 200 "$code"
V2=$(video_id)
code=$(post "/api/video" \
  '{"title":"XSS <script>alert(1)</script> & q #3","url":"videos/demo3.mp4"}' "$TOKEN_A")
check "alice 发视频 v3（含 XSS 标题）" 200 "$code"
V3=$(video_id)
if [[ -z "$V1" || -z "$V2" || -z "$V3" ]]; then
  echo "视频 id 提取失败（V1=$V1 V2=$V2 V3=$V3）" >&2
  exit 1
fi
expect_eq "三条视频 id 互不相同" yes \
  "$([[ "$V1" != "$V2" && "$V2" != "$V3" && "$V1" != "$V3" ]] && echo yes || echo no)"

echo "[2/9] /v/$V1 页面本体"
curl -s -D "$TMP/h1" -o "$TMP/body" "$GW/v/$V1"
check "GET /v/$V1" 200 "$(sed -n '1s/.* \([0-9][0-9][0-9]\) .*/\1/p' "$TMP/h1")"
check "Content-Type 为 HTML" "text/html; charset=utf-8" \
  "$(sed -n 's/^[Cc]ontent-[Tt]ype: *//p' "$TMP/h1" | tr -d '\r' | head -1)"
expect_body "含 Cache-Control: no-store" 'Cache-Control: no-store' "$TMP/h1"
SIZE=$(wc -c <"$TMP/body")
if [[ "$SIZE" -gt 8000 && "$SIZE" -lt 200000 ]]; then
  expect_eq "页面体积合理（8KB~200KB）" ok ok
else
  expect_eq "页面体积合理（8KB~200KB）" ok "实际 ${SIZE} 字节"
fi
expect_body "含播放器 <video" '<video id="player"'
expect_body "播放器指向 videos/demo1.mp4" 'src="[^"]*videos/demo1.mp4"'
expect_body "含作者名 alice" '<b>alice</b>'
expect_body "含关注按钮" 'id="followBtn"'
expect_body "含点赞按钮" 'id="likeBtn"'
expect_body "含点赞数容器" 'id="likeStat"'
expect_body "含评论数容器" 'id="commentStat"'
expect_body "含评论区容器" 'id="commentList"'
expect_body "含下载按钮" 'id="downloadBtn"'
expect_body "含分享按钮" 'id="shareBtn"'
expect_body "评论输入框" 'id="commentInput"'
expect_body "移动端 viewport" 'name="viewport"'
expect_body "深色主题" 'name="color-scheme" content="dark"'

echo "[3/9] OG / Twitter 预览标签"
for tag in 'og:title' 'og:description' 'og:image' 'og:video' 'og:url' 'og:type'; do
  expect_body "meta $tag" "property=\"$tag\""
done
expect_body "twitter:card" 'name="twitter:card" content="summary_large_image"'
expect_body "twitter:title" 'name="twitter:title"'
expect_body "twitter:image" 'name="twitter:image"'
OG_IMAGE=$(sed -n 's/.*property="og:image" content="\([^"]*\)".*/\1/p' "$TMP/body" | head -1)
OG_VIDEO=$(sed -n 's/.*property="og:video" content="\([^"]*\)".*/\1/p' "$TMP/body" | head -1)
OG_URL=$(sed -n 's/.*property="og:url" content="\([^"]*\)".*/\1/p' "$TMP/body" | head -1)
expect_eq "og:image 为绝对地址" "http://127.0.0.1:$GW_PORT/videos/demo1.jpg" "$OG_IMAGE"
expect_eq "og:video 为绝对地址" "http://127.0.0.1:$GW_PORT/videos/demo1.mp4" "$OG_VIDEO"
expect_eq "og:url 指向落地页" "http://127.0.0.1:$GW_PORT/v/$V1" "$OG_URL"
expect_body "canonical 链接" 'rel="canonical"'

echo "[4/9] 标题转义（XSS）"
code=$(get "/v/$V3")
check "GET /v/$V3" 200 "$code"
expect_body "标题按 HTML 转义" '&lt;script&gt;alert(1)&lt;/script&gt;'
expect_absent "页面不含可执行 script 标签" '<script>alert(1)</script>'
expect_absent "OG 标题也不含原始标签" 'content="XSS <script>'

echo "[5/9] 评论区 SSR + 分页"
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"第一条评论\"}" "$TOKEN_A")
check "发第 1 条评论" 200 "$code"
for i in $(seq 2 25); do
  post "/api/comment" "{\"video_id\":$V1,\"content\":\"评论 $i\"}" "$TOKEN_A" >/dev/null
done
code=$(get "/api/comment/$V1?limit=100")
check "评论列表（limit=100）" 200 "$code"
expect_eq "评论总数 25" 25 "$(grep -o '"comment":{' "$TMP/body" | wc -l)"

code=$(get "/v/$V1")
check "GET /v/$V1（评论后）" 200 "$code"
expect_eq "SSR 首屏评论 20 条" 20 "$(grep -o 'class="cmt"' "$TMP/body" | wc -l)"
expect_body "首屏含最新评论" '评论 25'
CURSOR=$(sed -n 's/.*data-next-cursor="\([^"]*\)".*/\1/p' "$TMP/body" | head -1)
expect_eq "分页游标非空" yes "$([[ -n "$CURSOR" ]] && echo yes || echo no)"
expect_body "评论数显示 25" '<small id="commentTotal">25</small>'
code=$(get "/api/comment/$V1?limit=20&cursor=$CURSOR")
check "第二页评论接口" 200 "$code"
expect_eq "第二页 5 条" 5 "$(grep -o '"comment":{' "$TMP/body" | wc -l)"

echo "[6/9] 404：不存在 / 非法 / 0 / 已删除"
code=$(get "/v/999999")
check "GET /v/999999" 404 "$code"
expect_body "404 页含提示" '视频不存在或已被删除'
expect_body "404 页含回首页按钮" '回首页看更多'
expect_absent "404 页不泄漏作者信息" 'alice'
code=$(get "/v/abc")
check "GET /v/abc（非数字）" 404 "$code"
code=$(get "/v/0")
check "GET /v/0（非法 id）" 404 "$code"
code=$(delete_path "/api/video/$V2" "" "$TOKEN_A")
check "删除 v2" 200 "$code"
code=$(get "/v/$V2")
check "GET /v/<已删除>" 404 "$code"

echo "[7/9] 身份态：关注 / 点赞 / 拉黑"
code=$(post "/api/user/$UID_A/follow" '{}' "$TOKEN_B")
check "bob 关注 alice" 200 "$code"
code=$(post "/api/like" "{\"video_id\":$V1}" "$TOKEN_B")
check "bob 点赞 v1" 200 "$code"
code=$(get "/v/$V1" "$TOKEN_B")
check "bob 视角 GET /v/$V1" 200 "$code"
expect_body "bob 视角显示已关注" '已关注</button>'
expect_body "bob 视角关注按钮带 on" 'class="follow on"'
expect_body "bob 视角点赞按钮带 on" 'id="likeBtn" type="button" class="op on"'
expect_body "bob 视角点赞数 1" '<b id="likeStat">1</b>'
code=$(get "/v/$V1")
check "匿名视角 GET /v/$V1" 200 "$code"
expect_absent "匿名视角无已关注" '已关注</button>'
expect_body "匿名视角显示关注按钮" '>关注</button>'

code=$(post "/api/user/$UID_A/block" '{}' "$TOKEN_B")
check "bob 拉黑 alice" 200 "$code"
code=$(get "/v/$V1" "$TOKEN_B")
check "bob 拉黑后 GET /v/$V1" 404 "$code"
expect_body "拉黑后 404 页正常" '视频不存在或已被删除'
code=$(get "/v/$V1")
check "匿名不受个人黑名单影响" 200 "$code"
code=$(delete_path "/api/user/$UID_A/block" "" "$TOKEN_B")
check "bob 取消拉黑" 200 "$code"
code=$(get "/v/$V1" "$TOKEN_B")
check "取消拉黑后恢复" 200 "$code"

code=$(post "/api/user/$UID_C/block" '{}' "$TOKEN_A")
check "alice 拉黑 carol（作者拉黑 viewer）" 200 "$code"
code=$(get "/v/$V1" "$TOKEN_C")
check "carol 访问 alice 的落地页" 404 "$code"
code=$(delete_path "/api/user/$UID_C/block" "" "$TOKEN_A")
check "alice 取消拉黑 carol" 200 "$code"
code=$(get "/v/$V1" "$TOKEN_C")
check "carol 恢复访问" 200 "$code"

echo "[8/9] 回归：首页 / API / HEAD"
code=$(get "/")
check "GET /（首页 SPA）" 200 "$code"
expect_body "首页含 feed 容器" 'id="feed"'
expect_absent "首页不含落地页标记" 'id="shareBtn"'
code=$(get "/api/video/$V1")
check "GET /api/video/$V1" 200 "$code"
expect_body "视频详情 JSON 正常" '"ok":true'
code=$(head_code "/v/$V1")
check "HEAD /v/$V1" 200 "$code"
code=$(get "/v/$V1/")
check "GET /v/$V1/（尾斜杠）" 200 "$code"

echo "[9/9] 性能：GET /v/$V1 TTFB（20 次串行）"
TIMES=()
for _ in $(seq 1 20); do
  TIMES+=("$(curl -s -o /dev/null -w '%{time_starttransfer}' "$GW/v/$V1")")
done
STATS=$(printf '%s\n' "${TIMES[@]}" | awk '
  { s += $1; if ($1 > m) m = $1; n++ }
  END { printf "%.3f %.3f %d", (s / n) * 1000, m * 1000, n }')
AVG=$(echo "$STATS" | cut -d' ' -f1)
MAX=$(echo "$STATS" | cut -d' ' -f2)
COUNT=$(echo "$STATS" | cut -d' ' -f3)
printf '  TTFB avg=%s ms  max=%s ms（%s 次，本机 loopback）\n' "$AVG" "$MAX" "$COUNT"
if awk "BEGIN{exit !($AVG < 50)}"; then
  expect_eq "TTFB 平均 < 50 ms" ok ok
else
  expect_eq "TTFB 平均 < 50 ms" ok "实际 ${AVG} ms"
fi
if awk "BEGIN{exit !($MAX < 200)}"; then
  expect_eq "TTFB 最大 < 200 ms" ok ok
else
  expect_eq "TTFB 最大 < 200 ms" ok "实际 ${MAX} ms"
fi

echo
echo "==== 分享落地页验收：$PASSED PASS / $FAILED FAIL ===="
if [[ "$FAILED" -ne 0 ]]; then
  exit 1
fi
exit 0
