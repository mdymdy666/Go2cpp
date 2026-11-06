#!/usr/bin/env bash
# 内容三件套（草稿箱 / 视频编辑 / 发布流程）验收（第 7 轮新增）。
#
# 用例分组：
#   [1] 封面接口：PNG/JPEG data URL 落盘、鉴权、格式/体积/坏 base64 拒绝
#   [2] 发布字段往返：POST /api/video 带 cover_url/clip_start_ms/clip_end_ms，
#       详情、feed、mine 三个出口都能读到
#   [3] 整包上传：带封面+裁剪上传真文件；非法裁剪/封面 400；文件落盘
#   [4] 分片上传：init → chunk → finish 带封面/裁剪，finish 返回三字段
#   [5] 幂等：同 Idempotency-Key 二次上传回放同一条视频，mine 数量不变
#   [6] 旧库迁移：手工建旧版 videos 表 → 重启 → 新列可用
#   [7] 前端：GET / 200，草稿自动保存/恢复/删除、抽帧、裁剪、进度、重试、
#       成功跳转、幂等键等关键实现均在页面里
#   [8] 性能：详情 / feed / 封面接口 TTFB < 200 ms（20 次均值）
#   [9] 回归：feed、mine、分享页 /v/:id 仍 200
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_content_cluster.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_content_cluster.pids"
DB="$ROOT/data/accept_content_cluster.db"
WEB="$ROOT/data/accept_content_cluster_web"
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

# 1x1 PNG（70 字节）
PNG_B64='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='
# 1x1 JPEG
JPG_B64='/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAF//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABBQJ//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAwEBPwF//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAgEBPwF//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQAGPwJ//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPyF//9oADAMBAAIAAwAAABD/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/EH//xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/EH//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/EH//2Q=='

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
    printf '  %-56s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-56s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_eq() { # name expected actual
  local name="$1" expected="$2" actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-56s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-56s FAIL  %s（期望 %s）\n' "$name" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

