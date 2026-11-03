#!/usr/bin/env bash
# 抖音 C7 验收：live(29105) + user(29101) 共用一个 SQLite。
#
# 覆盖：
#   1) 注册 alice/bob + 8 个并发用户
#   2) 创建直播间（无 token 401 / 缺 title 400 / 正常 200 status=0）
#   3) 开播（bob 403 / 不存在 404 / 房主 200 status=1）
#   4) 列表过滤（无参 / status=1 只列直播中 / status=0 不含 / 非法 400）
#   5) 进出房（幂等：重复 enter/leave 不重复计数；不存在 404；无 token 401）
#   6) 并发：10 个用户 × 10 轮同时 enter/leave → 观众数回 0、全程不为负
#   7) 关播（bob 403 / 房主 200 status=0 / 列表 status=1 不含）
#   8) 压测 GET /api/live/rooms 1000 次 → QPS / p50 / p99
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
LIVE_PORT="${DOUYIN_LIVE_PORT:-29105}"
USER_BASE="http://127.0.0.1:$USER_PORT"
LIVE_BASE="http://127.0.0.1:$LIVE_PORT"
DB="$ROOT/data/accept_live.db"
LOG_DIR="$ROOT/logs"
TMP="$(mktemp -d)"
PIDS=()
PASSED=0
FAILED=0

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$TMP"
}
trap cleanup EXIT

