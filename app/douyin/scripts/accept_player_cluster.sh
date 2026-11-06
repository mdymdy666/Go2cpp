#!/usr/bin/env bash
# 播放体验三件套（手势增强 / 进度条预览 / 自动连播）验收（第 8 轮新增）。
#
# 用例分组：
#   [1] 关注流数据面：注册/发视频/关注/取关，GET /api/feed/following 的行为
#   [2] 手势增强：单击 250ms 延迟、双击点赞、长按 500ms 2×、上下滑 goSlide、
#       左右滑切「推荐/关注」——检查实际返回页面里的事件绑定与状态机
#   [3] 进度条预览：悬浮时间气泡、buffered 高亮、60ms 节流 seek、松手恢复播放
#   [4] 自动连播：默认开、ended → 下一条、到底提示并刷新、倍速/音量继承
#   [5] 空白与边界：空关注流、未登录切关注、首条/末条越界、切 tab 竞态丢弃
#   [6] 内联 JS 结构：抽取 <script> 做括号平衡校验（环境无 node 的替代）
#   [7] 性能：GET /、/api/feed、/api/feed/following 的 TTFB < 200 ms（20 次均值）
#   [8] 回归：健康检查、视频详情、分享落地页
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_player_cluster.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_player_cluster.pids"
DB="$ROOT/data/accept_player_cluster.db"
WEB="$ROOT/data/accept_player_cluster_web"
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

mkdir -p "$LOG_DIR" "$ROOT/data" "$WEB/videos" "$TMP"
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
    printf '  %-58s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-58s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  check "$1" "$2" "$3"
}

expect_has() { # name fixed_string [file]
  local name="$1" needle="$2" file="${3:-$TMP/home.html}"
  if grep -qF -- "$needle" "$file"; then
    printf '  %-58s PASS  含 %s\n' "$name" "$needle"
    PASSED=$((PASSED + 1))
  else
    printf '  %-58s FAIL  缺少 %s\n' "$name" "$needle"
    FAILED=$((FAILED + 1))
  fi
}

