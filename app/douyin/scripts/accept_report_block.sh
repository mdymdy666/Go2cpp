#!/usr/bin/env bash
# 举报 / 拉黑验收（第 3 轮新增）：启动 7 进程 → 举报去重 → 拉黑/取消拉黑 →
# feed / 视频详情 / 评论 / 搜索 / 通知 全链路过滤 → 防骚扰写接口 → 黑名单管理。
#
# 用例分组：
#   [1] 举报：首次 201、重复 200 + duplicate、自己举报自己 400、目标不存在 404、
#       非法类型/空原因 400、匿名 401、GET 405
#   [2] 拉黑接口：拉黑 200、重复 already、状态查询、黑名单列表、自己拉黑自己
#       400、目标不存在 404、匿名 401、取消拉黑幂等
#   [3] feed 过滤：拉黑后 feed 不返回该作者视频，别人视频仍返回；取消后恢复
#   [4] 视频详情：被拉黑作者详情 404，取消后 200
#   [5] 评论过滤：拉黑后评论列表无该用户评论；取消后恢复
#   [6] 搜索过滤：拉黑后搜索用户/视频都不返回；取消后恢复
#   [7] 通知过滤：被拉黑者触发的通知不展示、未读数不计；取消后恢复
#   [8] 防骚扰：被拉黑后不能评论/点赞/关注（403）
#   [9] 前端元素：GET / 200 且含举报/拉黑入口与黑名单面板
#
# 进程管理：启动时把「名字 pid 端口」写入 logs/accept_report_block.pids，
# 退出时按该文件逐个校验 /proc/<pid>/exe 后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_report_block.pids"
DB="$ROOT/data/accept_report_block.db"
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

