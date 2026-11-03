#!/usr/bin/env bash
# 抖音 C2 user 服务验收：启动 user_service(29101) → 6 条 curl → PASS/FAIL。
#
# 覆盖：注册 / 登录 / 资料 / 重复注册 409 / 错误密码 401 / 不存在 404，
#       另加 3 条鉴权边界（无 token 401 / 关注自己 400 / 关注幂等）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/user_service"
PORT="${DOUYIN_PORT:-29101}"
BASE="http://127.0.0.1:$PORT"
DB="$ROOT/data/accept_user.db"
LOG="$ROOT/logs/accept_user.log"
TMP="$(mktemp -d)"
SERVICE_PID=""
PASSED=0
FAILED=0

cleanup() {
  if [[ -n "$SERVICE_PID" ]] && kill -0 "$SERVICE_PID" 2>/dev/null; then
    kill "$SERVICE_PID" 2>/dev/null || true
    wait "$SERVICE_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

check() {
  local name="$1" expected="$2" actual="$3" detail="${4:-}"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-30s PASS  HTTP %s %s\n' "$name" "$actual" "$detail"
    PASSED=$((PASSED + 1))
  else
    printf '  %-30s FAIL  HTTP %s (期望 %s) %s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

expect_body() {
  local name="$1" pattern="$2"
  if grep -q "$pattern" "$TMP/body"; then
    printf '  %-30s PASS  响应含 %s\n' "$name" "$pattern"
    PASSED=$((PASSED + 1))
  else
    printf '  %-30s FAIL  响应缺少 %s（实际：%s）\n' "$name" "$pattern" "$(head -c 200 "$TMP/body")"
    FAILED=$((FAILED + 1))
  fi
}

post() { # path json [token]
  local args=(-s -o "$TMP/body" -w '%{http_code}' -X POST
              -H 'Content-Type: application/json' -d "$2")
  if [[ -n "${3:-}" ]]; then
    args+=(-H "Authorization: Bearer $3")
  fi
  curl "${args[@]}" "$BASE$1"
}

get() { # path [token]
  local args=(-s -o "$TMP/body" -w '%{http_code}' "$BASE$1")
  if [[ -n "${2:-}" ]]; then
    args+=(-H "Authorization: Bearer $2")
  fi
  curl "${args[@]}"
}

del() { # path token
  curl -s -o "$TMP/body" -w '%{http_code}' -X DELETE \
       -H "Authorization: Bearer $2" "$BASE$1"
}

mkdir -p "$ROOT/data" "$ROOT/logs"
rm -f "$DB" "$DB-wal" "$DB-shm"

if [[ ! -x "$BIN" ]]; then
  echo "找不到 $BIN，请先跑 app/douyin/build.sh" >&2
  exit 1
fi

DOUYIN_DB="$DB" "$BIN" "$PORT" >"$LOG" 2>&1 &
SERVICE_PID=$!

ready=0
for _ in $(seq 1 100); do
  if curl -fsS "$BASE/health" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.05
done
if [[ "$ready" -ne 1 ]]; then
  echo "user_service 启动失败，日志：" >&2
  cat "$LOG" >&2
  exit 1
fi
echo "[accept_user] 服务就绪：$BASE（db=$DB，pid=$SERVICE_PID）"

echo "[1/6] 注册 alice"
code=$(post /api/user/register '{"name":"alice","email":"alice@example.com","password":"secret123"}')
check "注册 alice" 200 "$code"
TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body")
USER_ID=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
expect_body "注册响应含 ok/token" '"ok":true'
[[ -n "$TOKEN" ]] || echo "  警告：未提取到 token"

echo "[2/6] 登录 alice"
code=$(post /api/user/login '{"email":"alice@example.com","password":"secret123"}')
check "登录 alice" 200 "$code"
LOGIN_TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body")
expect_body "登录响应含 token" '"token":"'

echo "[3/6] 查资料 GET /api/user/$USER_ID"
code=$(get "/api/user/$USER_ID")
check "资料 $USER_ID" 200 "$code"
expect_body "资料含 alice" '"name":"alice"'

if grep -q 'password_hash\|"salt"' "$TMP/body"; then
  printf '  %-30s FAIL  响应泄露 password_hash/salt\n' "资料字段"
  FAILED=$((FAILED + 1))
else
  printf '  %-30s PASS  响应不含 password_hash/salt\n' "资料字段"
  PASSED=$((PASSED + 1))
fi

echo "[4/6] 重复注册同 email"
code=$(post /api/user/register '{"name":"alice2","email":"alice@example.com","password":"secret123"}')
check "重复注册 → 409" 409 "$code"
expect_body "冲突响应含 code" '"code":409'

echo "[5/6] 错误密码登录"
code=$(post /api/user/login '{"email":"alice@example.com","password":"wrong-password"}')
check "错误密码 → 401" 401 "$code"
expect_body "401 响应含 code" '"code":401'

echo "[6/6] 不存在用户"
code=$(get /api/user/99999)
check "不存在 → 404" 404 "$code"
expect_body "404 响应含 code" '"code":404'

echo "[附加] 鉴权边界"
code=$(post "/api/user/$USER_ID/follow" '{}')
check "无 token 关注 → 401" 401 "$code"
code=$(post "/api/user/$USER_ID/follow" '{}' "$TOKEN")
check "关注自己 → 400" 400 "$code"
code=$(get "/api/user/$USER_ID/following")
check "关注列表 → 200" 200 "$code"

# 造第二个用户，验证真实关注 + 幂等 + 取关。
code=$(post /api/user/register '{"name":"bob","email":"bob@example.com","password":"secret123"}')
BOB_ID=$(sed -n 's/.*"user":{"id":\([0-9]*\).*/\1/p' "$TMP/body")
BOB_TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$TMP/body")
check "注册 bob" 200 "$code"
code=$(post "/api/user/$BOB_ID/follow" '{}' "$TOKEN")
check "alice 关注 bob" 200 "$code"
code=$(post "/api/user/$BOB_ID/follow" '{}' "$TOKEN")
check "重复关注（幂等）" 200 "$code"
code=$(get "/api/user/$USER_ID/following")
check "alice 关注列表" 200 "$code"
expect_body "关注列表含 bob" "\"ids\":\\[$BOB_ID\\]"
code=$(del "/api/user/$BOB_ID/follow" "$TOKEN")
check "取关 bob" 200 "$code"
code=$(get "/api/user/$USER_ID/following")
check "取关后关注列表" 200 "$code"
expect_body "取关后列表为空" '"ids":\[\]'

# 非法 token。
code=$(post "/api/user/$BOB_ID/follow" '{}' "${TOKEN}x")
check "篡改 token → 401" 401 "$code"

echo "---------------------------------------------"
printf '[accept_user] 结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
if [[ "$FAILED" -eq 0 ]]; then
  echo "[accept_user] PASS"
  exit 0
fi
echo "[accept_user] FAIL"
exit 1
