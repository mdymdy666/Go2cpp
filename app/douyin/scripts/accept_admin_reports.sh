#!/usr/bin/env bash
# 举报后台（运营端）验收：启动 user / video / interaction / admin / gateway 五个进程，
# 全部经网关访问；只验证「举报处理」这一条链路。
#
# 用例分组：
#   [1] 权限：无 token 401、非 admin 403（列表与 handle）、服务直连同样校验
#   [2] 列表：pending 全量、limit=2 两页游标、next_cursor 收尾、非法 status/cursor 400
#   [3] 处理：dismiss → handled、重复 handle 409、非法 action 400、不存在 404
#   [4] 删评论：delete_target → 评论不可见、视频 comment_count 归零
#   [5] 删视频：delete_target → 视频详情 404
#   [6] 用户类型：delete_target 400，dismiss 可用
#   [7] 统计：pending=0 / handled=4 / today=4
#   [8] 页面：GET /admin 200，含标题与「忽略」「删除内容」按钮
#
# 默认端口与 accept_all.sh 错开（28090 / 29111 / 29112 / 29114 / 29117），
# 避免并行跑两个验收时互相抢占。进程管理：pid 写入 logs/*.pids，退出时按
# /proc/<pid>/exe 校验后精确 kill，不使用 pkill -f。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_admin_reports.pids"
DB="$ROOT/data/accept_admin_reports.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0

GW_PORT="${DOUYIN_GATEWAY_PORT:-28090}"
USER_PORT="${DOUYIN_USER_PORT:-29111}"
VIDEO_PORT="${DOUYIN_VIDEO_PORT:-29112}"
INTERACTION_PORT="${DOUYIN_INTERACTION_PORT:-29114}"
ADMIN_PORT="${DOUYIN_ADMIN_PORT:-29117}"
GW="http://127.0.0.1:$GW_PORT"
ADMIN="http://127.0.0.1:$ADMIN_PORT"

export DOUYIN_USER_PORT="$USER_PORT"
export DOUYIN_VIDEO_PORT="$VIDEO_PORT"
export DOUYIN_INTERACTION_PORT="$INTERACTION_PORT"
export DOUYIN_ADMIN_PORT="$ADMIN_PORT"

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

expect_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-48s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-48s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" \
      "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

not_body() { # name pattern
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-48s FAIL  响应仍含 %s\n' "$name" "$pattern"
    FAILED=$((FAILED + 1))
  else
    printf '  %-48s PASS  响应不含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
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

raw_get() { # url [token]
  local url="$1" token="${2:-}"
  local args=(-s --max-time 5 -o "$TMP/body" -w '%{http_code}' "$url")
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}"
}

