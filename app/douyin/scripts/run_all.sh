#!/usr/bin/env bash
# 常驻启动 douyin 全部服务（10 个进程），不自动退出。
# 用法：scripts/run_all.sh           启动（若已在跑会跳过）
#       scripts/run_all.sh stop      停止
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build"
LOG="$ROOT/logs"
DB="$ROOT/data/douyin.db"
LIB="/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl"
PIDF="$LOG/run_all.pids"
mkdir -p "$LOG"

export LD_LIBRARY_PATH="$LIB:${LD_LIBRARY_PATH:-}"
export DOUYIN_DB="$DB"

# 管理员白名单：按 DB 里 admin 账号的 id 解析，而不是写死 1。
# 背景（2026-11-06 用户视角审查）：admin_service 缺省白名单是 user_id=1，
# 而 seed_cloud.sh 先注册 30 个普通用户、最后才注册 admin（id=31），
# 结果 admin@douyin.local 进 /admin 被 403，反倒是首个普通用户 user0001
# 拿到了举报后台的全部权限——既不能用也越权。
admin_ids() {
  python3 - "$DB" <<'PY' 2>/dev/null || printf '1'
import sqlite3
import sys
try:
    con = sqlite3.connect(sys.argv[1])
    row = con.execute(
        "select id from users where email = 'admin@douyin.local' limit 1"
    ).fetchone()
    print(row[0] if row else 1)
except Exception:
    print(1)
PY
}
export DOUYIN_ADMIN_USER_IDS="${DOUYIN_ADMIN_USER_IDS:-$(admin_ids)}"

stop_all() {
  [[ -f "$PIDF" ]] || return 0
  while read -r name pid; do
    [[ -n "${name:-}" && -n "${pid:-}" ]] || continue
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      printf '[stop] %s pid=%s\n' "$name" "$pid"
    fi
  done <"$PIDF"
  : >"$PIDF"
  sleep 1
}

if [[ "${1:-}" == "stop" ]]; then
  stop_all
  exit 0
fi

: >"$PIDF"
start() { # name port
  local name="$1" port="$2"
  if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
    printf '[skip] %-18s port=%-5s 已在运行\n' "$name" "$port"
    return 0
  fi
  setsid nohup "$BIN/$name" "$port" >>"$LOG/run_${name}.log" 2>&1 &
  local pid=$!
  echo "$name $pid" >>"$PIDF"
  for _ in $(seq 1 100); do
    curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1 && break
    sleep 0.1
  done
  if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
    printf '[start] %-18s port=%-5s pid=%s\n' "$name" "$port" "$pid"
  else
    printf '[FAIL] %-18s port=%-5s 启动失败，见 %s/run_%s.log\n' "$name" "$port" "$LOG" "$name"
  fi
}

# 素材自检：仓库 .gitignore 忽略 *.mp4，demo1-5.mp4 必须由 orig 现场生成；
# 缺了就补，避免 seed 出来的记录指向不存在的文件（2026-11-06 事故）。
if ! bash "$ROOT/scripts/gen_demo_videos.sh" -q; then
  echo "[warn] demo 视频生成失败：首页会出现无法播放的条目（见 scripts/gen_demo_videos.sh）"
fi

echo "=== 启动 douyin 服务（DB=$DB）==="
start user_service        29101
start video_service       29102
start feed_service        29103
start interaction_service 29104
start live_service        29105
start search_service      29106
start admin_service       29107
start creator_service     29108
start dm_service          29109
start gateway             28080

echo
echo "=== 状态 ==="
for p in 28080 29101 29102 29103 29104 29105 29106 29107 29108 29109; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$p/health" 2>/dev/null)
  printf '  %-6s %s\n' "$p" "$code"
done
echo
echo "首页： http://127.0.0.1:28080/"
echo "直播： http://127.0.0.1:28080/live"
echo "设置： http://127.0.0.1:28080/settings"
echo "创作： http://127.0.0.1:28080/creator"
echo "消息： http://127.0.0.1:28080/messages"
echo "举报： http://127.0.0.1:28080/admin"
echo
echo "停止： scripts/run_all.sh stop（或 kill \$(cat $PIDF | awk '{print \$2}')）"