post() { # path json [token]
  local path="$1" payload="$2" token="${3:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X POST
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

del() { # path [token]
  local path="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' -X DELETE "$GW$path")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { grep -o "\"$1\":[0-9]*" "$TMP/body" | head -1 | cut -d: -f2; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
json_bool() { grep -o "\"$1\":\(true\|false\)" "$TMP/body" | head -1 | cut -d: -f2; }
items_count() { grep -o '"blocked_id":' "$TMP/body" | wc -l | tr -d ' '; }
db_count() { # sql args...
  python3 - "$DB" "$1" "${@:2}" <<'PY' 2>/dev/null || true
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
print(con.execute(sys.argv[2], tuple(sys.argv[3:])).fetchone()[0])
PY
}

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_report_block_$name.log" 2>&1 &
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
    cat "$LOG_DIR/accept_report_block_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s\n' "$name" "$port" "$pid"
}

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_report_block_"*.log

echo "[0/9] 启动 7 个进程"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service feed_service "$FEED_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service live_service "$LIVE_PORT"
start_service search_service "$SEARCH_PORT"
start_service gateway "$GW_PORT"

echo "[1/9] 注册 alice / bob / carol，alice / bob 各发一条视频"
code=$(post "/api/user/register" \
  '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
A_TOKEN="$(json_str token)"
A_UID="$(json_int id)"
code=$(post "/api/user/register" \
  '{"name":"bob","email":"bob@example.com","password":"secret123"}')
check "注册 bob" 200 "$code"
B_TOKEN="$(json_str token)"
B_UID="$(json_int id)"
code=$(post "/api/user/register" \
  '{"name":"carol","email":"carol@example.com","password":"secret123"}')
check "注册 carol" 200 "$code"
C_TOKEN="$(json_str token)"
C_UID="$(json_int id)"
if [[ -z "$A_TOKEN" || -z "$B_TOKEN" || -z "$C_TOKEN" ]]; then
  echo "取 token 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi
code=$(post "/api/video" '{"title":"alice 的举报拉黑验收视频","url":"/videos/demo1.mp4"}' "$A_TOKEN")
check "alice 发布视频" 200 "$code"
V_A="$(json_int id)"
code=$(post "/api/video" '{"title":"bob 的举报拉黑验收视频","url":"/videos/demo2.mp4"}' "$B_TOKEN")
check "bob 发布视频" 200 "$code"
V_B="$(json_int id)"
code=$(post "/api/comment" "{\"video_id\":$V_A,\"content\":\"bob 在被拉黑前的评论\"}" "$B_TOKEN")
check "bob 评论 alice 视频" 200 "$code"
C_B="$(json_int id)"
if [[ -z "$V_A" || -z "$V_B" ]]; then
  echo "取 video id 失败，响应：$(cat "$TMP/body")" >&2
  exit 1
fi

echo "[2/9] 举报：首次 / 重复 / 自己 / 不存在 / 非法参数 / 鉴权"
code=$(post "/api/report" \
  "{\"target_type\":\"video\",\"target_id\":$V_A,\"reason\":\"广告营销\",\"detail\":\"疑似引流\"}" \
  "$C_TOKEN")
check "carol 举报 alice 的视频" 201 "$code"
R1_ID="$(json_int id)"
check "  首次举报 duplicate=false" false "$(json_bool duplicate)"
check "  返回 target_type=video" video "$(json_str target_type)"
code=$(post "/api/report" \
  "{\"target_type\":\"video\",\"target_id\":$V_A,\"reason\":\"重复一次\"}" "$C_TOKEN")
check "重复举报同一视频" 200 "$code"
check "  duplicate=true" true "$(json_bool duplicate)"
check "  返回已有举报 id（不新增）" "$R1_ID" "$(json_int id)"
code=$(post "/api/report" \
  "{\"target_type\":\"comment\",\"target_id\":$C_B,\"reason\":\"人身攻击\"}" "$C_TOKEN")
check "carol 举报 bob 的评论" 201 "$code"
code=$(post "/api/report" \
  "{\"target_type\":\"comment\",\"target_id\":$C_B,\"reason\":\"再举报一次\"}" "$C_TOKEN")
check "重复举报同一评论" 200 "$code"
check "  duplicate=true" true "$(json_bool duplicate)"
code=$(post "/api/report" \
  "{\"target_type\":\"user\",\"target_id\":$B_UID,\"reason\":\"骚扰\"}" "$C_TOKEN")
check "carol 举报 bob 用户" 201 "$code"
code=$(post "/api/report" \
  "{\"target_type\":\"user\",\"target_id\":$C_UID,\"reason\":\"举报自己\"}" "$C_TOKEN")
check "举报自己（用户）→ 400" 400 "$code"
code=$(post "/api/report" \
  "{\"target_type\":\"video\",\"target_id\":$V_A,\"reason\":\"举报自己的视频\"}" "$A_TOKEN")
check "举报自己的视频 → 400" 400 "$code"
code=$(post "/api/report" \
  '{"target_type":"video","target_id":999999,"reason":"不存在"}' "$C_TOKEN")
check "举报不存在的视频 → 404" 404 "$code"
code=$(post "/api/report" \
  "{\"target_type\":\"live\",\"target_id\":$V_A,\"reason\":\"类型非法\"}" "$C_TOKEN")
check "非法 target_type → 400" 400 "$code"
code=$(post "/api/report" \
  "{\"target_type\":\"video\",\"target_id\":$V_A,\"reason\":\"   \"}" "$C_TOKEN")
check "空 reason → 400" 400 "$code"
code=$(post "/api/report" \
  "{\"target_type\":\"video\",\"target_id\":$V_A,\"reason\":\"匿名\"}")
check "匿名举报 → 401" 401 "$code"
code=$(get "/api/report")
check "GET /api/report → 405" 405 "$code"

echo "[3/9] 拉黑接口：拉黑 / 重复 / 状态 / 列表 / 自己 / 不存在 / 鉴权"
code=$(post "/api/user/$B_UID/block" '{}' "$C_TOKEN")
check "carol 拉黑 bob" 200 "$code"
check "  blocked=true" true "$(json_bool blocked)"
check "  already=false" false "$(json_bool already)"
code=$(post "/api/user/$B_UID/block" '{}' "$C_TOKEN")
check "重复拉黑（幂等）" 200 "$code"
check "  already=true" true "$(json_bool already)"
code=$(get "/api/user/$B_UID/block" "$C_TOKEN")
check "查询拉黑状态 blocked=true" true "$(json_bool blocked)"
code=$(get "/api/user/blocks" "$C_TOKEN")
check "黑名单列表 total=1" 1 "$(json_int total)"
check "  列表 items 数" 1 "$(items_count)"
check "  被拉黑者昵称 bob" bob "$(json_str blocked_name)"
code=$(post "/api/user/$C_UID/block" '{}' "$C_TOKEN")
check "拉黑自己 → 400" 400 "$code"
code=$(post "/api/user/999999/block" '{}' "$C_TOKEN")
check "拉黑不存在用户 → 404" 404 "$code"
code=$(post "/api/user/$B_UID/block" '{}')
check "匿名拉黑 → 401" 401 "$code"
code=$(get "/api/user/blocks")
check "匿名看黑名单 → 401" 401 "$code"

echo "[4/9] feed 过滤：拉黑后 bob 视频消失、alice 视频仍在；取消后恢复"
code=$(get "/api/feed?limit=100" "$C_TOKEN")
check "carol 的 feed 200" 200 "$code"
check "  不含被拉黑的 bob 视频" 0 "$(grep -c "\"id\":$V_B," "$TMP/body" || true)"
check "  仍含 alice 视频" 1 "$(grep -c "\"id\":$V_A," "$TMP/body" || true)"
code=$(get "/api/feed?limit=100" "$A_TOKEN")
check "alice 的 feed 仍含 bob 视频（未拉黑）" 1 "$(grep -c "\"id\":$V_B," "$TMP/body" || true)"
code=$(get "/api/feed?limit=100")
check "匿名 feed 不受任何黑名单影响" 1 "$(grep -c "\"id\":$V_B," "$TMP/body" || true)"