# ---- HTTP 封装 --------------------------------------------------------------
post() { # path json [token] [outfile]
  local path="$1" payload="$2" token="${3:-}" out="${4:-$TMP/body}"
  local args=(-s --max-time 5 -o "$out" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$payload" "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

del() { # path token [outfile]
  local path="$1" token="$2" out="${3:-$TMP/body}"
  curl -s --max-time 5 -o "$out" -w '%{http_code}' -X DELETE \
    -H "Authorization: Bearer $token" "$GW$path"
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
json_str() {
  grep -o "\"$1\":\"[^\"]*\"" "$TMP/body" | head -1 | \
    sed "s/^\"$1\":\"//; s/\"$//"
}
video_id() { sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }
items_count() { grep -o '"video":{"id":[0-9]*' "$TMP/body" | wc -l; }

# ---- 启动服务 ---------------------------------------------------------------
start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" DOUYIN_WEB_DIR="$WEB" "$bin" "$port" \
    >"$LOG_DIR/accept_player_cluster_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_player_cluster_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -rf "$WEB" "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_player_cluster_"*.log
mkdir -p "$WEB/videos"

echo "[0/8] 启动 7 个进程（6 业务 + gateway）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

register() { # name email -> token via $TMP/body
  post "/api/user/register" \
    "{\"name\":\"$1\",\"email\":\"$2\",\"password\":\"secret123\"}" >/dev/null
  json_str token
}

echo "[1/8] 用户 / 关注关系 / 关注流数据面"
TOKEN_A="$(register alice alice@player.test)"
UID_A="$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)"
TOKEN_B="$(register bob bob@player.test)"
UID_B="$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)"
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" || -z "$UID_A" || -z "$UID_B" ]]; then
  echo "注册失败（A=$TOKEN_A B=$TOKEN_B）" >&2
  exit 1
fi
expect_eq "alice/bob 是不同用户" yes \
  "$([[ "$UID_A" != "$UID_B" ]] && echo yes || echo no)"

V_A=()
for i in 1 2 3; do
  code=$(post "/api/video" \
    "{\"title\":\"播放测试 v$i\",\"url\":\"videos/demo$i.mp4\"}" "$TOKEN_A")
  check "alice 发第 $i 条视频" 200 "$code"
  V_A+=("$(video_id)")
done

code=$(get "/api/feed/following")
check "未登录关注流 → 401" 401 "$code"
code=$(get "/api/feed/following?limit=20" "$TOKEN_B")
check "bob 未关注时关注流 200" 200 "$code"
expect_eq "未关注时关注流为空" 0 "$(items_count)"

code=$(post "/api/user/$UID_A/follow" '{}' "$TOKEN_B")
check "bob 关注 alice" 200 "$code"
expect_eq "关注响应 following=true" true \
  "$(grep -o '"following":true' "$TMP/body" | head -1 | cut -d: -f2)"
code=$(get "/api/feed/following?limit=20" "$TOKEN_B")
check "关注后关注流 200" 200 "$code"
expect_eq "关注流含 alice 的 3 条" 3 "$(items_count)"
expect_eq "关注流作者全部是 alice" 3 \
  "$(grep -o "\"user_id\":$UID_A" "$TMP/body" | wc -l)"
expect_has "关注流响应带作者名" '"author":"alice"' "$TMP/body"

code=$(del "/api/user/$UID_A/follow" "$TOKEN_B")
check "bob 取关 alice" 200 "$code"
code=$(get "/api/feed/following?limit=20" "$TOKEN_B")
expect_eq "取关后关注流为空" 0 "$(items_count)"
code=$(post "/api/user/$UID_A/follow" '{}' "$TOKEN_B")
check "再次关注（供后续用例）" 200 "$code"

echo "[2/8] 前端手势增强（GET / 实际返回页面）"
code=$(get "/")
check "GET / 200" 200 "$code"
cp "$TMP/body" "$TMP/home.html"
SIZE=$(stat -c%s "$TMP/home.html")
expect_eq "页面非空（>150KB）" yes \
  "$([[ "$SIZE" -gt 150000 ]] && echo yes || echo no)"
printf '       页面大小 %s 字节\n' "$SIZE"

for pat in \
  'function bindPlayerGestures' \
  'TAP_DELAY_MS = 250' \
  'DOUBLE_TAP_MS = 280' \
  'LONG_PRESS_MS = 500' \
  'clearTapTimer' \
  'lastTapAt' \
  'pointerdown' \
  'pointerup' \
  'pointercancel' \
  'video.playbackRate = 2' \
  'speed-badge' \
  'slide.classList.add('"'"'speed-on'"'"')' \
  'function goSlide' \
  'function onSlideEnded' \
  'scrollIntoView({ block: '"'"'start'"'"', behavior: '"'"'smooth'"'"' })' \
  'function switchFeedTab' \
  'function stepFeedTab' \
  'stepFeedTab(dx < 0 ? 1 : -1)' \
  'feed-tab' \
  'data-tab="recommend"' \
  'data-tab="following"' \
  'api/feed/following?limit=20' \
  'feedTabs'
do
  expect_has "手势契约 $pat" "$pat"
done

echo "[3/8] 进度条预览"
for pat in \
  'progress-tip' \
  'progress-buffer' \
  'SEEK_THROTTLE_MS = 60' \
  'video.buffered' \
  'video.currentTime = w.start + ratio * d' \
  'function paintTip' \
  'function paintBuffer' \
  'function scheduleSeek' \
  'wasPlaying' \
  'if (wasPlaying)' \
  'video.play()' \
  'bar.addEventListener('"'"'pointermove'"'"'' \
  'paintTip(ratio)' \
  'hideTip'
do
  expect_has "进度契约 $pat" "$pat"
done

echo "[4/8] 自动连播"
for pat in \
  "AUTOPLAY_KEY = 'douyin_autoplay'" \
  "state.autoplay = localStorage.getItem(AUTOPLAY_KEY) !== '0'" \
  'video.loop = !state.autoplay' \
  'onSlideEnded(slide, false)' \
  'onSlideEnded(slide, true)' \
  '已到底，上滑刷新推荐流' \
  'setTimeout(function () { loadFeed(); }, 900)' \
  'function applyRate' \
  'RATE_STEPS' \
  'video.playbackRate = state.rate' \
  'video.volume = state.volume' \
  'applyRate()' \
  'applyAutoplay()' \
  'autoplayBtn' \
  'rateBtn'
do
  expect_has "连播契约 $pat" "$pat"
done

echo "[5/8] 空白 / 边界处理"
for pat in \
  'if (!slides.length) return false;' \
  'if (next < 0)' \
  'if (next >= slides.length) return false;' \
  '关注的人还没有作品' \
  '请先登录，再查看关注流' \
  '登录后才能看关注流，已切回推荐' \
  'if (state.feedTab !== tab) return;' \
  'Number(slide.dataset.endedAt) < 1200' \
  'if (!video) return;' \
  'if (!target || !target.closest) return false;' \
  'if (!items.length)' \
  '已经在「推荐」' \
  '已经在「关注」'
do
  expect_has "边界契约 $pat" "$pat"
done

echo "[6/8] 内联 JS 结构（<script> 括号平衡）"
if python3 - "$TMP/home.html" >"$TMP/jscheck" 2>&1 <<'PY'
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
            word = ""
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
then
  grep -q 'JSCHECK ok' "$TMP/jscheck"
  check "内联 JS 括号平衡" 0 0
  cat "$TMP/jscheck"
else
  check "内联 JS 括号平衡" 0 1
  cat "$TMP/jscheck"
fi

echo "[7/8] 性能（TTFB，20 次均值，含网关转发）"
measure_ttfb() { # path [token]
  local sum=0 t
  for _ in $(seq 1 20); do
    if [[ -n "${2:-}" ]]; then
      t=$(curl -s -o /dev/null -w '%{time_starttransfer}' \
        -H "Authorization: Bearer $2" "$GW$1")
    else
      t=$(curl -s -o /dev/null -w '%{time_starttransfer}' "$GW$1")
    fi
    sum=$(awk -v a="$sum" -v b="$t" 'BEGIN{print a+b}')
  done
  awk -v s="$sum" 'BEGIN{printf "%.2f", s*1000/20}'
}
AVG_HOME=$(measure_ttfb "/")
expect_eq "GET / TTFB < 200 ms" yes \
  "$(awk -v v="$AVG_HOME" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       GET /       avg=%s ms\n' "$AVG_HOME"
AVG_FEED=$(measure_ttfb "/api/feed?limit=20")
expect_eq "推荐流 TTFB < 200 ms" yes \
  "$(awk -v v="$AVG_FEED" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       /api/feed   avg=%s ms\n' "$AVG_FEED"
AVG_FOL=$(measure_ttfb "/api/feed/following?limit=20" "$TOKEN_B")
expect_eq "关注流 TTFB < 200 ms" yes \
  "$(awk -v v="$AVG_FOL" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       following   avg=%s ms\n' "$AVG_FOL"

echo "[8/8] 回归"
code=$(get "/health")
check "回归 GET /health" 200 "$code"
code=$(get "/api/health/all")
check "回归 GET /api/health/all" 200 "$code"
code=$(get "/api/feed?limit=20")
check "回归 GET /api/feed" 200 "$code"
expect_eq "回归 feed 至少 3 条" yes \
  "$([[ "$(items_count)" -ge 3 ]] && echo yes || echo no)"
code=$(get "/api/video/${V_A[0]}" "$TOKEN_A")
check "回归 GET /api/video/:id" 200 "$code"
code=$(get "/v/${V_A[0]}")
check "回归分享页 /v/:id" 200 "$code"
expect_has "分享页含 og:title" 'og:title' "$TMP/body"

echo "---------------------------------------------"
printf '[accept_player_cluster] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_player_cluster] PASS"
  exit 0
fi
echo "[accept_player_cluster] FAIL"
exit 1