expect_contains() { # name pattern [file]
  local name="$1" pattern="$2" file="${3:-$TMP/body}"
  if grep -q -- "$pattern" "$file"; then
    printf '  %-56s PASS  含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-56s FAIL  缺少 %s\n' "$name" "$pattern"
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
    >"$LOG_DIR/accept_content_cluster_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_content_cluster_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -rf "$WEB" "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_content_cluster_"*.log
mkdir -p "$WEB/videos"

echo "[0/9] 启动 7 个进程（6 业务 + gateway）"
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

echo "[1/9] 准备用户与素材"
TOKEN_A="$(register alice alice@content.test)"
UID_A="$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)"
TOKEN_B="$(register bob bob@content.test)"
if [[ -z "$TOKEN_A" || -z "$TOKEN_B" ]]; then
  echo "注册失败" >&2
  exit 1
fi
# 假 mp4：前 12 字节含 ftyp 魔数即可通过服务端校验
printf '\x00\x00\x00\x18ftypisom\x00\x00\x02\x00isomiso2' >"$TMP/fake.mp4"
head -c 256 /dev/urandom >>"$TMP/fake.mp4"

echo "[2/9] 封面接口（/api/video/cover）"
code=$(post "/api/video/cover" "{\"data_url\":\"data:image/png;base64,$PNG_B64\"}" "$TOKEN_A")
check "PNG 封面上传" 200 "$code"
expect_eq "返回 cover_url 前缀" "videos/cover" "$(json_str cover_url | cut -c1-12)"
COVER_A="$(json_str cover_url)"
expect_eq "封面文件已落盘" yes \
  "$([[ -f "$WEB/$COVER_A" ]] && echo yes || echo no)"
expect_eq "落盘字节数一致" "$(json_int bytes)" "$(stat -c%s "$WEB/$COVER_A")"
code=$(post "/api/video/cover" "{\"data_url\":\"data:image/jpeg;base64,$JPG_B64\"}" "$TOKEN_A")
check "JPEG 封面上传" 200 "$code"
COVER_B="$(json_str cover_url)"
expect_eq "JPEG 扩展名" ".jpg" ".${COVER_B##*.}"

code=$(post "/api/video/cover" "{\"data_url\":\"data:image/png;base64,$PNG_B64\"}")
check "未登录封面上传 → 401" 401 "$code"
code=$(post "/api/video/cover" '{"data_url":"data:image/gif;base64,R0lGODlh"}' "$TOKEN_A")
check "非 jpeg/png → 400" 400 "$code"
code=$(post "/api/video/cover" "{\"data_url\":\"data:image/png;base64,!!!\"}" "$TOKEN_A")
check "坏 base64 → 400" 400 "$code"
code=$(post "/api/video/cover" '{"data_url":"data:image/png;base64,AAAA"}' "$TOKEN_A")
check "base64 不是图片 → 400" 400 "$code"
code=$(post "/api/video/cover" '{"data_url":""}' "$TOKEN_A")
check "空 data_url → 400" 400 "$code"
code=$(post "/api/video/cover" '{}' "$TOKEN_A")
check "缺 data_url → 400" 400 "$code"

echo "[3/9] 发布字段往返（POST /api/video）"
code=$(post "/api/video" \
  "{\"title\":\"带封面和裁剪\",\"url\":\"videos/a.mp4\",\"cover_url\":\"$COVER_A\",\"clip_start_ms\":1500,\"clip_end_ms\":6500}" \
  "$TOKEN_A")
check "发布带 cover/clip" 200 "$code"
V1="$(video_id)"
expect_eq "响应 cover_url 回传" "$COVER_A" "$(json_str cover_url)"
expect_eq "响应 clip_start_ms" 1500 "$(json_int clip_start_ms)"
expect_eq "响应 clip_end_ms" 6500 "$(json_int clip_end_ms)"
code=$(get "/api/video/$V1" "$TOKEN_A")
check "详情 200" 200 "$code"
expect_eq "详情 cover_url" "$COVER_A" "$(json_str cover_url)"
expect_eq "详情 clip_start_ms" 1500 "$(json_int clip_start_ms)"
expect_eq "详情 clip_end_ms" 6500 "$(json_int clip_end_ms)"
code=$(get "/api/feed?limit=20")
check "feed 200" 200 "$code"
expect_contains "feed 带 cover_url" "\"cover_url\":\"$COVER_A\""
expect_contains "feed 带 clip_end_ms" '"clip_end_ms":6500'
code=$(get "/api/video/mine?limit=20" "$TOKEN_A")
check "mine 200" 200 "$code"
expect_contains "mine 带裁剪字段" '"clip_start_ms":1500'

code=$(post "/api/video" \
  '{"title":"非法裁剪","url":"videos/b.mp4","clip_start_ms":5000,"clip_end_ms":5000}' \
  "$TOKEN_A")
check "start>=end → 400" 400 "$code"
code=$(post "/api/video" \
  '{"title":"负裁剪","url":"videos/b.mp4","clip_start_ms":-1,"clip_end_ms":1000}' \
  "$TOKEN_A")
check "负裁剪 → 400" 400 "$code"
code=$(post "/api/video" \
  '{"title":"坏封面","url":"videos/b.mp4","cover_url":"javascript:alert(1)"}' \
  "$TOKEN_A")
check "非白名单封面 → 400" 400 "$code"
code=$(post "/api/video" \
  '{"title":"超范围","url":"videos/b.mp4","clip_end_ms":999999999}' "$TOKEN_A")
check "超 24h 裁剪 → 400" 400 "$code"

echo "[4/9] 整包上传（带封面 + 裁剪）"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $TOKEN_A" -H 'Content-Type: video/mp4' \
  --data-binary "@$TMP/fake.mp4" \
  "$GW/api/video/upload?title=%E4%B8%8A%E4%BC%A0%E5%B0%81%E9%9D%A2%E8%A3%81%E5%89%AA&cover_url=$COVER_B&clip_start_ms=200&clip_end_ms=1800")
check "整包上传 200" 200 "$code"
V2="$(video_id)"
expect_eq "上传响应 cover_url" "$COVER_B" "$(json_str cover_url)"
expect_eq "上传响应 clip_start_ms" 200 "$(json_int clip_start_ms)"
expect_eq "上传响应 clip_end_ms" 1800 "$(json_int clip_end_ms)"
expect_eq "上传响应 url 指向新文件" yes \
  "$(json_str url | grep -q '^videos/upload_' && echo yes || echo no)"
code=$(get "/api/video/$V2" "$TOKEN_A")
expect_eq "上传后详情 clip_end_ms" 1800 "$(json_int clip_end_ms)"

code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $TOKEN_A" -H 'Content-Type: video/mp4' \
  --data-binary "@$TMP/fake.mp4" \
  "$GW/api/video/upload?title=x&clip_start_ms=900&clip_end_ms=900")
check "上传 start=end → 400" 400 "$code"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $TOKEN_A" -H 'Content-Type: video/mp4' \
  --data-binary "@$TMP/fake.mp4" \
  "$GW/api/video/upload?title=x&cover_url=..%2Fetc%2Fpasswd")
check "上传路径穿越封面 → 400" 400 "$code"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $TOKEN_A" -H 'Content-Type: video/mp4' \
  --data-binary "@$TMP/fake.mp4" "$GW/api/video/upload?title=&clip_start_ms=0")
check "上传空标题 → 400" 400 "$code"

echo "[5/9] 分片上传（init/chunk/finish 带封面/裁剪）"
TOTAL=$(stat -c%s "$TMP/fake.mp4")
code=$(post "/api/video/upload/init" "{\"total\":$TOTAL,\"title\":\"分片裁剪\"}" "$TOKEN_A")
check "init 200" 200 "$code"
UP_ID="$(json_str upload_id)"
code=$(curl -s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $TOKEN_A" \
  -H 'Content-Type: application/octet-stream' \
  -H "Content-Range: bytes 0-$((TOTAL - 1))/$TOTAL" \
  --data-binary "@$TMP/fake.mp4" \
  "$GW/api/video/upload/chunk?upload_id=$UP_ID")
check "chunk 200" 200 "$code"
code=$(post "/api/video/upload/finish" \
  "{\"upload_id\":\"$UP_ID\",\"title\":\"分片裁剪\",\"cover_url\":\"$COVER_A\",\"clip_start_ms\":100,\"clip_end_ms\":900}" \
  "$TOKEN_A")
check "finish 200" 200 "$code"
V3="$(video_id)"
expect_eq "finish cover_url" "$COVER_A" "$(json_str cover_url)"
expect_eq "finish clip_start_ms" 100 "$(json_int clip_start_ms)"
expect_eq "finish clip_end_ms" 900 "$(json_int clip_end_ms)"

echo "[6/9] 幂等（同 Idempotency-Key 不重复建视频）"
KEY="idem-content-$$-$RANDOM"
curl -s --max-time 5 -o "$TMP/body" -D "$TMP/h1" -X POST \
  -H "Authorization: Bearer $TOKEN_A" -H 'Content-Type: video/mp4' \
  -H "Idempotency-Key: $KEY" --data-binary "@$TMP/fake.mp4" \
  "$GW/api/video/upload?title=%E5%B9%82%E7%AD%89%E6%B5%8B%E8%AF%95" >/dev/null
ID1="$(video_id)"
# 基数必须取在「第一次上传之后」：第二次上传若回放，数量不应再变。
get "/api/video/mine?limit=100" "$TOKEN_A" >/dev/null
BEFORE=$(items_count)
curl -s --max-time 5 -o "$TMP/body" -D "$TMP/h2" -X POST \
  -H "Authorization: Bearer $TOKEN_A" -H 'Content-Type: video/mp4' \
  -H "Idempotency-Key: $KEY" --data-binary "@$TMP/fake.mp4" \
  "$GW/api/video/upload?title=%E5%B9%82%E7%AD%89%E6%B5%8B%E8%AF%95" >/dev/null
ID2="$(video_id)"
check "二次上传返回同一视频 id" "$ID1" "$ID2"
expect_contains "二次上传标记 Replayed" "Idempotency-Replayed: true" "$TMP/h2"
get "/api/video/mine?limit=100" "$TOKEN_A" >/dev/null
AFTER=$(items_count)
check "同 key 未重复建视频" "$BEFORE" "$AFTER"

echo "[7/9] 旧库迁移（videos 缺新列 → ALTER 补齐）"
# 只重启 video_service：网关与其它服务必须保持在线，否则后续请求全部 000。
VID_PID="$(awk '$1 == "video_service" {print $2}' "$PID_FILE")"
if [[ -n "$VID_PID" ]] && kill -0 "$VID_PID" 2>/dev/null; then
  kill "$VID_PID" 2>/dev/null || true
  for _ in $(seq 1 100); do
    kill -0 "$VID_PID" 2>/dev/null || break
    sleep 0.02
  done
  kill -9 "$VID_PID" 2>/dev/null || true
  wait "$VID_PID" 2>/dev/null || true
fi
sed -i "\#^video_service $VID_PID #d" "$PID_FILE"
python3 - "$DB" <<'PY'
import sqlite3
import sys
con = sqlite3.connect(sys.argv[1])
con.execute("DROP TABLE IF EXISTS videos")
con.execute(
    "CREATE TABLE videos(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " user_id INTEGER NOT NULL, title TEXT NOT NULL, url TEXT NOT NULL,"
    " like_count INTEGER NOT NULL DEFAULT 0,"
    " comment_count INTEGER NOT NULL DEFAULT 0,"
    " created_at INTEGER NOT NULL)"
)
con.commit()
PY
start_service video_service "$VIDEO_PORT"
code=$(post "/api/video" \
  '{"title":"迁移后发布","url":"videos/m.mp4","clip_start_ms":300,"clip_end_ms":700}' \
  "$TOKEN_A")
check "旧库迁移后发布 200" 200 "$code"
V4="$(video_id)"
code=$(get "/api/video/$V4" "$TOKEN_A")
check "旧库迁移后详情 200" 200 "$code"
expect_eq "迁移后 clip_start_ms" 300 "$(json_int clip_start_ms)"
expect_eq "迁移后 clip_end_ms" 700 "$(json_int clip_end_ms)"

echo "[8/9] 前端实现检查（GET /）"
code=$(get "/")
check "GET / 200" 200 "$code"
cp "$TMP/body" "$TMP/home.html"
for pat in \
  'douyin_draft_autosave' \
  'douyin_drafts' \
  'DRAFT_DEBOUNCE_MS = 1000' \
  '有未发布的草稿，是否恢复？' \
  '继续编辑' \
  'deleteDraft' \
  'clearCurrentDraft' \
  'captureFrames' \
  'grabFrame' \
  'canvas.toDataURL' \
  'pubFrames' \
  'clip_start_ms' \
  'clip_end_ms' \
  'pubClipStart' \
  'dataset.clipStart' \
  'upload.onprogress' \
  'pubProgressFill' \
  'pub.lastError' \
  'Idempotency-Key' \
  'showProfile(state.user.id' \
  'openPublish'
do
  expect_contains "前端含 $pat" "$pat" "$TMP/home.html"
done

echo "[9/9] 性能与回归"
measure_avg() { # path [token]
  local sum=0 t
  for _ in $(seq 1 20); do
    if [[ -n "${2:-}" ]]; then
      t=$(curl -s -o /dev/null -w '%{time_total}' \
        -H "Authorization: Bearer $2" "$GW$1")
    else
      t=$(curl -s -o /dev/null -w '%{time_total}' "$GW$1")
    fi
    sum=$(awk -v a="$sum" -v b="$t" 'BEGIN{print a+b}')
  done
  awk -v s="$sum" 'BEGIN{printf "%.1f", s*1000/20}'
}
AVG_DETAIL=$(measure_avg "/api/video/$V2" "$TOKEN_A")
expect_eq "视频详情 TTFB < 200 ms（20 次均值）" yes \
  "$(awk -v v="$AVG_DETAIL" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       detail avg=%s ms\n' "$AVG_DETAIL"
AVG_FEED=$(measure_avg "/api/feed?limit=20")
expect_eq "feed TTFB < 200 ms（20 次均值）" yes \
  "$(awk -v v="$AVG_FEED" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       feed avg=%s ms\n' "$AVG_FEED"
AVG_COVER=$(measure_avg "/api/video/cover" "$TOKEN_A")
expect_eq "封面接口 TTFB < 200 ms" yes \
  "$(awk -v v="$AVG_COVER" 'BEGIN{print (v < 200) ? "yes" : "no"}')"
printf '       cover avg=%s ms\n' "$AVG_COVER"

code=$(get "/api/feed?limit=20")
check "回归 /api/feed" 200 "$code"
code=$(get "/api/video/mine?limit=10" "$TOKEN_A")
check "回归 /api/video/mine" 200 "$code"
code=$(get "/v/$V4")
check "回归分享页 /v/:id（迁移后重建的视频）" 200 "$code"

echo "---------------------------------------------"
printf '[accept_content_cluster] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_content_cluster] PASS"
  exit 0
fi
echo "[accept_content_cluster] FAIL"
exit 1
