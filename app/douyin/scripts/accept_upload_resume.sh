#!/usr/bin/env bash
# 抖音「分片上传 / 断点续传」验收：4 个新接口 + 服务重启持久化 + 异常路径。
#
# 链路：user_service(29201) 注册取 token → video_service(29202) 分片上传。
# 覆盖：
#   1) 10 MB 分 5×2 MB 完整上传 → finish → 字节级 cmp 一致；
#   2) 传 2 片后杀 video_service 重启 → status 仍为 4 MB → 续传完成；
#   3) 续传途中杀客户端（限速 + kill -9）→ status 不前进、无半片落盘；
#   4) 乱序 / 总大小不符 / 跨用户 / 非法 Content-Range → 400。
#
# 用法：scripts/accept_upload_resume.sh
# 依赖：build.sh 已产出 build/{user_service,video_service}；curl / cmp / split / dd。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
USER_PORT="${DOUYIN_UPLOAD_USER_PORT:-29201}"
VIDEO_PORT="${DOUYIN_UPLOAD_VIDEO_PORT:-29202}"
USER_BASE="http://127.0.0.1:$USER_PORT"
BASE="http://127.0.0.1:$VIDEO_PORT"
TMP="$(mktemp -d)"
DB="$TMP/upload.db"
WEB="$TMP/web"
CHUNK=2097152
TOTAL=10485760
PASSED=0
FAILED=0
USER_PID=""
VIDEO_PID=""
LAST_PID=""
CODE=""
UPLOAD_ID=""
VIDEO_URL=""
TOKEN_A=""
TOKEN_B=""

mkdir -p "$WEB/videos" "$TMP/log"

stop_pid() { # pid
  local pid="$1"
  [[ -n "$pid" ]] || return 0
  kill -0 "$pid" 2>/dev/null || return 0
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 100); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.02
  done
  kill -9 "$pid" 2>/dev/null || true
  return 0
}

cleanup() {
  stop_pid "$VIDEO_PID"
  stop_pid "$USER_PID"
  rm -rf "$TMP"
}
trap cleanup EXIT

check() { # name expected actual [detail]
  if [[ "$2" == "$3" ]]; then
    printf '  %-48s PASS  %s %s\n' "$1" "$3" "${4:-}"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  实际 %s（期望 %s）%s\n' "$1" "$3" "$2" "${4:-}"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  check "$1" "$2" "$3"
}

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" DOUYIN_WEB_DIR="$WEB" "$bin" "$port" \
    >"$TMP/log/$name.log" 2>&1 &
  LAST_PID=$!
  for _ in $(seq 1 200); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  echo "$name 启动失败，日志：" >&2
  cat "$TMP/log/$name.log" >&2
  return 1
}

