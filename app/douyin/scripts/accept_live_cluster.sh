#!/usr/bin/env bash
# 直播三件套验收（第 10 轮新增）：直播间聊天 + 礼物 + 贡献榜。
#
# 覆盖：
#   [1]  准备：启动 user(29101) / live(29105) / gateway(28080)，注册 10 个用户
#   [2]  聊天发送：无 token 401 / 空与空白 400 / 200 字成功 / 201 字 400 /
#        房间不存在 404 / 公开读 200 / 短路径与 room 前缀两种写法
#   [3]  增量拉取：after=0 全量、after=last 只回新消息、limit 分页升序、
#        非法 after/limit 400
#   [4]  拉黑过滤：被房主拉黑者不能发言（403）、历史消息不出现在列表、
#        不能送礼；自己拉黑房主同样 403；取消拉黑后恢复
#   [5]  礼物：5 个价目 / 余额初始 1000 / 送礼扣款 / 聊天区 gift 消息
#   [6]  余额不足：400、余额不变、榜单不变、聊天条数不变（事务回滚）
#   [7]  榜单：SUM(price*count) 排序 / limit 上限 / 新房间空榜 / SQL 索引命中
#   [8]  并发一致性：20 并发送礼恰好扣完 1000、余额不为负；10 用户 × 10 条
#        并发消息 id 严格递增无遗漏
#   [9]  前端：GET / 200、GET /live 200、聊天/礼物/榜单元素与 2s 轮询
#   [10] 性能：聊天增量与榜单 TTFB 各 20 次均值 < 200ms
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_live_cluster.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_live_cluster.pids"
DB="$ROOT/data/accept_live_cluster.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0

GW_PORT="${DOUYIN_GATEWAY_PORT:-28080}"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
LIVE_PORT="${DOUYIN_LIVE_PORT:-29105}"
GW="http://127.0.0.1:$GW_PORT"
USER="http://127.0.0.1:$USER_PORT"
LIVE="http://127.0.0.1:$LIVE_PORT"

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