echo "[5/9] 视频详情：被拉黑作者 404，取消后 200"
code=$(get "/api/video/$V_B" "$C_TOKEN")
check "carol 看 bob 的视频详情 → 404" 404 "$code"
code=$(get "/api/video/$V_A" "$C_TOKEN")
check "carol 看 alice 的视频详情 → 200" 200 "$code"

echo "[6/9] 评论过滤：拉黑 bob 后其评论消失；取消后恢复"
code=$(get "/api/comment/$V_A?limit=50" "$C_TOKEN")
check "carol 看评论列表 200" 200 "$code"
check "  不含 bob 的评论" 0 "$(grep -c 'bob 在被拉黑前的评论' "$TMP/body" || true)"
code=$(get "/api/comment/$V_A?limit=50" "$A_TOKEN")
check "alice 仍能看到 bob 的评论（未拉黑）" 1 "$(grep -c 'bob 在被拉黑前的评论' "$TMP/body" || true)"

echo "[7/9] 搜索过滤：拉黑后用户/视频都不返回；取消后恢复"
code=$(get "/api/search?q=bob&type=user" "$C_TOKEN")
check "carol 搜索用户 bob：items 为空" 0 "$(grep -c '"name":"bob"' "$TMP/body" || true)"
code=$(get "/api/search?q=bob&type=user" "$A_TOKEN")
check "alice 搜索用户 bob：仍能搜到" 1 "$(grep -c '"name":"bob"' "$TMP/body" || true)"
code=$(get "/api/search?q=%E6%8B%89%E9%BB%91%E9%AA%8C%E6%94%B6&type=video" "$C_TOKEN")
check "carol 搜索视频：不含 bob 的视频" 0 "$(grep -c "\"id\":$V_B," "$TMP/body" || true)"
code=$(get "/api/search?q=%E6%8B%89%E9%BB%91%E9%AA%8C%E6%94%B6&type=video" "$A_TOKEN")
check "alice 搜索视频：仍含 bob 的视频" 1 "$(grep -c "\"id\":$V_B," "$TMP/body" || true)"

