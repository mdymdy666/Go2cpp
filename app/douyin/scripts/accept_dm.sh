#!/usr/bin/env bash
# 抖音私信（DM）验收：user(29101) + dm(29109) + gateway(28080) 共用一个 SQLite。
#
# 覆盖：
#   1) 注册 alice / bob / carol
#   2) 发送：无 token 401、content 缺失/空/501 字 400、正常发送、
#      会话复用（同一对用户只建一个会话）
#   3) 会话列表：对方昵称 / 最后一条 / 未读数；分页游标
#   4) 增量拉取：after=0 全量、after=last 空页、after 非法 400
#   5) 已读：标记后未读归零、重复标记幂等
#   6) 越权：carol 拉取/标记别人会话 403；不存在会话 404
#   7) 拉黑：bob 拉黑 alice 后 alice 发私信 403；取消后恢复
#   8) 并发：10 个用户各发 5 条 → 无丢失、无重复；页面元素齐全
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_dm.pids"
DB="$ROOT/data/accept_dm.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0

GW_PORT="${DOUYIN_GATEWAY_PORT:-28080}"
USER_PORT="${DOUYIN_USER_PORT:-29101}"
DM_PORT="${DOUYIN_DM_PORT:-29109}"
GW="http://127.0.0.1:$GW_PORT"

mkdir -p "$LOG_DIR" "$ROOT/data" "$TMP"
: >"$PID_FILE"

cleanup() {
  local name pid port expected actual
  if [[ -f "$PID_FILE" ]]; then
    while read -r name pid port; do
      [[ -n "${name:-}" && -n "${pid:-}" ]] || continue
      expected="$BIN_DIR/$name"
      actual="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
      if [[ -n "$actual" && "$actual" != "$expected" ]]; then
        continue
      fi
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    done <"$PID_FILE"
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_dm_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_dm_$name.log" >&2
    exit 1
  fi
  printf '[start] %-12s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_dm_"*.log

echo "[0/8] 启动 user / dm / gateway"
start_service user_service "$USER_PORT"
start_service dm_service "$DM_PORT"
start_service gateway "$GW_PORT"

echo "[1/8] 注册 alice / bob / carol"
TOKEN_A=""; UID_A=""; TOKEN_B=""; UID_B=""; TOKEN_C=""; UID_C=""
for who in alice bob carol; do
  code=$(post "/api/user/register" \
    "{\"name\":\"$who\",\"email\":\"$who@example.com\",\"password\":\"secret123\"}")
  check "注册 $who" 200 "$code"
  tok=$(json_str token)
  uid=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
  case "$who" in
    alice) TOKEN_A="$tok"; UID_A="$uid" ;;
    bob) TOKEN_B="$tok"; UID_B="$uid" ;;
    carol) TOKEN_C="$tok"; UID_C="$uid" ;;
  esac
done
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" || -z "$TOKEN_C" ]]; then
  echo "token 提取失败" >&2
  exit 1
fi

echo "[2/8] 发送与校验"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B,\"content\":\"hi\"}" "")
check "无 token 发送" 401 "$code"
code=$(post "/api/dm/send" "{\"content\":\"hi\"}" "$TOKEN_A")
check "缺 to_user_id" 400 "$code"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B}" "$TOKEN_A")
check "缺 content" 400 "$code"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B,\"content\":\"\"}" "$TOKEN_A")
check "空 content" 400 "$code"
LONG=$(python3 -c 'print("字" * 501)')
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B,\"content\":\"$LONG\"}" "$TOKEN_A")
check "501 字 content" 400 "$code"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_A,\"content\":\"self\"}" "$TOKEN_A")
check "给自己发" 400 "$code"
code=$(post "/api/dm/send" "{\"to_user_id\":999999,\"content\":\"x\"}" "$TOKEN_A")
check "目标用户不存在" 404 "$code"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B,\"content\":\"你好 bob\"}" "$TOKEN_A")
check "alice 首发" 200 "$code"
CONV=$(json_int conversation_id)
expect_body "返回 message" '"message":{'
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B,\"content\":\"第二条\"}" "$TOKEN_A")
check "alice 发第二条" 200 "$code"
expect_eq "会话复用（同一 id）" "$CONV" "$(json_int conversation_id)"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_A,\"content\":\"你好 alice\"}" "$TOKEN_B")
check "bob 回复" 200 "$code"

echo "[3/8] 会话列表与分页"
code=$(get "/api/dm/conversations" "$TOKEN_A")
check "alice 会话列表" 200 "$code"
expect_body "含对方昵称 bob" '"peer_name":"bob"'
expect_body "含最后一条" '"last_content":"你好 alice"'
expect_eq "alice 未读 1" 1 "$(json_int unread)"
code=$(get "/api/dm/conversations?limit=0" "$TOKEN_A")
check "limit=0 非法" 400 "$code"
code=$(get "/api/dm/conversations?limit=51" "$TOKEN_A")
check "limit=51 非法" 400 "$code"
code=$(get "/api/dm/conversations?cursor=abc" "$TOKEN_A")
check "cursor 非法" 400 "$code"
code=$(get "/api/dm/conversations?limit=1" "$TOKEN_A")
check "分页 limit=1" 200 "$code"
CUR=$(json_str next_cursor)
if [[ -n "$CUR" ]]; then
  code=$(get "/api/dm/conversations?limit=1&cursor=$CUR" "$TOKEN_A")
  check "分页第二页" 200 "$code"