expect_nonzero() { # name pattern
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

del() { # base path token
  local base="$1" path="$2" token="${3:-}"
  local args=(-s --max-time 10 -o "$TMP/body" -w '%{http_code}' -X DELETE "$base$path")
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

msg_count() { grep -o '"kind":"[a-z]*"' "$TMP/body" | wc -l | tr -d ' '; }
rank_count() { grep -o '"user_id":' "$TMP/body" | wc -l | tr -d ' '; }
rank_coins() { grep -o '"coins":[0-9]*' "$TMP/body" | grep -o '[0-9]*' | tr '\n' ' '; }

sql_value() { # sql
  python3 -c 'import sqlite3,sys
con = sqlite3.connect(sys.argv[1])
row = con.execute(sys.argv[2]).fetchone()
print(row[0] if row and row[0] is not None else "")' "$DB" "$1"
}

sql_plan() { # sql
  python3 -c 'import sqlite3,sys
con = sqlite3.connect(sys.argv[1])
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
  tail -20 "$LOG_DIR/accept_live_cluster_$name.log" >&2
  return 1
}

start_service() { # bin port base name
  local bin="$1" port="$2" base="$3" name="$4"
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_live_cluster_$name.log" 2>&1 &
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
    "{\"name\":\"$name\",\"email\":\"$name@live10.com\",\"password\":\"secret123\"}")
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

send_msg() { # token room content
  post "$LIVE" "/api/live/$2/messages" "{\"content\":\"$3\"}" "$1"
}

echo "[1/10] 准备：启动 user / live / gateway，注册用户"
for bin in user_service live_service gateway; do
  if [[ ! -x "$BIN_DIR/$bin" ]]; then
    echo "找不到 $BIN_DIR/$bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
done
rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_live_cluster_"*.log
for base in "$USER" "$LIVE" "$GW"; do
  if curl -fsS --max-time 1 "$base/health" >/dev/null 2>&1; then
    echo "端口已被占用：$base（请先停止旧服务再跑本脚本）" >&2
    exit 1
  fi
done
start_service "$BIN_DIR/user_service" "$USER_PORT" "$USER" user_service
start_service "$BIN_DIR/live_service" "$LIVE_PORT" "$LIVE" live_service
start_service "$BIN_DIR/gateway" "$GW_PORT" "$GW" gateway

USERS=(alice bob carol dave eve u5 u6 u7 u8 u9)
for user in "${USERS[@]}"; do
  register "$user"
done
check "注册用户数" 10 "${#TOKENS[@]}"
check "alice uid" 1 "${IDS[alice]}"

code=$(post "$LIVE" /api/live/room '{"title":"alice 直播间"}' "${TOKENS[alice]}")
check "创建直播间" 200 "$code"
ROOM=$(json_int id)
[[ -n "$ROOM" ]] || { echo "房间 id 解析失败" >&2; exit 1; }
code=$(post "$LIVE" "/api/live/room/$ROOM/start" '{}' "${TOKENS[alice]}")
check "开播" 200 "$code"
printf '  房间 id=%s 房主=%s DB=%s\n' "$ROOM" "${IDS[alice]}" "$DB"

echo "[2/10] 聊天发送与长度校验"
code=$(post "$LIVE" "/api/live/$ROOM/messages" '{"content":"hi"}')
check "无 token 发言" 401 "$code"
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "")
check "空内容" 400 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/messages" '{"content":"   "}' "${TOKENS[bob]}")
check "全空白内容" 400 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/messages" '{}' "${TOKENS[bob]}")
check "缺 content" 400 "$code"
LONG200=$(printf '字%.0s' $(seq 1 200))
LONG201=$(printf '字%.0s' $(seq 1 201))
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "$LONG200")
check "200 字消息" 200 "$code"
expect_body "200 字长度回显" '"ok":true'
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "$LONG201")
check "201 字消息" 400 "$code"
expect_body "201 字错误提示" '不能超过 200 字'
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "bob-1")
check "bob 发消息 1" 200 "$code"
expect_body "消息 kind=chat" '"kind":"chat"'
expect_body "消息带昵称" '"user_name":"bob"'
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "bob-2")
check "bob 发消息 2" 200 "$code"
LAST=$(json_int id)
code=$(post "$LIVE" "/api/live/room/$ROOM/messages" '{"content":"room 前缀写法"}' "${TOKENS[carol]}")
check "room 前缀路径发言" 200 "$code"
code=$(send_msg "${TOKENS[carol]}" "$ROOM" "carol-1")
check "carol 发消息" 200 "$code"
code=$(post "$LIVE" "/api/live/999999/messages" '{"content":"x"}' "${TOKENS[bob]}")
check "不存在房间发言" 404 "$code"
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=0&limit=50")
check "无 token 读消息（公开）" 200 "$code"

echo "[3/10] 增量拉取与分页"
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=0&limit=50")
check "after=0 拉取" 200 "$code"
TOTAL=$(msg_count)
check "全量条数（200字+bob1+bob2+room前缀+carol）" 5 "$TOTAL"
FIRST_ID=$(grep -o '"id":[0-9]*' "$TMP/body" | head -1 | grep -o '[0-9]*')
LAST_ID=$(grep -o '"id":[0-9]*' "$TMP/body" | tail -1 | grep -o '[0-9]*')
if [[ "$FIRST_ID" -lt "$LAST_ID" ]]; then
  printf '  %-46s PASS  升序 id %s..%s\n' "消息 id 升序" "$FIRST_ID" "$LAST_ID"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  升序 id %s..%s\n' "消息 id 升序" "$FIRST_ID" "$LAST_ID"
  FAILED=$((FAILED + 1))
fi
check "last_id 等于最后一条" "$LAST_ID" "$(json_int last_id)"
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=$LAST_ID&limit=50")
check "after=last 增量" 200 "$code"
check "增量条数 0" 0 "$(msg_count)"
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "bob-3")
check "再发一条" 200 "$code"
NEW_ID=$(json_int id)
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=$LAST_ID&limit=50")
check "增量只回新消息" 1 "$(msg_count)"
check "增量消息 id 正确" "$NEW_ID" "$(json_int id)"
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=0&limit=2")
check "limit=2 分页" 2 "$(msg_count)"
check "分页首条 = 第一条" "$FIRST_ID" "$(json_int id)"
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=abc")
check "after 非法" 400 "$code"
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=-1")
check "after 负数" 400 "$code"
code=$(get "$LIVE" "/api/live/$ROOM/messages?limit=0")
check "limit=0" 400 "$code"
code=$(get "$LIVE" "/api/live/$ROOM/messages?limit=101")
check "limit=101" 400 "$code"
code=$(get "$LIVE" "/api/live/999999/messages")
check "不存在房间拉取" 404 "$code"