echo "[8/9] 通知过滤 + 防骚扰写接口 + 取消拉黑恢复"
# alice 的通知里现在有 bob 的一条评论通知；alice 拉黑 bob 后不展示。
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "alice 未读数（bob 的评论）" 1 "$(json_int unread_count)"
code=$(post "/api/user/$B_UID/block" '{}' "$A_TOKEN")
check "alice 拉黑 bob" 200 "$code"
check "  被拉黑者触发的通知不展示" 0 "$(grep -c '"actor_name":"bob"' "$TMP/body" || true)"
code=$(get "/api/notifications/unread_count" "$A_TOKEN")
check "  未读数不计被拉黑者的通知" 0 "$(json_int unread_count)"
# 防骚扰：bob 被 alice 拉黑后，写操作全部 403。
code=$(post "/api/comment" "{\"video_id\":$V_A,\"content\":\"我还能评论吗\"}" "$B_TOKEN")
check "bob 评论 alice 视频 → 403" 403 "$code"
code=$(post "/api/like" "{\"video_id\":$V_A}" "$B_TOKEN")
check "bob 点赞 alice 视频 → 403" 403 "$code"
code=$(post "/api/user/$A_UID/follow" '{}' "$B_TOKEN")
check "bob 关注 alice → 403" 403 "$code"
code=$(get "/api/video/$V_A" "$B_TOKEN")
check "bob 看 alice 视频详情 → 404" 404 "$code"
# 取消拉黑 alice→bob 与 carol→bob，过滤恢复。
code=$(del "/api/user/$B_UID/block" "$A_TOKEN")
check "alice 取消拉黑 bob" 200 "$code"
check "  blocked=false" false "$(json_bool blocked)"
code=$(del "/api/user/$B_UID/block" "$C_TOKEN")
check "carol 取消拉黑 bob" 200 "$code"
code=$(del "/api/user/$B_UID/block" "$C_TOKEN")
check "重复取消拉黑（幂等）" 200 "$code"
check "  blocked=false" false "$(json_bool blocked)"
code=$(get "/api/user/$B_UID/block" "$C_TOKEN")
check "状态查询 blocked=false" false "$(json_bool blocked)"
code=$(get "/api/user/blocks" "$C_TOKEN")
check "黑名单已清空 total=0" 0 "$(json_int total)"
code=$(get "/api/feed?limit=100" "$C_TOKEN")
check "取消后 feed 恢复 bob 视频" 1 "$(grep -c "\"id\":$V_B," "$TMP/body" || true)"
code=$(get "/api/comment/$V_A?limit=50" "$C_TOKEN")
check "取消后评论恢复 bob 的评论" 1 "$(grep -c 'bob 在被拉黑前的评论' "$TMP/body" || true)"
code=$(get "/api/search?q=bob&type=user" "$C_TOKEN")
check "取消后搜索恢复 bob" 1 "$(grep -c '"name":"bob"' "$TMP/body" || true)"
code=$(get "/api/notifications" "$A_TOKEN")
check "取消后通知恢复 bob 的评论" 1 "$(grep -c '"actor_name":"bob"' "$TMP/body" || true)"
code=$(post "/api/comment" "{\"video_id\":$V_A,\"content\":\"取消拉黑后可以评论\"}" "$B_TOKEN")
check "取消后 bob 可以评论" 200 "$code"

echo "[9/9] 前端元素与数据库约束"
code=$(curl -s --max-time 5 -o "$TMP/page.html" -w '%{http_code}' "$GW/")
check "GET / 200" 200 "$code"
for needle in reportModal blockPanel menuSheet openReport toggleBlock blacklistBtn; do
  if grep -q "$needle" "$TMP/page.html"; then present=1; else present=0; fi
  check "页面含 $needle" 1 "$present"
done
DUP_REPORT=$(db_count \
  "SELECT COUNT(*) FROM reports WHERE reporter_id=? AND target_type='video' AND target_id=?" \
  "$C_UID" "$V_A")
if [[ "$DUP_REPORT" =~ ^[0-9]+$ ]]; then
  check "reports 唯一约束：同人同目标只有 1 行" 1 "$DUP_REPORT"
fi
DUP_BLOCK=$(db_count \
  "SELECT COUNT(*) FROM blocks WHERE user_id=? AND blocked_id=?" \
  "$A_UID" "$B_UID")
if [[ "$DUP_BLOCK" =~ ^[0-9]+$ ]]; then
  check "blocks 唯一约束：取消后为 0 行" 0 "$DUP_BLOCK"
fi

echo "---------------------------------------------"
printf '[accept_report_block] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_report_block] PASS"
  exit 0
fi
echo "[accept_report_block] FAIL"
exit 1