fi

echo "[4/8] 增量拉取"
code=$(get "/api/dm/$CONV/messages?after=0" "$TOKEN_A")
check "after=0 全量" 200 "$code"
expect_eq "消息条数 3" 3 "$(grep -o '"sender_id":' "$TMP/body" | wc -l)"
LAST=$(json_int last_id)
code=$(get "/api/dm/$CONV/messages?after=$LAST" "$TOKEN_A")
check "after=last 增量" 200 "$code"
expect_eq "增量空页" 0 "$(grep -o '"sender_id":' "$TMP/body" | wc -l)"
code=$(get "/api/dm/$CONV/messages?after=abc" "$TOKEN_A")
check "after 非法" 400 "$code"
code=$(get "/api/dm/$CONV/messages?limit=101" "$TOKEN_A")
check "limit=101 非法" 400 "$code"

echo "[5/8] 已读"
code=$(post "/api/dm/$CONV/read" '{}' "$TOKEN_A")
check "alice 标记已读" 200 "$code"
expect_body "返回 updated" '"updated":'
code=$(get "/api/dm/conversations" "$TOKEN_A")
check "已读后列表" 200 "$code"
expect_eq "未读归零" 0 "$(json_int unread)"
code=$(post "/api/dm/$CONV/read" '{}' "$TOKEN_A")
check "重复标记幂等" 200 "$code"

echo "[6/8] 越权与非成员"
code=$(get "/api/dm/$CONV/messages?after=0" "$TOKEN_C")
check "carol 拉取别人会话" 403 "$code"
code=$(post "/api/dm/$CONV/read" '{}' "$TOKEN_C")
check "carol 标记别人会话" 403 "$code"
code=$(get "/api/dm/999999/messages" "$TOKEN_A")
check "不存在会话" 404 "$code"
code=$(get "/api/dm/conversations" "")
check "匿名会话列表" 401 "$code"

echo "[7/8] 拉黑过滤"
code=$(post "/api/user/$UID_A/block" '{}' "$TOKEN_B")
check "bob 拉黑 alice" 200 "$code"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B,\"content\":\"还能发吗\"}" "$TOKEN_A")
check "被拉黑后发私信" 403 "$code"
code=$(post "/api/user/$UID_A/block" '{}' "$TOKEN_B")
check "重复拉黑幂等" 200 "$code"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X DELETE \
  -H "Authorization: Bearer $TOKEN_B" "$GW/api/user/$UID_A/block")
check "取消拉黑" 200 "$code"
code=$(post "/api/dm/send" "{\"to_user_id\":$UID_B,\"content\":\"恢复了\"}" "$TOKEN_A")
check "取消拉黑后可发" 200 "$code"

echo "[8/8] 并发发送 + 页面"
CONC_PIDS=()
for i in $(seq 1 10); do
  (
    for j in $(seq 1 5); do
      curl -s --max-time 5 -o /dev/null -X POST \
        -H 'Content-Type: application/json' \
        -H "Authorization: Bearer $TOKEN_A" \
        -d "{\"to_user_id\":$UID_B,\"content\":\"并发 $i-$j\"}" \
        "$GW/api/dm/send"
    done
  ) &
  CONC_PIDS+=($!)
done
wait "${CONC_PIDS[@]}"
code=$(get "/api/dm/$CONV/messages?after=0&limit=100" "$TOKEN_A")
check "并发后全量拉取" 200 "$code"
expect_eq "并发消息总数（3+1+50）" 54 "$(grep -o '"sender_id":' "$TMP/body" | wc -l)"
expect_eq "并发消息无重复 id" 54 \
  "$(grep -o '"id":[0-9]*' "$TMP/body" | sort -u | wc -l)"
code=$(curl -s --max-time 5 -o "$TMP/page" -w '%{http_code}' "$GW/messages")
check "GET /messages 页面" 200 "$code"
for marker in 'id="convList"' 'id="chatBody"' 'id="input"' 'POLL_MS = 2000' \
  'api/dm/conversations' 'api/dm/'; do
  if grep -q "$marker" "$TMP/page"; then
    printf '  %-42s PASS  页面含 %s\n' "页面元素" "$marker"
    PASSED=$((PASSED + 1))
  else
    printf '  %-42s FAIL  页面缺少 %s\n' "页面元素" "$marker"
    FAILED=$((FAILED + 1))
  fi
done

echo "---------------------------------------------"
printf '[accept_dm] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_dm] PASS"
  exit 0
fi
echo "[accept_dm] FAIL"
exit 1