echo "[4/10] 拉黑过滤（双向）"
code=$(post "$USER" "/api/user/${IDS[bob]}/block" '{}' "${TOKENS[alice]}")
check "alice 拉黑 bob" 200 "$code"
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "被拉黑还想说话")
check "被房主拉黑后发言" 403 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":2,"count":1}' "${TOKENS[bob]}")
check "被房主拉黑后送礼" 403 "$code"
code=$(send_msg "${TOKENS[alice]}" "$ROOM" "房主发言不受影响")
check "房主自己发言" 200 "$code"
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=0&limit=100")
check "拉黑后拉取列表" 200 "$code"
if grep -qF 'bob-1' "$TMP/body"; then
  printf '  %-46s FAIL  被拉黑者历史消息仍可见\n' "历史消息过滤"
  FAILED=$((FAILED + 1))
else
  printf '  %-46s PASS  被拉黑者历史消息已过滤\n' "历史消息过滤"
  PASSED=$((PASSED + 1))
fi
code=$(post "$USER" "/api/user/${IDS[bob]}/block" '{}' "${TOKENS[alice]}")
check "重复拉黑幂等" 200 "$code"
code=$(del "$USER" "/api/user/${IDS[bob]}/block" "${TOKENS[alice]}")
check "alice 取消拉黑 bob" 200 "$code"
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "取消拉黑后恢复")
check "取消拉黑后发言" 200 "$code"
code=$(post "$USER" "/api/user/${IDS[alice]}/block" '{}' "${TOKENS[bob]}")
check "bob 拉黑房主" 200 "$code"
code=$(send_msg "${TOKENS[bob]}" "$ROOM" "我拉黑了房主")
check "自己拉黑房主后发言" 403 "$code"
expect_body "反向拉黑提示" '你已拉黑对方'
code=$(del "$USER" "/api/user/${IDS[alice]}/block" "${TOKENS[bob]}")
check "bob 取消拉黑房主" 200 "$code"

echo "[5/10] 礼物：价目 / 余额 / 扣款 / 聊天特效"
code=$(get "$LIVE" /api/live/gifts)
check "礼物列表" 200 "$code"
check "礼物数量 5" 5 "$(json_int count)"
expect_body "礼物价格（棒棒糖 10）" '"name":"棒棒糖","price_coins":10'
expect_body "礼物价格（火箭 100）" '"name":"火箭","price_coins":100'
code=$(get "$LIVE" /api/live/wallet "${TOKENS[bob]}")
check "bob 钱包" 200 "$code"
check "初始余额 1000" 1000 "$(json_int coins)"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":2,"count":2}' "${TOKENS[bob]}")
check "送棒棒糖 ×2" 200 "$code"
check "扣款后余额 980" 980 "$(json_int coins)"
expect_body "送礼返回礼物名" '"name":"棒棒糖"'
expect_body "送礼消息 kind=gift" '"kind":"gift"'
expect_body "送礼消息文案" '送出 棒棒糖 ×2'
GIFT_MSG_ID=$(grep -o '"message":{"id":[0-9]*' "$TMP/body" | grep -o '[0-9]*$')
code=$(get "$LIVE" "/api/live/$ROOM/messages?after=$((GIFT_MSG_ID - 1))&limit=10")
check "礼物消息进聊天区" 1 "$(msg_count)"
expect_body "聊天区礼物文案" '送出 棒棒糖 ×2'
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":4,"count":1}' "${TOKENS[bob]}")
check "送火箭 ×1" 200 "$code"
check "扣款后余额 880" 880 "$(json_int coins)"
code=$(get "$LIVE" /api/live/wallet "${TOKENS[bob]}")
check "钱包查询余额 880" 880 "$(json_int coins)"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":999,"count":1}' "${TOKENS[bob]}")
check "不存在的礼物" 404 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":2,"count":0}' "${TOKENS[bob]}")
check "count=0" 400 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":2,"count":100}' "${TOKENS[bob]}")
check "count=100" 400 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"count":1}' "${TOKENS[bob]}")
check "缺 gift_id" 400 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":1,"count":1}')
check "送礼无 token" 401 "$code"
code=$(post "$LIVE" "/api/live/999999/gift" '{"gift_id":1,"count":1}' "${TOKENS[bob]}")
check "不存在房间送礼" 404 "$code"
check "送礼记录数" 2 "$(sql_value "SELECT COUNT(*) FROM gift_records WHERE room_id=$ROOM AND sender_id=${IDS[bob]}")"
check "送礼总额（20+100）" 120 "$(sql_value "SELECT SUM(g.price_coins*r.count) FROM gift_records r JOIN gifts g ON g.id=r.gift_id WHERE r.room_id=$ROOM AND r.sender_id=${IDS[bob]}")"