post_json() { # url token payload
  local args=(-s --max-time 30 -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$3" "$1")
  [[ -n "$2" ]] && args+=(-H "Authorization: Bearer $2")
  CODE=$(curl "${args[@]}")
}

get_auth() { # url token
  local args=(-s --max-time 30 -o "$TMP/body" -w '%{http_code}' "$1")
  [[ -n "$2" ]] && args+=(-H "Authorization: Bearer $2")
  CODE=$(curl "${args[@]}")
}

send_chunk() { # token upload_id start end total file
  CODE=$(curl -s --max-time 60 -o "$TMP/body" -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $1" \
    -H "Content-Range: bytes $3-$4/$5" \
    --data-binary "@$6" \
    "$BASE/api/video/upload/chunk?upload_id=$2")
}

send_index() { # token upload_id index file
  local start=$(( $3 * CHUNK ))
  send_chunk "$1" "$2" "$start" "$((start + CHUNK - 1))" "$TOTAL" "$4"
}

json_int() { sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p" "$TMP/body" | head -1; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }

init_upload() { # token total title
  post_json "$BASE/api/video/upload/init" "$1" "{\"total\":$2,\"title\":\"$3\"}"
  UPLOAD_ID=$(json_str upload_id)
}

get_status() { # token upload_id
  get_auth "$BASE/api/video/upload/status/$2" "$1"
}

finish_upload() { # token upload_id title
  post_json "$BASE/api/video/upload/finish" "$1" \
    "{\"upload_id\":\"$2\",\"title\":\"$3\"}"
  VIDEO_URL=$(json_str url)
}

# 生成带 ftyp 魔数的指定 MB 文件，并按 2 MB 切片。
make_mp4() { # path megabytes
  dd if=/dev/urandom of="$1" bs=1M count="$2" status=none
  printf 'ftyp' | dd of="$1" bs=1 seek=4 conv=notrunc status=none
  split -b "$CHUNK" -d -a 1 "$1" "$1.part."
}

echo "[1/6] 启动 user_service:$USER_PORT / video_service:$VIDEO_PORT"
start_service user_service "$USER_PORT"
USER_PID=$LAST_PID
start_service video_service "$VIDEO_PORT"
VIDEO_PID=$LAST_PID
echo "[start] user=$USER_PID video=$VIDEO_PID db=$DB"

post_json "$USER_BASE/api/user/register" "" \
  '{"name":"alice","email":"alice@upload.test","password":"secret123"}'
check "注册 alice" 200 "$CODE"
TOKEN_A=$(json_str token)
post_json "$USER_BASE/api/user/register" "" \
  '{"name":"bob","email":"bob@upload.test","password":"secret123"}'
check "注册 bob" 200 "$CODE"
TOKEN_B=$(json_str token)
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" ]]; then
  echo "token 提取失败" >&2
  exit 1
fi

echo "[2/6] 完整上传 10 MB（5 × 2 MB）"
make_mp4 "$TMP/a.mp4" 10
init_upload "$TOKEN_A" "$TOTAL" "完整上传"
check "POST /init 返回 upload_id（UUID）" yes \
  "$([[ "$UPLOAD_ID" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$ ]] && echo yes || echo no)" \
  "$UPLOAD_ID"
UP_A="$UPLOAD_ID"
for i in 0 1 2 3 4; do
  send_index "$TOKEN_A" "$UP_A" "$i" "$TMP/a.mp4.part.$i"
  check "chunk $i 追加 → 200" 200 "$CODE"
  expect_eq "chunk $i 后 received" "$(( (i + 1) * CHUNK ))" "$(json_int received)"
done
get_status "$TOKEN_A" "$UP_A"
check "GET /status 已收 10 MB" 200 "$CODE"
expect_eq "status.received" "$TOTAL" "$(json_int received)"
expect_eq "status.ranges 单段 [0,10MB)" "{\"start\":0,\"end\":$TOTAL}" \
  "$(sed -n 's/.*"ranges":\[\([^]]*\)\].*/\1/p' "$TMP/body" | head -1)"
finish_upload "$TOKEN_A" "$UP_A" "完整上传"
check "POST /finish → 200" 200 "$CODE"
expect_eq "finish 返回 video.id>0" yes \
  "$([[ "$(json_int id)" -gt 0 ]] && echo yes || echo no)"
FINAL_A="$WEB/$VIDEO_URL"
expect_eq "正式文件大小" "$TOTAL" "$(stat -c%s "$FINAL_A" 2>/dev/null || echo missing)"
if cmp -s "$TMP/a.mp4" "$FINAL_A"; then
  expect_eq "正式文件与源文件 cmp 一致" yes yes
else
  expect_eq "正式文件与源文件 cmp 一致" yes no
fi
expect_eq "finish 后 .part 已删除" absent \
  "$([[ -e "$WEB/videos/.upload_$UP_A.part" ]] && echo present || echo absent)"

echo "[3/6] 断点续传：服务重启 + 客户端中途被杀"
make_mp4 "$TMP/b.mp4" 10
init_upload "$TOKEN_A" "$TOTAL" "断点续传"
UP_B="$UPLOAD_ID"
send_index "$TOKEN_A" "$UP_B" 0 "$TMP/b.mp4.part.0"
check "续传：chunk 0 → 200" 200 "$CODE"
send_index "$TOKEN_A" "$UP_B" 1 "$TMP/b.mp4.part.1"
check "续传：chunk 1 → 200" 200 "$CODE"
get_status "$TOKEN_A" "$UP_B"
expect_eq "2 片后 status.received" 4194304 "$(json_int received)"

stop_pid "$VIDEO_PID"
VIDEO_PID=""
start_service video_service "$VIDEO_PORT"
VIDEO_PID=$LAST_PID
check "video_service 重启后 /health" 200 \
  "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/health")"
get_status "$TOKEN_A" "$UP_B"
check "重启后 GET /status → 200（状态持久化）" 200 "$CODE"
expect_eq "重启后 received 仍为 4 MB" 4194304 "$(json_int received)"

# 模拟断网：限速上传第 3 片，0.6 s 后 kill -9 客户端。
curl -s --max-time 60 --limit-rate 150k -o /dev/null -X POST \
  -H "Authorization: Bearer $TOKEN_A" \
  -H "Content-Range: bytes 4194304-6291455/$TOTAL" \
  --data-binary "@$TMP/b.mp4.part.2" \
  "$BASE/api/video/upload/chunk?upload_id=$UP_B" &
KILLED_PID=$!
sleep 0.6
kill -9 "$KILLED_PID" 2>/dev/null
wait "$KILLED_PID" 2>/dev/null
sleep 0.3
get_status "$TOKEN_A" "$UP_B"
expect_eq "客户端被杀后 received 不前进（无半片落盘）" 4194304 "$(json_int received)"
expect_eq "暂存文件大小仍为 4 MB" 4194304 \
  "$(stat -c%s "$WEB/videos/.upload_$UP_B.part" 2>/dev/null || echo missing)"

for i in 2 3 4; do
  send_index "$TOKEN_A" "$UP_B" "$i" "$TMP/b.mp4.part.$i"
  check "断点续传 chunk $i → 200" 200 "$CODE"
done
get_status "$TOKEN_A" "$UP_B"
expect_eq "续传完成 received" "$TOTAL" "$(json_int received)"
finish_upload "$TOKEN_A" "$UP_B" "断点续传"
check "断点续传 finish → 200" 200 "$CODE"
FINAL_B="$WEB/$VIDEO_URL"
if cmp -s "$TMP/b.mp4" "$FINAL_B"; then
  expect_eq "续传文件与源文件 cmp 一致" yes yes
else
  expect_eq "续传文件与源文件 cmp 一致" yes no
fi

echo "[4/6] 异常路径：乱序 / 总大小不符 / 跨用户 / 非法头"
init_upload "$TOKEN_A" "$TOTAL" "异常"
UP_C="$UPLOAD_ID"
send_index "$TOKEN_A" "$UP_C" 2 "$TMP/b.mp4.part.2"
check "分片乱序（先发第 3 片）→ 400" 400 "$CODE"
get_status "$TOKEN_A" "$UP_C"
expect_eq "乱序被拒后 received 仍为 0" 0 "$(json_int received)"
send_index "$TOKEN_A" "$UP_C" 0 "$TMP/b.mp4.part.0"
check "补发第 1 片 → 200" 200 "$CODE"
finish_upload "$TOKEN_A" "$UP_C" "异常"
check "总大小不符时 finish → 400" 400 "$CODE"

expect_eq "跨用户 status → 400" 400 "$(get_auth "$BASE/api/video/upload/status/$UP_C" "$TOKEN_B"; echo "$CODE")"
send_index "$TOKEN_B" "$UP_C" 1 "$TMP/b.mp4.part.1"
check "跨用户 chunk → 400" 400 "$CODE"
post_json "$BASE/api/video/upload/finish" "$TOKEN_B" \
  "{\"upload_id\":\"$UP_C\",\"title\":\"劫持\"}"
check "跨用户 finish → 400" 400 "$CODE"
get_status "$TOKEN_A" "$UP_C"
expect_eq "跨用户尝试后 alice 的进度不变" "$CHUNK" "$(json_int received)"

head -c 1048576 "$TMP/b.mp4.part.1" >"$TMP/short.bin"
send_chunk "$TOKEN_A" "$UP_C" "$CHUNK" "$((CHUNK + CHUNK - 1))" "$TOTAL" \
  "$TMP/short.bin"
check "分片长度与 Content-Range 不符 → 400" 400 "$CODE"
CODE=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $TOKEN_A" -H 'Content-Range: bytes abc' \
  --data-binary "@$TMP/b.mp4.part.0" \
  "$BASE/api/video/upload/chunk?upload_id=$UP_C")
check "非法 Content-Range → 400" 400 "$CODE"
get_status "$TOKEN_A" "not-a-uuid"
check "非 UUID upload_id → 400" 400 "$CODE"
post_json "$BASE/api/video/upload/init" "" '{"total":10485760}'
check "init 无 token → 401" 401 "$CODE"
post_json "$BASE/api/video/upload/init" "$TOKEN_A" '{"total":33554433}'
check "init total 超 32 MiB → 400" 400 "$CODE"

echo "[5/6] 库表核对"
TABLE_COUNT=$(python3 - "$DB" "$UP_C" <<'PY' 2>/dev/null || echo "no-python3"
import sqlite3
import sys

con = sqlite3.connect(sys.argv[1])
row = con.execute(
    "SELECT COUNT(*) FROM upload_sessions WHERE upload_id = ?", (sys.argv[2],)
).fetchone()
print(row[0])
PY
)
if [[ "$TABLE_COUNT" == "no-python3" ]]; then
  echo "  （无 python3，跳过表核对）"
else
  expect_eq "upload_sessions 行仍存在（未 finish）" 1 "$TABLE_COUNT"
  ROW_USER=$(python3 - "$DB" "$UP_C" <<'PY' 2>/dev/null || echo "0"
import sqlite3
import sys

con = sqlite3.connect(sys.argv[1])
row = con.execute(
    "SELECT received_bytes FROM upload_sessions WHERE upload_id = ?", (sys.argv[2],)
).fetchone()
print(row[0] if row else -1)
PY
)
  expect_eq "库内 received_bytes 与接口一致" "$CHUNK" "$ROW_USER"
fi

echo
printf '通过 %d，失败 %d\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -ne 0 ]]; then
  echo "分片上传验收失败" >&2
  exit 1
fi
echo "分片上传 / 断点续传验收全部通过"