check() { # name expected actual [detail]
  local name="$1" expected="$2" actual="$3" detail="${4:-}"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-40s PASS  HTTP %s %s\n' "$name" "$actual" "$detail"
    PASSED=$((PASSED + 1))
  else
    printf '  %-40s FAIL  HTTP %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-40s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-40s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-40s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-40s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

post() { # base path json [token]
  local base="$1" path="$2" payload="$3" token="${4:-}"
  local args=(-s -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$payload" "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

get() { # base path [token]
  local base="$1" path="$2" token="${3:-}"
  local args=(-s -o "$TMP/body" -w '%{http_code}' "$base$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

post_json() { # base path token -> body + \n + http_code
  local base="$1" path="$2" token="$3"
  curl -s -w $'\n%{http_code}' -X POST -H 'Content-Type: application/json' \
       -d '{}' -H "Authorization: Bearer $token" "$base$path"
}

room_count() { grep -o '"room":{"id"' "$TMP/body" | wc -l; }
room_ids() { grep -o '"room":{"id":[0-9]*' "$TMP/body" | grep -o '[0-9]*$'; }

room_field() { # id field
  local id="$1" field="$2"
  grep -o "\"room\":{\"id\":$id,[^}]*}" "$TMP/body" | head -1 |
    sed -n "s/.*\"$field\":\([0-9-]*\).*/\1/p"
}

start_service() { # bin port base name
  local bin="$1" port="$2" base="$3" name="$4"
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_live_$name.log" 2>&1 &
  PIDS+=("$!")
  local ready=0
  for _ in $(seq 1 200); do
    if curl -fsS "$base/health" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "$name 启动失败，日志：" >&2
    cat "$LOG_DIR/accept_live_$name.log" >&2
    exit 1
  fi
  printf '[start] %s %s（db=%s pid=%s）\n' "$name" "$base" "$DB" "$!"
}

declare -A TOKENS=()
declare -A IDS=()

register() { # name
  local name="$1" code
  code=$(post "$USER_BASE" /api/user/register \
    "{\"name\":\"$name\",\"email\":\"$name@example.com\",\"password\":\"secret123\"}")
  if [[ "$code" != "200" ]]; then
    printf '注册 %s 失败 HTTP %s：%s\n' "$name" "$code" "$(head -c 200 "$TMP/body")" >&2
    exit 1
  fi
  TOKENS[$name]=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body")
  IDS[$name]=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
  if [[ -z "${TOKENS[$name]}" || -z "${IDS[$name]}" ]]; then
    echo "token/user id 提取失败：$name" >&2
    exit 1
  fi
}

mkdir -p "$ROOT/data" "$LOG_DIR"
rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_live_"*.log

for bin in user_service live_service; do
  if [[ ! -x "$BIN_DIR/$bin" ]]; then
    echo "找不到 $BIN_DIR/$bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
done

start_service "$BIN_DIR/user_service" "$USER_PORT" "$USER_BASE" user_service
start_service "$BIN_DIR/live_service" "$LIVE_PORT" "$LIVE_BASE" live_service

echo "[1/8] 注册 alice / bob + 8 个并发用户"
register alice
register bob
for i in $(seq 1 8); do
  register "u$i"
done
check "alice 注册后 uid" "1" "${IDS[alice]}"
expect_eq "并发用户数（含 alice/bob）" 10 "$((${#TOKENS[@]}))"

echo "[2/8] 创建直播间"
code=$(post "$LIVE_BASE" /api/live/room '{"title":"alice 的直播间"}')
check "无 token 创建" 401 "$code"
code=$(post "$LIVE_BASE" /api/live/room '{}' "${TOKENS[alice]}")
check "缺 title 创建" 400 "$code"
code=$(post "$LIVE_BASE" /api/live/room '{"title":"alice 的直播间"}' "${TOKENS[alice]}")
check "alice 创建房间" 200 "$code"
expect_body "创建响应含 status=0" '"status":0'
expect_body "创建响应含 viewer_count=0" '"viewer_count":0'
expect_body "创建响应含标题" 'alice 的直播间'
ROOM=$(sed -n 's/.*"room":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
[[ -n "$ROOM" ]] || {
  echo "房间 id 提取失败" >&2
  exit 1
}
printf '  房间 id=%s 房主=%s\n' "$ROOM" "${IDS[alice]}"

echo "[3/8] 开播权限"
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/start" '{}' "${TOKENS[bob]}")
check "bob 开 alice 的房间" 403 "$code"
expect_body "403 响应含 code" '"code":403'
code=$(post "$LIVE_BASE" "/api/live/room/999999/start" '{}' "${TOKENS[alice]}")
check "开不存在房间" 404 "$code"
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/start" '{}')
check "开播无 token" 401 "$code"
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/start" '{}' "${TOKENS[alice]}")
check "房主开播" 200 "$code"
expect_body "开播后 status=1" '"status":1'
expect_eq "开播后房间状态" 1 "$(room_field "$ROOM" status)"

echo "[4/8] 列表过滤"
code=$(get "$LIVE_BASE" "/api/live/rooms")
check "列表（无参）" 200 "$code"
expect_eq "无参条数" 1 "$(room_count)"
expect_eq "无参房间 id" "$ROOM" "$(room_ids)"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=1")
check "列表 status=1" 200 "$code"
expect_eq "status=1 条数" 1 "$(room_count)"
expect_eq "status=1 房间 id" "$ROOM" "$(room_ids)"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=0")
check "列表 status=0" 200 "$code"
expect_eq "status=0 条数" 0 "$(room_count)"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=2")
check "列表 status=2" 400 "$code"

echo "[5/8] 进出房（幂等）"
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/enter" '{}' "${TOKENS[bob]}")
check "bob 进房" 200 "$code"
expect_body "进房 changed=true" '"changed":true'
expect_body "进房后 count=1" '"viewer_count":1'
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/enter" '{}' "${TOKENS[bob]}")
check "bob 重复进房" 200 "$code"
expect_body "重复进房 changed=false" '"changed":false'
expect_body "重复进房 count 仍=1" '"viewer_count":1'
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/enter" '{}' "${TOKENS[alice]}")
check "alice 进房" 200 "$code"
expect_body "两人进房 count=2" '"viewer_count":2'
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/leave" '{}' "${TOKENS[bob]}")
check "bob 出房" 200 "$code"
expect_body "出房 changed=true" '"changed":true'
expect_body "出房后 count=1" '"viewer_count":1'
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/leave" '{}' "${TOKENS[bob]}")
check "bob 重复出房" 200 "$code"
expect_body "重复出房 changed=false" '"changed":false'
expect_body "重复出房 count 仍=1" '"viewer_count":1'
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/leave" '{}' "${TOKENS[alice]}")
check "alice 出房" 200 "$code"
expect_body "全员出房 count=0" '"viewer_count":0'
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/enter" '{}')
check "进房无 token" 401 "$code"
code=$(post "$LIVE_BASE" "/api/live/room/999999/enter" '{}' "${TOKENS[alice]}")
check "进不存在房间" 404 "$code"
code=$(post "$LIVE_BASE" "/api/live/room/999999/leave" '{}' "${TOKENS[alice]}")
check "出不存在房间" 404 "$code"

echo "[6/8] 并发：10 用户 × 10 轮同时 enter/leave"
CONC_USERS=(alice bob u1 u2 u3 u4 u5 u6 u7 u8)

conc_worker() { # user rounds out
  local user="$1" rounds="$2" out="$3"
  local ok=0 bad=0 r resp code body
  for r in $(seq 1 "$rounds"); do
    resp=$(post_json "$LIVE_BASE" "/api/live/room/$ROOM/enter" "${TOKENS[$user]}")
    code=${resp##*$'\n'}
    body=${resp%$'\n'*}
    if [[ "$code" == "200" && "$body" == *'"changed":true'* &&
          "$body" != *'"viewer_count":-'* ]]; then
      ok=$((ok + 1))
    else
      bad=$((bad + 1))
    fi
    resp=$(post_json "$LIVE_BASE" "/api/live/room/$ROOM/leave" "${TOKENS[$user]}")
    code=${resp##*$'\n'}
    body=${resp%$'\n'*}
    if [[ "$code" == "200" && "$body" == *'"changed":true'* &&
          "$body" != *'"viewer_count":-'* ]]; then
      ok=$((ok + 1))
    else
      bad=$((bad + 1))
    fi
  done
  printf '%s %s\n' "$ok" "$bad" >"$out"
}

CONC_OK=0
CONC_BAD=0
CONC_WPIDS=()
for u in "${CONC_USERS[@]}"; do
  conc_worker "$u" 10 "$TMP/conc_$u.result" &
  CONC_WPIDS+=("$!")
done
wait "${CONC_WPIDS[@]}"
for u in "${CONC_USERS[@]}"; do
  read -r ok bad <"$TMP/conc_$u.result"
  CONC_OK=$((CONC_OK + ok))
  CONC_BAD=$((CONC_BAD + bad))
done
expect_eq "并发请求成功数（10×10×2）" 200 "$CONC_OK"
expect_eq "并发请求失败/负计数数" 0 "$CONC_BAD"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=1")
check "并发后查列表" 200 "$code"
expect_eq "并发后 viewer_count" 0 "$(room_field "$ROOM" viewer_count)"
expect_eq "并发后房间数" 1 "$(room_count)"

# 10 个不同用户同时进房 → count 期望正好 10；再同时出房 → 回 0。
CONC_WPIDS=()
for u in "${CONC_USERS[@]}"; do
  ( curl -s -o /dev/null -X POST -H "Authorization: Bearer ${TOKENS[$u]}" \
      "$LIVE_BASE/api/live/room/$ROOM/enter" ) &
  CONC_WPIDS+=("$!")
done
wait "${CONC_WPIDS[@]}"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=1")
check "10 人同时进房后列表" 200 "$code"
expect_eq "10 人同时进房后 count" 10 "$(room_field "$ROOM" viewer_count)"
CONC_WPIDS=()
for u in "${CONC_USERS[@]}"; do
  ( curl -s -o /dev/null -X POST -H "Authorization: Bearer ${TOKENS[$u]}" \
      "$LIVE_BASE/api/live/room/$ROOM/leave" ) &
  CONC_WPIDS+=("$!")
done
wait "${CONC_WPIDS[@]}"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=1")
check "10 人同时出房后列表" 200 "$code"
expect_eq "10 人同时出房后 count" 0 "$(room_field "$ROOM" viewer_count)"

echo "[7/8] 关播"
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/stop" '{}' "${TOKENS[bob]}")
check "bob 关 alice 的房间" 403 "$code"
code=$(post "$LIVE_BASE" "/api/live/room/999999/stop" '{}' "${TOKENS[alice]}")
check "关不存在房间" 404 "$code"
code=$(post "$LIVE_BASE" "/api/live/room/$ROOM/stop" '{}' "${TOKENS[alice]}")
check "房主关播" 200 "$code"
expect_body "关播后 status=0" '"status":0'
expect_eq "关播后房间状态" 0 "$(room_field "$ROOM" status)"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=1")
check "关播后列表 status=1" 200 "$code"
expect_eq "关播后直播中条数" 0 "$(room_count)"
code=$(get "$LIVE_BASE" "/api/live/rooms?status=0")
check "关播后列表 status=0" 200 "$code"
expect_eq "关播后未开播条数" 1 "$(room_count)"
code=$(get "$LIVE_BASE" "/metrics")
check "metrics" 200 "$code"
expect_body "metrics 含 requests" '"requests":'

echo "[8/8] 压测 GET /api/live/rooms（1000 次）"
BENCH_N=1000
BENCH_WORKERS=8
BENCH_PER=$((BENCH_N / BENCH_WORKERS))
bench_worker() { # out n
  local out="$1" n="$2"
  : >"$out"
  for _ in $(seq 1 "$n"); do
    curl -s -o /dev/null -w '%{http_code} %{time_total}\n' \
      "$LIVE_BASE/api/live/rooms" >>"$out"
  done
}
BENCH_START=$(date +%s%N)
BENCH_WPIDS=()
for w in $(seq 1 "$BENCH_WORKERS"); do
  bench_worker "$TMP/bench_$w.txt" "$BENCH_PER" &
  BENCH_WPIDS+=("$!")
done
wait "${BENCH_WPIDS[@]}"
BENCH_END=$(date +%s%N)
BENCH_MS=$(((BENCH_END - BENCH_START) / 1000000))
cat "$TMP"/bench_*.txt >"$TMP/bench_all.txt"
BENCH_TOTAL=$(wc -l <"$TMP/bench_all.txt")
BENCH_BAD=$(awk '$1 != 200' "$TMP/bench_all.txt" | wc -l)
expect_eq "压测请求数" 1000 "$BENCH_TOTAL"
expect_eq "压测非 200 数" 0 "$BENCH_BAD"
awk '$1 == 200 {print $2}' "$TMP/bench_all.txt" | sort -n >"$TMP/bench_lat.txt"
read -r P50 P99 < <(awk '{a[NR] = $1} END {
  n = NR; if (n == 0) { print "0 0"; exit }
  i50 = int(n * 0.50) + 1; if (i50 > n) i50 = n
  i99 = int(n * 0.99) + 1; if (i99 > n) i99 = n
  printf "%.3f %.3f\n", a[i50] * 1000, a[i99] * 1000
}' "$TMP/bench_lat.txt")
QPS=$(awk -v n="$BENCH_TOTAL" -v ms="$BENCH_MS" 'BEGIN {
  if (ms <= 0) ms = 1
  printf "%.1f", n * 1000 / ms
}')
printf '  [bench] N=%s 并发=%s 墙钟=%sms QPS=%s p50=%sms p99=%sms\n' \
  "$BENCH_TOTAL" "$BENCH_WORKERS" "$BENCH_MS" "$QPS" "$P50" "$P99"
PASSED=$((PASSED + 1))

echo "---------------------------------------------"
printf '[accept_live] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_live] PASS"
  exit 0
fi
echo "[accept_live] FAIL"
exit 1