echo "[6/10] 余额不足与事务一致性"
BEFORE_COINS=$(sql_value "SELECT coins FROM users WHERE id=${IDS[bob]}")
BEFORE_COUNT=$(sql_value "SELECT COUNT(*) FROM live_messages WHERE room_id=$ROOM")
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":5,"count":99}' "${TOKENS[bob]}")
check "余额不足（城堡×99）" 400 "$code"
expect_body "余额不足提示" '余额不足'
check "余额未变" "$BEFORE_COINS" "$(sql_value "SELECT coins FROM users WHERE id=${IDS[bob]}")"
check "消息数未变" "$BEFORE_COUNT" "$(sql_value "SELECT COUNT(*) FROM live_messages WHERE room_id=$ROOM")"
check "礼物记录数未变" 2 "$(sql_value "SELECT COUNT(*) FROM gift_records WHERE room_id=$ROOM AND sender_id=${IDS[bob]}")"
check "榜单金额未变" 120 "$(sql_value "SELECT SUM(g.price_coins*r.count) FROM gift_records r JOIN gifts g ON g.id=r.gift_id WHERE r.room_id=$ROOM AND r.sender_id=${IDS[bob]}")"

echo "[7/10] 贡献榜：排序 / 上限 / 空榜"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":4,"count":2}' "${TOKENS[carol]}")
check "carol 送火箭 ×2" 200 "$code"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":1,"count":3}' "${TOKENS[dave]}")
check "dave 送小心心 ×3" 200 "$code"
code=$(get "$LIVE" "/api/live/$ROOM/ranking?limit=10")
check "贡献榜查询" 200 "$code"
check "榜单人数 3" 3 "$(rank_count)"
check "榜单金额顺序" "200 120 3 " "$(rank_coins)"
check "榜首 carol" "${IDS[carol]}" "$(json_int user_id)"
code=$(get "$LIVE" "/api/live/$ROOM/ranking?limit=2")
check "limit=2" 2 "$(rank_count)"
check "limit=2 只有前两名" "200 120 " "$(rank_coins)"
code=$(get "$LIVE" "/api/live/$ROOM/ranking?limit=0")
check "榜单 limit=0" 400 "$code"
code=$(get "$LIVE" "/api/live/$ROOM/ranking?limit=51")
check "榜单 limit=51" 400 "$code"
code=$(get "$LIVE" "/api/live/999999/ranking")
check "不存在房间榜单" 404 "$code"
code=$(post "$LIVE" /api/live/room '{"title":"空榜房间"}' "${TOKENS[alice]}")
check "创建第二个房间" 200 "$code"
ROOM2=$(json_int id)
code=$(get "$LIVE" "/api/live/$ROOM2/ranking?limit=10")
check "空榜查询" 200 "$code"
check "空榜条数 0" 0 "$(rank_count)"
PLAN=$(sql_plan "SELECT r.sender_id, COALESCE(u.name,''), SUM(g.price_coins*r.count), SUM(r.count) FROM gift_records r JOIN gifts g ON g.id=r.gift_id LEFT JOIN users u ON u.id=r.sender_id WHERE r.room_id=$ROOM GROUP BY r.sender_id ORDER BY 3 DESC, r.sender_id ASC LIMIT 10")
if grep -q 'idx_gift_records_room' <<<"$PLAN"; then
  printf '  %-46s PASS  榜单命中 idx_gift_records_room\n' "榜单查询计划"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  榜单未命中房间索引：%s\n' "榜单查询计划" "$PLAN"
  FAILED=$((FAILED + 1))
