#!/usr/bin/env bash
# 常驻启动 douyin 全部服务（10 个进程），不自动退出。
# 用法：scripts/run_all.sh           启动（已在跑且二进制未变则跳过；二进制更新则重启）
#       scripts/run_all.sh stop      停止（按端口精确杀，不依赖 pid 文件）
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

PORTS=(28080 29101 29102 29103 29104 29105 29106 29107 29108 29109)
NAMES=(gateway user_service video_service feed_service interaction_service live_service search_service admin_service creator_service dm_service)

# 查某端口上的监听进程 pid（优先 ss，回退 /proc）
port_pid() {
  local port="$1"
  local pid=""
  if command -v ss >/dev/null 2>&1; then
    pid=$(ss -ltnp 2>/dev/null | awk -v p=":$port" '$4 ~ p {print}' | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
  fi
  if [[ -z "$pid" ]]; then
    pid=$(ps -eo pid,args 2>/dev/null | grep -E "(gateway|user_service|video_service|feed_service|interaction_service|live_service|search_service|admin_service|creator_service|dm_service) $port$" | grep -v grep | awk '{print $1}' | head -1)
  fi
  printf '%s' "$pid"
}

stop_all() {
  local port pid killed=0
  for port in "${PORTS[@]}"; do
    pid=$(port_pid "$port")
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null && killed=$((killed + 1))
      printf '[stop] port=%-5s pid=%s\n' "$port" "$pid"
    fi
  done
  # 兜底：pid 文件里的残留
  if [[ -f "$PIDF" ]]; then
    while read -r name pid; do
      [[ -n "${pid:-}" ]] || continue
      kill -9 "$pid" 2>/dev/null && killed=$((killed + 1))
    done <"$PIDF"
  fi
  : >"$PIDF"
  sleep 1
  printf '[stop] 共停止 %d 个\n' "$killed"
}

if [[ "${1:-}" == "stop" ]]; then
  stop_all
  exit 0
fi

: >"$PIDF"

start() { # name port
  local name="$1" port="$2"
  local bin="$BIN/$name"
  local existing
  existing=$(port_pid "$port")
  if [[ -n "$existing" ]]; then
    # 端口在监听：比对进程 exe mtime 与二进制 mtime
    local exe="/proc/$existing/exe"
    if [[ -e "$exe" && -e "$bin" ]]; then
      local exe_m bin_m
      exe_m=$(stat -L -c %Y "$exe" 2>/dev/null || echo 0)
      bin_m=$(stat -c %Y "$bin" 2>/dev/null || echo 0)
      if (( bin_m > exe_m )); then
        printf '[restart] %-18s port=%-5s 二进制比进程新，强制重启（旧 pid=%s）\n' "$name" "$port" "$existing"
        kill -9 "$existing" 2>/dev/null
        sleep 0.5
      else
        printf '[skip] %-18s port=%-5s 已在运行且二进制未变\n' "$name" "$port"
        return 0
      fi
    else
      printf '[skip] %-18s port=%-5s 已在运行\n' "$name" "$port"
      return 0
    fi
  fi
  setsid nohup "$bin" "$port" >>"$LOG/run_${name}.log" 2>&1 &
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

echo "=== 启动 douyin 服务（DB=$DB）==="
for i in "${!PORTS[@]}"; do
  start "${NAMES[$i]}" "${PORTS[$i]}"
done

echo
echo "=== 状态 ==="
for p in "${PORTS[@]}"; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$p/health" 2>/dev/null)
  printf '  %-6s %s\n' "$p" "$code"
done
echo
echo "首页： http://127.0.0.1:28080/   直播： /live   设置： /settings"
echo "创作： /creator   消息： /messages   举报： /admin"
echo "停止： scripts/run_all.sh stop"
