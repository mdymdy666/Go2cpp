#!/usr/bin/env bash
# douyin UI E2E 一键跑：起服务（复用 run_all.sh）→ Playwright E2E → 停服务 → 摘要。
#
# 用法：bash scripts/run_ui_tests.sh
# 退出码：pytest 的退出码（0 = 全绿）；服务无论成败都会在 EXIT 时停掉。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# admin@douyin.local 的 id 是 31（首个注册用户 id=1 是 user0001）。管理端默认只认
# id 1，这里显式加上 31，保证「admin 能看列表/处理举报」用例走真实权限。
export DOUYIN_ADMIN_USER_IDS="${DOUYIN_ADMIN_USER_IDS:-1,31}"

LOG_FILE="/tmp/douyin_ui_e2e.log"

cleanup() {
  echo "=== 停止服务 ==="
  bash scripts/run_all.sh stop >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "=== 停止旧服务（确保环境变量生效）==="
bash scripts/run_all.sh stop >/dev/null 2>&1 || true

echo "=== 启动服务（10 进程，端口 28080 + 29101-29109）==="
if ! bash scripts/run_all.sh; then
  echo "[FAIL] 服务启动失败，见 logs/run_*.log" >&2
  exit 1
fi

echo "=== 等待网关就绪 ==="
ready=0
for _ in $(seq 1 60); do
  if curl -fsS --max-time 1 "http://127.0.0.1:28080/health" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.5
done
if [[ "$ready" -ne 1 ]]; then
  echo "[FAIL] 网关 10 秒内未就绪" >&2
  exit 1
fi

echo "=== 运行 UI E2E（Playwright / Chromium）==="
PYTHONPATH="$ROOT/tests/ui:${PYTHONPATH:-}" \
  python3 -m pytest tests/ui -q --tb=short 2>&1 | tee "$LOG_FILE"
status=${PIPESTATUS[0]}

passed=$(grep -oE '[0-9]+ passed' "$LOG_FILE" | tail -1 | grep -oE '[0-9]+')
failed=$(grep -oE '[0-9]+ failed' "$LOG_FILE" | tail -1 | grep -oE '[0-9]+')
passed=${passed:-0}
failed=${failed:-0}

echo
echo "=== UI E2E 摘要 ==="
echo "  passed: $passed"
echo "  failed: $failed"
echo "  日志：  $LOG_FILE"
if [[ "$status" -ne 0 ]]; then
  echo "  结果：  FAILED"
else
  echo "  结果：  PASSED（≥30 用例全绿）"
fi
exit "$status"