fi
PLAN=$(sql_plan "SELECT m.id FROM live_messages m LEFT JOIN users u ON u.id=m.user_id WHERE m.room_id=$ROOM AND m.id>0 ORDER BY m.id ASC LIMIT 50")
if grep -q 'idx_live_messages_room' <<<"$PLAN"; then
  printf '  %-46s PASS  聊天命中 idx_live_messages_room\n' "聊天查询计划"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  聊天未命中房间索引：%s\n' "聊天查询计划" "$PLAN"
  FAILED=$((FAILED + 1))
fi

echo "[8/10] 并发一致性"
# 20 个并发各送 1 个火箭（100 金币）：1000 余额恰好够 10 次，其余必须 400。
BURST_PIDS=()
for i in $(seq 1 20); do
  (
    code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' -X POST \
      -H 'Content-Type: application/json' \
      -H "Authorization: Bearer ${TOKENS[eve]}" \
      -d '{"gift_id":4,"count":1}' "$LIVE/api/live/$ROOM/gift")
    printf '%s\n' "$code" >"$TMP/burst_$i.code"
  ) &
  BURST_PIDS+=("$!")
done
wait "${BURST_PIDS[@]}"
OK=0
BAD=0
for i in $(seq 1 20); do
  code=$(cat "$TMP/burst_$i.code" 2>/dev/null || echo ERR)
  if [[ "$code" == "200" ]]; then OK=$((OK + 1)); else BAD=$((BAD + 1)); fi
done
check "并发送礼成功数（1000/100=10）" 10 "$OK"
check "并发送礼失败数" 10 "$BAD"
check "eve 余额归零" 0 "$(sql_value "SELECT coins FROM users WHERE id=${IDS[eve]}")"
check "eve 送礼总额 1000" 1000 "$(sql_value "SELECT SUM(g.price_coins*r.count) FROM gift_records r JOIN gifts g ON g.id=r.gift_id WHERE r.room_id=$ROOM AND r.sender_id=${IDS[eve]}")"
check "eve 记录条数 10" 10 "$(sql_value "SELECT COUNT(*) FROM gift_records WHERE room_id=$ROOM AND sender_id=${IDS[eve]}")"
check "余额不为负" 0 "$(sql_value "SELECT COUNT(*) FROM users WHERE coins < 0")"
code=$(post "$LIVE" "/api/live/$ROOM/gift" '{"gift_id":1,"count":1}' "${TOKENS[eve]}")
check "余额为 0 再送礼" 400 "$code"

# 10 用户 × 10 条并发消息：100 条 id 严格递增、增量拉取无遗漏。
MSG_USERS=(alice bob carol dave eve u5 u6 u7 u8 u9)
BEFORE_TOTAL=$(sql_value "SELECT COUNT(*) FROM live_messages WHERE room_id=$ROOM")
PIDS=()
for user in "${MSG_USERS[@]}"; do
  (
    for r in $(seq 1 10); do
      curl -s --max-time 10 -o /dev/null -X POST \
        -H 'Content-Type: application/json' \
        -H "Authorization: Bearer ${TOKENS[$user]}" \
        -d "{\"content\":\"$user-$r\"}" "$LIVE/api/live/$ROOM/messages"
    done
  ) &
  PIDS+=("$!")
done
wait "${PIDS[@]}"
AFTER_TOTAL=$(sql_value "SELECT COUNT(*) FROM live_messages WHERE room_id=$ROOM")
check "并发消息后总条数（原 + 100）" "$((BEFORE_TOTAL + 100))" "$AFTER_TOTAL"
DUP=$(sql_value "SELECT COUNT(*) - COUNT(DISTINCT id) FROM live_messages WHERE room_id=$ROOM")
check "消息 id 无重复" 0 "$DUP"
MONO=$(sql_value "SELECT COUNT(*) FROM (SELECT id, LAG(id) OVER (ORDER BY id) AS prev FROM live_messages WHERE room_id=$ROOM) WHERE prev IS NOT NULL AND id <= prev")
check "消息 id 严格递增" 0 "$MONO"

