#!/usr/bin/env bash
# 幂等中间件验收：同 key / 不同 key / 无 key / TTL 过期 四组真实 HTTP 测试。
#
# 链路：起 user/video/interaction 三个真实进程（独立测试库
# data/idempotency_test.db，不碰生产 data/douyin.db），注册 + 发视频后
# 用 curl 直接打 interaction_service，最后用 sqlite 断言评论条数。
#
# 用法：scripts/idempotency_test.sh
# 退出码：0=全部通过；1=有失败或启动失败。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build"
DB="$ROOT/data/idempotency_test.db"
LOG="$ROOT/logs/idem_test_interaction.log"
TMP="$(mktemp -d)"
USER_PORT=29201
VIDEO_PORT=29202
INTER_PORT=29204
PIDS=()
PASS=0
FAIL=0
TOKEN=""
V1=""
INTER_PID=""

cleanup() {
  local pid
  for pid in "${PIDS[@]:-}"; do
    [[ -n "$pid" ]] || continue
    kill "$pid" 2>/dev/null || true
  done
  rm -rf "$TMP"
}
trap cleanup EXIT

start_service() { # name port [ENV=VAL ...]
  local name="$1" port="$2"
  shift 2
  env "$@" DOUYIN_DB="$DB" "$BIN/$name" "$port" >>"$LOG" 2>&1 &
  local pid=$!
  PIDS+=("$pid")
  if [[ "$name" == "interaction_service" ]]; then
    INTER_PID="$pid"
  fi
  local ready=0
  for _ in $(seq 1 200); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "启动 $name 失败，日志尾部：" >&2
    tail -10 "$LOG" >&2
    exit 1
  fi
}

stop_interaction() {
  [[ -n "$INTER_PID" ]] || return 0
  kill "$INTER_PID" 2>/dev/null || true
  for _ in $(seq 1 100); do
    kill -0 "$INTER_PID" 2>/dev/null || return 0
    sleep 0.02
  done
  kill -9 "$INTER_PID" 2>/dev/null || true
}

check() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-46s PASS  %s\n' "$name" "$actual"
    PASS=$((PASS + 1))
  else
    printf '  %-46s FAIL  实际=%s 期望=%s\n' "$name" "$actual" "$expected"
    FAIL=$((FAIL + 1))
  fi
}

comment_id() {
  sed -n 's/.*"comment":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1
}

# post_comment CONTENT KEY [TOKEN]
post_comment() {
  local content="$1" key="${2:-}" token="${3:-$TOKEN}"
  local args=(-s --max-time 5 -D "$TMP/headers" -o "$TMP/body" -w '%{http_code}'
              -X POST -H 'Content-Type: application/json'
              -d "{\"video_id\":$V1,\"content\":\"$content\"}"
              -H "Authorization: Bearer $token"
              "http://127.0.0.1:$INTER_PORT/api/comment")
  if [[ -n "$key" ]]; then
    args+=(-H "Idempotency-Key: $key")
  fi
  curl "${args[@]}"
}

replayed_header() { # 输出 yes/no
  if grep -qi '^Idempotency-Replayed: true' "$TMP/headers"; then
    echo yes
  else
    echo no
  fi
}

db_counts() { # 输出两行：comments 总数 / idempotency_keys 总数
  python3 - "$DB" <<'PY'
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
print(db.execute("SELECT COUNT(*) FROM comments").fetchone()[0])
print(db.execute("SELECT COUNT(*) FROM idempotency_keys").fetchone()[0])
PY
}

rm -f "$DB" "$DB-wal" "$DB-shm"
: >"$LOG"
mkdir -p "$ROOT/data" "$ROOT/logs"

echo "[setup] 启动 user/video/interaction（TTL=86400）"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service interaction_service "$INTER_PORT" DOUYIN_IDEMPOTENCY_TTL_SECONDS=86400

echo "[setup] 注册 alice + 发一条视频"
curl -s --max-time 5 -o "$TMP/body" -X POST -H 'Content-Type: application/json' \
  -d '{"name":"alice","email":"alice@example.com","password":"secret123"}' \
  "http://127.0.0.1:$USER_PORT/api/user/register" >/dev/null
TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body" | head -1)
curl -s --max-time 5 -o "$TMP/body" -X POST -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"title":"幂等测试视频","url":"http://example.com/idem.mp4"}' \
  "http://127.0.0.1:$VIDEO_PORT/api/video" >/dev/null
V1=$(sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
if [[ -z "$TOKEN" || -z "$V1" ]]; then
  echo "setup 失败：token=$TOKEN video=$V1" >&2
  tail -10 "$LOG" >&2
  exit 1
fi

KEY1="idem-key-$(date +%s)-a"
KEY2="idem-key-$(date +%s)-b"
KEY3="idem-key-$(date +%s)-c"

echo "[1/4] 同 key 发两次：第二次回放第一次结果"
code1=$(post_comment "同 key 评论" "$KEY1")
id1=$(comment_id)
cp "$TMP/body" "$TMP/first_body"
hdr1=$(replayed_header)
code2=$(post_comment "同 key 评论" "$KEY1")
id2=$(comment_id)
hdr2=$(replayed_header)
cp "$TMP/body" "$TMP/second_body"
check "第一次 HTTP" 200 "$code1"
check "第二次 HTTP" 200 "$code2"
check "两次 comment id 相同" "$id1" "$id2"
check "第二次带 Idempotency-Replayed" yes "$hdr2"
check "第一次不带 Idempotency-Replayed" no "$hdr1"
check "第二次响应体与第一次完全一致" same "$(cmp -s "$TMP/first_body" "$TMP/second_body" && echo same || echo diff)"
check "DB comments 条数" 1 "$(db_counts | sed -n '1p')"

echo "[2/4] 不同 key 发两次：执行两次"
code3=$(post_comment "不同 key 评论 A" "$KEY2")
id3=$(comment_id)
code4=$(post_comment "不同 key 评论 B" "$KEY3")
id4=$(comment_id)
check "第一次 HTTP" 200 "$code3"
check "第二次 HTTP" 200 "$code4"
check "两次 comment id 不同" yes "$([[ "$id3" != "$id4" ]] && echo yes || echo no)"
check "DB comments 条数（累计）" 3 "$(db_counts | sed -n '1p')"

echo "[3/4] 无 key 发两次：不保证幂等，执行两次"
code5=$(post_comment "无 key 评论 A" "")
id5=$(comment_id)
code6=$(post_comment "无 key 评论 B" "")
id6=$(comment_id)
check "第一次 HTTP" 200 "$code5"
check "第二次 HTTP" 200 "$code6"
check "两次 comment id 不同" yes "$([[ "$id5" != "$id6" ]] && echo yes || echo no)"
check "无 key 两次均无 Replayed 头" no "$(replayed_header)"
check "DB comments 条数（累计）" 5 "$(db_counts | sed -n '1p')"
check "WARN 记录了缺 key 请求" yes \
  "$(grep -q '缺少 Idempotency-Key' "$LOG" && echo yes || echo no)"

echo "[4/4] TTL 过期：TTL=1s 重启 interaction，等 2.5s 后同 key 重新执行"
stop_interaction
start_service interaction_service "$INTER_PORT" DOUYIN_IDEMPOTENCY_TTL_SECONDS=1
sleep 2.5
code7=$(post_comment "过期后同 key 重发" "$KEY1")
id7=$(comment_id)
check "过期后 HTTP" 200 "$code7"
check "过期后重新执行（id 变化）" yes "$([[ "$id7" != "$id1" ]] && echo yes || echo no)"
check "过期后不带 Replayed 头" no "$(replayed_header)"
check "DB comments 条数（累计）" 6 "$(db_counts | sed -n '1p')"
expect_id1=$(python3 - "$DB" "$KEY1" <<'PY'
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
row = db.execute("SELECT COUNT(*), MAX(created_at) FROM idempotency_keys WHERE key = ?",
                 (sys.argv[2],)).fetchone()
print(row[0])
PY
)
check "过期重执行后 key 记录仍只有 1 行" 1 "$expect_id1"

echo
echo "结果：PASS=$PASS FAIL=$FAIL"
if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi
echo "全部通过"
