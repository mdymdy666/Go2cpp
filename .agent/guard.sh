#!/usr/bin/env bash
# Go2Cpp 定时任务轮次互斥锁。用法: guard.sh acquire | release
set -u
ROOT=/UserData/CodexWorkSpace/Go2Cpp
LOCK="$ROOT/.agent/run.lock"
STALE=5400   # 90 分钟视为陈旧；大于 60 分钟单轮预算，保留容差

case "${1:-}" in
  acquire)
    if mkdir "$LOCK" 2>/dev/null; then
      date +%s > "$LOCK/started_at"; echo "ACQUIRED"; exit 0
    fi
    now=$(date +%s); start=$(cat "$LOCK/started_at" 2>/dev/null || echo 0)
    age=$(( now - start ))
    if [ "$age" -lt "$STALE" ]; then echo "BUSY:$age"; exit 0; fi
    rm -rf "$LOCK" && mkdir "$LOCK" && date +%s > "$LOCK/started_at"
    echo "TOOK_OVER:$age"; exit 0 ;;
  release)
    [ -d "$LOCK" ] && rm -rf "$LOCK"; echo "RELEASED"; exit 0 ;;
  *) echo "usage: guard.sh acquire|release"; exit 2 ;;
esac