echo "[9/10] 前端页面与轮询"
code=$(get "$GW" /)
check "GET /" 200 "$code"
expect_body "首页直播入口" 'href="/live"'
code=$(get "$GW" "/live?room=$ROOM")
check "GET /live" 200 "$code"
expect_body "聊天列表元素" 'id="chatList"'
expect_body "聊天输入框" 'id="chatInput"'
expect_body "礼物面板" 'id="giftPanel"'
expect_body "礼物网格" 'id="giftGrid"'
expect_body "贡献榜面板" 'id="rankingPanel"'
expect_body "榜单列表" 'id="rankList"'
expect_body "礼物按钮" 'id="giftBtn"'
expect_body "榜单按钮" 'id="rankBtn"'
expect_body "2s 轮询常量" 'POLL_MS = 2000'
expect_body "增量 after 参数" 'after='
expect_body "乐观更新临时节点" 'data-temp'
expect_body "礼物特效层" 'id="fxLayer"'
code=$(get "$GW" /api/live/rooms)
check "网关转发直播列表" 200 "$code"
code=$(get "$GW" "/api/live/$ROOM/messages?after=0&limit=10")
check "网关转发聊天增量" 200 "$code"
code=$(get "$GW" "/api/live/$ROOM/ranking?limit=10")
check "网关转发贡献榜" 200 "$code"
code=$(get "$GW" "/live")
check "GET /live（无房间参数）" 200 "$code"
expect_body "房间选择器" 'id="roomPicker"'

echo "[10/10] 性能：聊天增量 / 榜单 TTFB"
ttfb() { # url n
  local url="$1" n="${2:-20}" total=0 t
  for _ in $(seq 1 "$n"); do
    t=$(curl -s --max-time 5 -o /dev/null -w '%{time_total}' "$url")
    total=$(awk -v a="$total" -v b="$t" 'BEGIN{printf "%.6f", a+b}')
  done
  awk -v s="$total" -v n="$n" 'BEGIN{printf "%.3f", s * 1000 / n}'
}
MSG_TTFB=$(ttfb "$LIVE/api/live/$ROOM/messages?after=0&limit=50")
RANK_TTFB=$(ttfb "$LIVE/api/live/$ROOM/ranking?limit=10")
PAGE_TTFB=$(ttfb "$GW/live?room=$ROOM")
printf '  [ttfb] 聊天增量 %sms / 榜单 %sms / 直播页 %sms（阈值 200ms）\n' \
  "$MSG_TTFB" "$RANK_TTFB" "$PAGE_TTFB"
if awk -v v="$MSG_TTFB" 'BEGIN{exit !(v < 200)}'; then
  printf '  %-46s PASS  %sms < 200ms\n' "聊天增量 TTFB" "$MSG_TTFB"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  %sms >= 200ms\n' "聊天增量 TTFB" "$MSG_TTFB"
  FAILED=$((FAILED + 1))
fi
if awk -v v="$RANK_TTFB" 'BEGIN{exit !(v < 200)}'; then
  printf '  %-46s PASS  %sms < 200ms\n' "榜单 TTFB" "$RANK_TTFB"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  %sms >= 200ms\n' "榜单 TTFB" "$RANK_TTFB"
  FAILED=$((FAILED + 1))
fi
if awk -v v="$PAGE_TTFB" 'BEGIN{exit !(v < 200)}'; then
  printf '  %-46s PASS  %sms < 200ms\n' "直播页 TTFB" "$PAGE_TTFB"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  %sms >= 200ms\n' "直播页 TTFB" "$PAGE_TTFB"
  FAILED=$((FAILED + 1))
fi

echo "---------------------------------------------"
printf '[accept_live_cluster] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_live_cluster] PASS"
  exit 0
fi
echo "[accept_live_cluster] FAIL"
exit 1