json_int() { grep -o "\"$1\":[0-9]*" "$TMP/body" | head -1 | cut -d: -f2; }
json_str() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$TMP/body" | head -1; }
json_bool() { grep -o "\"$1\":\(true\|false\)" "$TMP/body" | head -1 | cut -d: -f2; }
reports_count() { grep -o '"target_type":' "$TMP/body" | wc -l | tr -d ' '; }
video_id() { sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }
comment_id() { sed -n 's/.*"comment":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }
report_id() { sed -n 's/.*"report":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1; }

start_service() { # name port
  local name="$1" port="$2"
  local bin="$BIN_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "找不到 $bin，请先跑 app/douyin/build.sh" >&2
    exit 1
  fi
  DOUYIN_DB="$DB" "$bin" "$port" >"$LOG_DIR/accept_admin_reports_$name.log" 2>&1 &
  local pid=$!
  echo "$name $pid $port" >>"$PID_FILE"
  local ready=0
  for _ in $(seq 1 200); do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "$name 启动即退出，日志：" >&2
      cat "$LOG_DIR/accept_admin_reports_$name.log" >&2
      exit 1
    fi
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "$name 启动失败，日志：" >&2
    cat "$LOG_DIR/accept_admin_reports_$name.log" >&2
    exit 1
  fi
  printf '[start] %-18s port=%-5s pid=%-6s db=%s\n' "$name" "$port" "$pid" "$DB"
}

# 启动前确认端口空闲：否则 ready 探测可能命中别人的旧实例，跑出假结果。
for port in "$GW_PORT" "$USER_PORT" "$VIDEO_PORT" "$INTERACTION_PORT" "$ADMIN_PORT"; do
  if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
    exec 3>&- 2>/dev/null || true
    echo "端口 $port 已被占用，请先停止旧进程（或换端口重跑）" >&2
    exit 1
  fi
done

rm -f "$DB" "$DB-wal" "$DB-shm" "$LOG_DIR/accept_admin_reports_"*.log

echo "[1/8] 启动 user / video / interaction / gateway"
start_service user_service "$USER_PORT"
start_service video_service "$VIDEO_PORT"
start_service interaction_service "$INTERACTION_PORT"
start_service gateway "$GW_PORT"

echo "[2/8] 注册 alice（id=1，admin）/ bob（id=2），随后启动 admin"
code=$(post "/api/user/register" \
  '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
TOKEN_A=$(json_str token)
UID_A=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body" | head -1)
code=$(post "/api/user/register" \
  '{"name":"bob","email":"bob@example.com","password":"secret456"}')
check "注册 bob" 200 "$code"
TOKEN_B=$(json_str token)
expect_eq "alice 是首个用户（admin 缺省 id=1）" 1 "$UID_A"
export DOUYIN_ADMIN_USER_IDS="$UID_A"
start_service admin_service "$ADMIN_PORT"

echo "[3/8] 造数据：2 条视频 + 1 条评论 + 4 条举报"
code=$(post "/api/video" '{"title":"广告视频","url":"http://example.com/a.mp4"}' "$TOKEN_A")
check "alice 发视频 v1" 200 "$code"
V1=$(video_id)
code=$(post "/api/video" '{"title":"待删视频","url":"http://example.com/b.mp4"}' "$TOKEN_A")
check "alice 发视频 v2" 200 "$code"
V2=$(video_id)
code=$(post "/api/comment" "{\"video_id\":$V1,\"content\":\"违规评论内容\"}" "$TOKEN_A")
check "alice 评论 v1" 200 "$code"
C1=$(comment_id)
code=$(post "/api/report" "{\"target_type\":\"video\",\"target_id\":$V1,\"reason\":\"垃圾广告\"}" "$TOKEN_B")
check "举报视频 v1" 201 "$code"
R1=$(report_id)
code=$(post "/api/report" "{\"target_type\":\"video\",\"target_id\":$V2,\"reason\":\"侵权\"}" "$TOKEN_B")
check "举报视频 v2" 201 "$code"
R2=$(report_id)
code=$(post "/api/report" "{\"target_type\":\"comment\",\"target_id\":$C1,\"reason\":\"辱骂\"}" "$TOKEN_B")
check "举报评论 c1" 201 "$code"
R3=$(report_id)
code=$(post "/api/report" "{\"target_type\":\"user\",\"target_id\":$UID_A,\"reason\":\"冒充官方\"}" "$TOKEN_B")
check "举报用户 alice" 201 "$code"
R4=$(report_id)

echo "[4/8] 权限：无 token 401 / 非 admin 403（网关 + 服务直连）"
code=$(get "/api/admin/reports")
check "网关：无 token 列表 → 401" 401 "$code"
code=$(get "/api/admin/reports" "$TOKEN_B")
check "网关：bob（非 admin）列表 → 403" 403 "$code"
code=$(post "/api/admin/reports/$R1/handle" '{"action":"dismiss"}' "$TOKEN_B")
check "网关：bob（非 admin）处理 → 403" 403 "$code"
code=$(raw_get "$ADMIN/api/admin/reports")
check "直连 admin：无 token → 401" 401 "$code"
code=$(raw_get "$ADMIN/api/admin/reports" "$TOKEN_B")
check "直连 admin：非 admin → 403" 403 "$code"
code=$(raw_get "$ADMIN/api/admin/reports" "$TOKEN_A")
check "直连 admin：admin → 200" 200 "$code"

echo "[5/8] 列表：全量 / 分页 / 非法参数"
code=$(get "/api/admin/reports?status=pending" "$TOKEN_A")
check "admin 列表 pending → 200" 200 "$code"
expect_eq "pending 共 4 条" 4 "$(reports_count)"
expect_body "列表含举报人 bob" '"reporter_name":"bob"'
expect_body "列表含视频摘要" '"target_summary":"广告视频"'
expect_body "列表含评论摘要" '"target_summary":"违规评论内容"'
expect_body "列表含原因" '"reason":"垃圾广告"'
code=$(get "/api/admin/reports?status=pending&limit=2" "$TOKEN_A")
check "第一页 limit=2 → 200" 200 "$code"
expect_eq "第一页 2 条" 2 "$(reports_count)"
CURSOR=$(json_str next_cursor)
if [[ -n "$CURSOR" ]]; then
  printf '  %-48s PASS  cursor=%s\n' "第一页 next_cursor 非空" "$CURSOR"
  PASSED=$((PASSED + 1))
else
  printf '  %-48s FAIL  next_cursor 为空\n' "第一页 next_cursor 非空"
  FAILED=$((FAILED + 1))
fi
code=$(get "/api/admin/reports?status=pending&limit=2&cursor=$CURSOR" "$TOKEN_A")
check "第二页 → 200" 200 "$code"
expect_eq "第二页 2 条" 2 "$(reports_count)"
expect_eq "第二页 next_cursor 为空" "" "$(json_str next_cursor)"
code=$(get "/api/admin/reports?status=bogus" "$TOKEN_A")
check "非法 status → 400" 400 "$code"
code=$(get "/api/admin/reports?cursor=abc" "$TOKEN_A")
check "非法 cursor → 400" 400 "$code"

echo "[6/8] 处理：dismiss / 重复 / 非法 action / 不存在"
code=$(post "/api/admin/reports/$R1/handle" '{"action":"bogus"}' "$TOKEN_A")
check "非法 action → 400" 400 "$code"
code=$(post "/api/admin/reports/99999/handle" '{"action":"dismiss"}' "$TOKEN_A")
check "不存在的举报 → 404" 404 "$code"
code=$(post "/api/admin/reports/$R1/handle" '{"action":"dismiss"}' "$TOKEN_A")
check "dismiss 举报 v1 → 200" 200 "$code"
expect_body "响应 status=handled" '"status":"handled"'
expect_eq "dismiss 不删内容" false "$(json_bool deleted)"
code=$(post "/api/admin/reports/$R1/handle" '{"action":"dismiss"}' "$TOKEN_A")
check "重复处理 → 409" 409 "$code"

echo "[7/8] 删除内容：评论 / 视频 / 用户类型"
code=$(post "/api/admin/reports/$R3/handle" '{"action":"delete_target"}' "$TOKEN_A")
check "删评论 c1 → 200" 200 "$code"
expect_eq "评论已删除 deleted=true" true "$(json_bool deleted)"
code=$(get "/api/comment/$V1")
check "评论列表仍可读（视频存在）" 200 "$code"
not_body "评论列表不再含违规评论" '违规评论内容'
code=$(get "/api/video/$V1")
check "视频 v1 详情 → 200" 200 "$code"
expect_eq "评论删除后 comment_count=0" 0 "$(json_int comment_count)"
code=$(post "/api/admin/reports/$R2/handle" '{"action":"delete_target"}' "$TOKEN_A")
check "删视频 v2 → 200" 200 "$code"
expect_eq "视频已删除 deleted=true" true "$(json_bool deleted)"
code=$(get "/api/video/$V2")
check "被删视频详情 → 404" 404 "$code"
code=$(post "/api/admin/reports/$R4/handle" '{"action":"delete_target"}' "$TOKEN_A")
check "用户类型 delete_target → 400" 400 "$code"
code=$(post "/api/admin/reports/$R4/handle" '{"action":"dismiss"}' "$TOKEN_A")
check "用户类型 dismiss → 200" 200 "$code"

echo "[8/8] 统计 / 页面"
code=$(get "/api/admin/stats" "$TOKEN_A")
check "stats → 200" 200 "$code"
expect_eq "待处理 0" 0 "$(json_int pending)"
expect_eq "已处理 4" 4 "$(json_int handled)"
expect_eq "今日新增 4" 4 "$(json_int today)"
code=$(get "/admin")
check "GET /admin 页面 → 200" 200 "$code"
expect_body "页面标题" '举报后台'
expect_body "忽略按钮" '忽略'
expect_body "删除内容按钮" '删除内容'
expect_body "页面调用举报列表接口" '/api/admin/reports'
expect_body "页面调用 stats 接口" '/api/admin/stats'

printf '[accept_admin_reports] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_admin_reports] PASS"
  exit 0
fi
echo "[accept_admin_reports] FAIL"
exit 1
